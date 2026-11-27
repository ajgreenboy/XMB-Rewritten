//! Safe Rust parser for Sony CXML containers (`P3TF`, `RAFO`, and `_RAF` zlib-wrapped `RAFO`).
//!
//! Used by PS3 themes (`.p3t`) and Rich Appearance Format scenes (`.raf` / `bgimage/anim`).
//! Validates the 56-byte CXML header, 6 section slices (element tree, ID table, string table,
//! int array, float array, file data), depth-first element traversal with cycle/attribute budgets,
//! all 8 CXML attribute kinds, and bounded zlib file/container decompression.

#![forbid(unsafe_code)]

use std::collections::HashSet;
use std::io::Read;

use flate2::read::ZlibDecoder;

/// Maximum decompressed size for an outer `_RAF` / zlib container or an embedded CXML file (256 MiB).
pub const MAX_CXML_INFLATED_BYTES: usize = 256 * 1024 * 1024;
/// Maximum number of elements visited during depth-first traversal.
pub const MAX_CXML_ELEMENTS: usize = 200_000;
/// Maximum attributes per element.
pub const MAX_ATTRIBUTES_PER_ELEMENT: u32 = 4096;
/// Maximum total attributes across all elements in a container.
pub const MAX_TOTAL_ATTRIBUTES: usize = 4 * 1024 * 1024;

const K_NONE: u32 = 0xFFFF_FFFF;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CxmlContainerKind {
    P3tf = 0,
    Rafo = 1,
    CompressedRaf = 2,
}

#[derive(Debug, Clone, PartialEq)]
pub enum CxmlAttribute {
    Int(i64),
    Float(f64),
    String(String),
    Ints(Vec<i64>),
    Floats(Vec<f64>),
    File { offset: u32, size: u32 },
}

#[derive(Debug, Clone, PartialEq)]
pub struct CxmlElement {
    pub depth: i32,
    pub name: String,
    pub attributes: Vec<(String, CxmlAttribute)>,
}

#[derive(Debug, Clone)]
pub struct ParsedCxml {
    pub unpacked_bytes: Vec<u8>,
    pub container_kind: CxmlContainerKind,
    pub compressed_scene: bool,
    pub version: u32,
    pub sections: [(u32, u32); 6],
    pub elements: Vec<CxmlElement>,
}

fn be32(d: &[u8], at: usize) -> u32 {
    if at.checked_add(4).map_or(false, |end| end <= d.len()) {
        u32::from_be_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
    } else {
        K_NONE
    }
}

fn clip(d: &[u8], a: usize, b: usize) -> &[u8] {
    let start = a.min(d.len());
    let end = b.clamp(start, d.len());
    &d[start..end]
}

fn cstr(d: &[u8], at: usize) -> String {
    if at >= d.len() {
        return String::new();
    }
    let slice = &d[at..];
    let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
    String::from_utf8_lossy(&slice[..end]).into_owned()
}

/// Inflates a raw RFC 1950 zlib stream with a strict output cap (`cap` bytes).
/// Returns `None` if the stream is not valid zlib or exceeds `cap`.
pub fn inflate_bounded(data: &[u8], cap: usize) -> Option<Vec<u8>> {
    if data.is_empty() {
        return None;
    }
    let mut decoder = ZlibDecoder::new(data);
    let mut out = Vec::new();
    let mut buf = [0u8; 16384];
    loop {
        match decoder.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                if out.len().saturating_add(n) > cap {
                    return None;
                }
                out.extend_from_slice(&buf[..n]);
            }
            Err(_) => return None,
        }
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// Mirrors `p3t.cpp`'s `inflate(const QByteArray& packed)`: if `packed` has a valid 2-byte RFC 1950
/// zlib header and decompresses cleanly within `MAX_CXML_INFLATED_BYTES`, returns the inflated bytes;
/// otherwise returns `packed` unchanged.
pub fn inflate_cxml_file_or_passthrough(packed: &[u8]) -> Vec<u8> {
    if packed.len() < 2 {
        return packed.to_vec();
    }
    let b0 = packed[0] as u16;
    let b1 = packed[1] as u16;
    if (b0 & 0x0F) != 8 || ((b0 << 8) | b1) % 31 != 0 {
        return packed.to_vec();
    }
    inflate_bounded(packed, MAX_CXML_INFLATED_BYTES).unwrap_or_else(|| packed.to_vec())
}

/// Parses a `P3TF`, `RAFO`, `_RAF`, or raw-zlib `RAFO` container into its unpacked container bytes
/// and depth-first element list.
pub fn parse_cxml(data: &[u8]) -> Result<ParsedCxml, String> {
    let (raw, container_kind, compressed_scene) = if data.starts_with(b"P3TF") {
        (data.to_vec(), CxmlContainerKind::P3tf, false)
    } else if data.starts_with(b"RAFO") {
        (data.to_vec(), CxmlContainerKind::Rafo, false)
    } else if data.starts_with(b"_RAF") {
        let declared = be32(data, 4);
        if declared == K_NONE || (declared as usize) > MAX_CXML_INFLATED_BYTES {
            return Err("The RAF scene is too large to read.".to_string());
        }
        let inflated = if data.len() > 8 {
            inflate_bounded(&data[8..], MAX_CXML_INFLATED_BYTES).unwrap_or_default()
        } else {
            Vec::new()
        };
        if !inflated.starts_with(b"RAFO") {
            return Err("The RAF scene could not be decompressed.".to_string());
        }
        (inflated, CxmlContainerKind::CompressedRaf, true)
    } else {
        let unpacked = inflate_cxml_file_or_passthrough(data);
        if !unpacked.starts_with(b"RAFO") {
            return Err("This is not a PS3 theme file.".to_string());
        }
        (unpacked, CxmlContainerKind::Rafo, true)
    };

    if raw.len() < 56 {
        return Err("This is not a PS3 theme file.".to_string());
    }

    let mut h = [0u32; 13];
    for (i, slot) in h.iter_mut().enumerate() {
        *slot = be32(&raw, 4 + 4 * i);
    }

    let tree = clip(&raw, h[1] as usize, (h[1] as usize).saturating_add(h[2] as usize));
    let ids = clip(&raw, h[3] as usize, (h[3] as usize).saturating_add(h[4] as usize));
    let strings = clip(&raw, h[5] as usize, (h[5] as usize).saturating_add(h[6] as usize));
    let int_len = (h[8] - (h[8] % 4)) as usize;
    let float_len = (h[10] - (h[10] % 4)) as usize;
    let int_bytes = clip(&raw, h[7] as usize, (h[7] as usize).saturating_add(int_len));
    let float_bytes = clip(&raw, h[9] as usize, (h[9] as usize).saturating_add(float_len));

    let mut ints = Vec::with_capacity(int_bytes.len() / 4);
    let mut i = 0usize;
    while i + 4 <= int_bytes.len() {
        ints.push((be32(int_bytes, i) as i32) as i64);
        i += 4;
    }

    let mut floats = Vec::with_capacity(float_bytes.len() / 4);
    let mut j = 0usize;
    while j + 4 <= float_bytes.len() {
        floats.push(f32::from_bits(be32(float_bytes, j)) as f64);
        j += 4;
    }

    let mut elements = Vec::new();
    let mut stack: Vec<(u32, i32)> = vec![(0, 0)];
    let mut seen: HashSet<u32> = HashSet::new();
    let mut total_attributes: usize = 0;

    while let Some((off, depth)) = stack.pop() {
        let off_usize = off as usize;
        if off_usize.saturating_add(28) > tree.len()
            || seen.contains(&off)
            || seen.len() > MAX_CXML_ELEMENTS
            || total_attributes > MAX_TOTAL_ATTRIBUTES
        {
            continue;
        }
        seen.insert(off);

        let w = |idx: usize| -> u32 { be32(tree, off_usize + 4 * idx) };
        let elem_name = cstr(strings, w(0) as usize);
        let attr_count = w(1).min(MAX_ATTRIBUTES_PER_ELEMENT);
        let mut attributes: Vec<(String, CxmlAttribute)> = Vec::new();

        for attr_idx in 0..attr_count {
            total_attributes += 1;
            if total_attributes > MAX_TOTAL_ATTRIBUTES {
                break;
            }
            let a = off_usize
                .saturating_add(28)
                .saturating_add(16 * (attr_idx as usize));
            if a.saturating_add(16) > tree.len() {
                break;
            }
            let kind = be32(tree, a + 4);
            let v1 = be32(tree, a + 8);
            let v2 = be32(tree, a + 12);

            let attr_val = match kind {
                1 => CxmlAttribute::Int((v1 as i32) as i64),
                2 => CxmlAttribute::Float(f32::from_bits(v1) as f64),
                3 => CxmlAttribute::String(cstr(strings, v1 as usize)),
                4 => {
                    let b = (v1 as usize).min(ints.len());
                    let e = (v1 as usize).saturating_add(v2 as usize).min(ints.len());
                    CxmlAttribute::Ints(ints[b..e].to_vec())
                }
                5 => {
                    let b = (v1 as usize).min(floats.len());
                    let e = (v1 as usize).saturating_add(v2 as usize).min(floats.len());
                    CxmlAttribute::Floats(floats[b..e].to_vec())
                }
                6 => CxmlAttribute::File { offset: v1, size: v2 },
                7 | 8 => CxmlAttribute::String(cstr(ids, (v1 as usize).saturating_add(4))),
                _ => continue,
            };

            let key = cstr(strings, be32(tree, a) as usize);
            if let Some(existing) = attributes.iter_mut().find(|(k, _)| *k == key) {
                existing.1 = attr_val;
            } else {
                attributes.push((key, attr_val));
            }
        }

        elements.push(CxmlElement {
            depth,
            name: elem_name,
            attributes,
        });

        let next_sibling = w(4);
        let first_child = w(5);
        if next_sibling != K_NONE {
            stack.push((next_sibling, depth));
        }
        if first_child != K_NONE {
            stack.push((first_child, depth.saturating_add(1)));
        }
    }

    if elements.is_empty() {
        return Err("The theme file holds no elements.".to_string());
    }

    let sections = [
        (h[1], h[2]),
        (h[3], h[4]),
        (h[5], h[6]),
        (h[7], h[8]),
        (h[9], h[10]),
        (h[11], h[12]),
    ];

    Ok(ParsedCxml {
        unpacked_bytes: raw,
        container_kind,
        compressed_scene,
        version: h[0],
        sections,
        elements,
    })
}

/// Encodes `ParsedCxml` metadata and element tree into a compact little-endian buffer for FFI hand-off:
///   [u8 container_kind][u8 compressed_scene][u32 version LE]
///   [6 * (u32 offset LE, u32 size LE)]
///   [u32 element_count LE]
///   per element:
///     [i32 depth LE][u32 name_len LE][u8 * name_len][u32 attr_count LE]
///     per attribute:
///       [u32 key_len LE][u8 * key_len][u8 kind]
///       kind 0 (Int):    [i64 val LE]
///       kind 1 (Float):  [f64 val LE]
///       kind 2 (String): [u32 len LE][u8 * len]
///       kind 3 (Ints):   [u32 count LE][i64 * count LE]
///       kind 4 (Floats): [u32 count LE][f64 * count LE]
///       kind 5 (File):   [u32 offset LE][u32 size LE]
pub fn encode_parsed_cxml_tree(parsed: &ParsedCxml) -> Vec<u8> {
    let mut out = Vec::with_capacity(64 + parsed.elements.len() * 48);
    out.push(parsed.container_kind as u8);
    out.push(u8::from(parsed.compressed_scene));
    out.extend_from_slice(&parsed.version.to_le_bytes());
    for &(off, sz) in &parsed.sections {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&sz.to_le_bytes());
    }
    out.extend_from_slice(&(parsed.elements.len() as u32).to_le_bytes());
    for elem in &parsed.elements {
        out.extend_from_slice(&elem.depth.to_le_bytes());
        let name_bytes = elem.name.as_bytes();
        out.extend_from_slice(&(name_bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(name_bytes);
        out.extend_from_slice(&(elem.attributes.len() as u32).to_le_bytes());
        for (key, attr) in &elem.attributes {
            let key_bytes = key.as_bytes();
            out.extend_from_slice(&(key_bytes.len() as u32).to_le_bytes());
            out.extend_from_slice(key_bytes);
            match attr {
                CxmlAttribute::Int(v) => {
                    out.push(0);
                    out.extend_from_slice(&v.to_le_bytes());
                }
                CxmlAttribute::Float(v) => {
                    out.push(1);
                    out.extend_from_slice(&v.to_le_bytes());
                }
                CxmlAttribute::String(s) => {
                    out.push(2);
                    let sb = s.as_bytes();
                    out.extend_from_slice(&(sb.len() as u32).to_le_bytes());
                    out.extend_from_slice(sb);
                }
                CxmlAttribute::Ints(list) => {
                    out.push(3);
                    out.extend_from_slice(&(list.len() as u32).to_le_bytes());
                    for &item in list {
                        out.extend_from_slice(&item.to_le_bytes());
                    }
                }
                CxmlAttribute::Floats(list) => {
                    out.push(4);
                    out.extend_from_slice(&(list.len() as u32).to_le_bytes());
                    for &item in list {
                        out.extend_from_slice(&item.to_le_bytes());
                    }
                }
                CxmlAttribute::File { offset, size } => {
                    out.push(5);
                    out.extend_from_slice(&offset.to_le_bytes());
                    out.extend_from_slice(&size.to_le_bytes());
                }
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn build_minimal_p3tf() -> Vec<u8> {
        // strings: "theme\0info\0name\0selection\0"
        let strings = b"theme\0info\0name\0selection\0";
        let ids = b"\x00\x00\x00\x00TestTheme\0";
        let ints = 8i32.to_be_bytes();
        let floats = 1.5f32.to_be_bytes();
        let files = b"PAYLOAD";

        // Element 0 ("theme" at 0, 0 attrs, first_child = 28)
        // Element 1 ("info" at 28, 2 attrs: name=id(0), selection=int(8))
        let mut tree = vec![0u8; 28 + 28 + 32];
        // elem 0
        tree[0..4].copy_from_slice(&0u32.to_be_bytes()); // "theme"
        tree[4..8].copy_from_slice(&0u32.to_be_bytes()); // 0 attrs
        tree[8..12].copy_from_slice(&K_NONE.to_be_bytes());
        tree[12..16].copy_from_slice(&K_NONE.to_be_bytes());
        tree[16..20].copy_from_slice(&K_NONE.to_be_bytes());
        tree[20..24].copy_from_slice(&28u32.to_be_bytes()); // first_child = 28
        tree[24..28].copy_from_slice(&28u32.to_be_bytes());
        // elem 1 at offset 28
        tree[28..32].copy_from_slice(&6u32.to_be_bytes()); // "info"
        tree[32..36].copy_from_slice(&2u32.to_be_bytes()); // 2 attrs
        tree[36..40].copy_from_slice(&0u32.to_be_bytes());
        tree[40..44].copy_from_slice(&K_NONE.to_be_bytes());
        tree[44..48].copy_from_slice(&K_NONE.to_be_bytes());
        tree[48..52].copy_from_slice(&K_NONE.to_be_bytes());
        tree[52..56].copy_from_slice(&K_NONE.to_be_bytes());
        // attr 0: "name" (11), kind 7 (id), v1 = 0
        tree[56..60].copy_from_slice(&11u32.to_be_bytes());
        tree[60..64].copy_from_slice(&7u32.to_be_bytes());
        tree[64..68].copy_from_slice(&0u32.to_be_bytes());
        tree[68..72].copy_from_slice(&0u32.to_be_bytes());
        // attr 1: "selection" (16), kind 1 (int), v1 = 8
        tree[72..76].copy_from_slice(&16u32.to_be_bytes());
        tree[76..80].copy_from_slice(&1u32.to_be_bytes());
        tree[80..84].copy_from_slice(&8u32.to_be_bytes());
        tree[84..88].copy_from_slice(&0u32.to_be_bytes());

        let tree_off = 56u32;
        let ids_off = tree_off + tree.len() as u32;
        let strings_off = ids_off + ids.len() as u32;
        let ints_off = strings_off + strings.len() as u32;
        let floats_off = ints_off + ints.len() as u32;
        let files_off = floats_off + floats.len() as u32;

        let mut raw = Vec::new();
        raw.extend_from_slice(b"P3TF");
        raw.extend_from_slice(&272u32.to_be_bytes());
        for &(off, len) in &[
            (tree_off, tree.len() as u32),
            (ids_off, ids.len() as u32),
            (strings_off, strings.len() as u32),
            (ints_off, ints.len() as u32),
            (floats_off, floats.len() as u32),
            (files_off, files.len() as u32),
        ] {
            raw.extend_from_slice(&off.to_be_bytes());
            raw.extend_from_slice(&len.to_be_bytes());
        }
        raw.extend_from_slice(&tree);
        raw.extend_from_slice(ids);
        raw.extend_from_slice(strings);
        raw.extend_from_slice(&ints);
        raw.extend_from_slice(&floats);
        raw.extend_from_slice(files);
        raw
    }

    #[test]
    fn parses_p3tf_and_compressed_raf() {
        let p3tf = build_minimal_p3tf();
        let parsed = parse_cxml(&p3tf).expect("valid P3TF should parse");
        assert_eq!(parsed.container_kind, CxmlContainerKind::P3tf);
        assert!(!parsed.compressed_scene);
        assert_eq!(parsed.version, 272);
        assert_eq!(parsed.elements.len(), 2);
        assert_eq!(parsed.elements[0].name, "theme");
        assert_eq!(parsed.elements[1].name, "info");
        assert_eq!(parsed.elements[1].depth, 1);
        assert_eq!(
            parsed.elements[1].attributes[0],
            ("name".to_string(), CxmlAttribute::String("TestTheme".to_string()))
        );
        assert_eq!(
            parsed.elements[1].attributes[1],
            ("selection".to_string(), CxmlAttribute::Int(8))
        );

        // Wrap as _RAF + zlib(RAFO)
        let mut rafo = p3tf;
        rafo[0..4].copy_from_slice(b"RAFO");
        let mut enc = ZlibEncoder::new(Vec::new(), Compression::default());
        enc.write_all(&rafo).unwrap();
        let z = enc.finish().unwrap();
        let mut raf = Vec::new();
        raf.extend_from_slice(b"_RAF");
        raf.extend_from_slice(&(rafo.len() as u32).to_be_bytes());
        raf.extend_from_slice(&z);

        let parsed_raf = parse_cxml(&raf).expect("valid _RAF should parse");
        assert_eq!(parsed_raf.container_kind, CxmlContainerKind::CompressedRaf);
        assert!(parsed_raf.compressed_scene);
        assert_eq!(parsed_raf.elements.len(), 2);
    }

    #[test]
    fn rejects_non_cxml_data() {
        let err = parse_cxml(b"NOT_A_THEME_FILE").unwrap_err();
        assert_eq!(err, "This is not a PS3 theme file.");
    }
}

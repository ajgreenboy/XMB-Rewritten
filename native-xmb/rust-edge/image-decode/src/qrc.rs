//! Safe Rust parser and bounded-zlib unpacker for Sony CXML QRC (`QRCC`/`QRCF` v0x110) containers
//! (`QrcArchive` in `native-xmb/src/qrc.cpp`).
//!
//! Zero `unsafe`.

#![forbid(unsafe_code)]

use std::collections::HashSet;
use std::io::Read;

use flate2::read::ZlibDecoder;

const MAX_QRC_EXPANSION: u32 = 32 * 1024 * 1024;
const MAX_NODE_ATTRIBUTES: u32 = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QrcEntry {
    pub id: String,
    pub offset: u32,
    pub size: u32,
    pub expanded: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedQrc {
    pub unpacked_bytes: Vec<u8>,
    pub data_base: u32,
    pub entries: Vec<QrcEntry>,
}

fn check_range(bytes: &[u8], offset: u64, size: u64) -> Result<(), String> {
    let total = bytes.len() as u64;
    if offset > total || size > total - offset {
        return Err("QRC range exceeds container".to_string());
    }
    Ok(())
}

fn word(bytes: &[u8], offset: u64) -> Result<u32, String> {
    check_range(bytes, offset, 4)?;
    let idx = offset as usize;
    Ok(u32::from_be_bytes([bytes[idx], bytes[idx + 1], bytes[idx + 2], bytes[idx + 3]]))
}

pub fn inflate_qrc_slice(slice: &[u8], expanded: u32) -> Result<Vec<u8>, String> {
    if expanded == 0 || expanded > MAX_QRC_EXPANSION {
        return Err("QRC expansion exceeds 32 MiB".to_string());
    }
    let cap = (expanded as usize) + 1;
    let mut decoder = ZlibDecoder::new(slice).take(cap as u64);
    let mut out = Vec::with_capacity(expanded as usize);
    if decoder.read_to_end(&mut out).is_err() || out.len() != (expanded as usize) {
        return Err("QRC expanded length mismatch".to_string());
    }
    Ok(out)
}

fn unpack_section(bytes: &[u8], offset: u64, size: u32, expanded: u32) -> Result<Vec<u8>, String> {
    if expanded == 0 || expanded > MAX_QRC_EXPANSION {
        return Err("QRC expansion exceeds 32 MiB".to_string());
    }
    check_range(bytes, offset, size as u64)?;
    let start = offset as usize;
    let end = start + (size as usize);
    inflate_qrc_slice(&bytes[start..end], expanded)
}

fn read_string(bytes: &[u8], base: u32, size: u32, offset: u64) -> Result<String, String> {
    if offset >= (size as u64) {
        return Err("QRC string offset exceeds table".to_string());
    }
    let start = (base as u64).saturating_add(offset) as usize;
    let limit = (base as u64).saturating_add(size as u64) as usize;
    if start >= bytes.len() || limit > bytes.len() || start >= limit {
        return Err("QRC string offset exceeds table".to_string());
    }
    let sub = &bytes[start..limit];
    let Some(nul_pos) = sub.iter().position(|&b| b == 0) else {
        return Err("QRC string is unterminated".to_string());
    };
    Ok(String::from_utf8_lossy(&sub[..nul_pos]).into_owned())
}

pub fn parse_qrc(input: &[u8]) -> Result<ParsedQrc, String> {
    if input.len() > 64 * 1024 * 1024 {
        return Err("QRC exceeds 64 MiB".to_string());
    }
    let bytes = if input.starts_with(b"QRCC") {
        let exp = word(input, 4)?;
        let packed_len = u32::try_from(input.len().saturating_sub(8)).map_err(|_| "QRC exceeds 64 MiB".to_string())?;
        unpack_section(input, 8, packed_len, exp)?
    } else {
        input.to_vec()
    };
    if !bytes.starts_with(b"QRCF") || word(&bytes, 4)? != 0x110 {
        return Err("Unsupported QRC version".to_string());
    }
    let toc = word(&bytes, 8)?;
    let toc_size = word(&bytes, 12)?;
    let ids = word(&bytes, 16)?;
    let ids_size = word(&bytes, 20)?;
    let strings = word(&bytes, 24)?;
    let strings_size = word(&bytes, 28)?;
    let data_base = word(&bytes, 48)?;
    let data_size = word(&bytes, 52)?;

    check_range(&bytes, toc as u64, toc_size as u64)?;
    check_range(&bytes, ids as u64, ids_size as u64)?;
    check_range(&bytes, strings as u64, strings_size as u64)?;
    check_range(&bytes, data_base as u64, data_size as u64)?;

    let mut entries = Vec::new();
    let mut seen_ids = HashSet::new();
    let mut offset: u64 = 0;
    let toc_size_u64 = toc_size as u64;
    while offset < toc_size_u64 {
        if toc_size_u64 - offset < 28 {
            return Err("Truncated QRC node".to_string());
        }
        let at = (toc as u64) + offset;
        let count = word(&bytes, at + 4)?;
        let length = 28u64 + (count as u64) * 16u64;
        if count > MAX_NODE_ATTRIBUTES || length > toc_size_u64 - offset {
            return Err("Invalid QRC attributes".to_string());
        }
        let mut id = String::new();
        let mut entry_offset = 0u32;
        let mut entry_size = 0u32;
        let mut entry_expanded = 0u32;
        let mut has_source = false;
        let mut has_size = false;
        for i in 0..count {
            let attr = at + 28 + (i as u64) * 16;
            let name_off = word(&bytes, attr)? as u64;
            let name = read_string(&bytes, strings, strings_size, name_off)?;
            let attr_type = word(&bytes, attr + 4)?;
            let a = word(&bytes, attr + 8)?;
            let b = word(&bytes, attr + 12)?;
            if name == "id" && attr_type == 7 {
                if (a as u64) + 4 > (ids_size as u64) || (word(&bytes, (ids as u64) + (a as u64))? as u64) != offset {
                    return Err("Invalid QRC ID back-reference".to_string());
                }
                id = read_string(&bytes, ids, ids_size, (a as u64) + 4)?;
            } else if name == "src" && attr_type == 6 {
                if a > data_size || b > data_size - a {
                    return Err("QRC file exceeds data table".to_string());
                }
                entry_offset = a;
                entry_size = b;
                has_source = true;
            } else if name == "size" && attr_type == 1 {
                entry_expanded = a;
                has_size = true;
            }
        }
        if has_source {
            if id.is_empty() || !seen_ids.insert(id.clone()) {
                return Err("Invalid or duplicate QRC file ID".to_string());
            }
            if has_size && entry_expanded == 0 {
                return Err("Invalid QRC expanded size".to_string());
            }
            entries.push(QrcEntry {
                id,
                offset: entry_offset,
                size: entry_size,
                expanded: entry_expanded,
            });
        }
        offset += length;
    }
    Ok(ParsedQrc {
        unpacked_bytes: bytes,
        data_base,
        entries,
    })
}

pub fn encode_parsed_qrc_toc(parsed: &ParsedQrc) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&parsed.data_base.to_le_bytes());
    out.extend_from_slice(&(parsed.entries.len() as u32).to_le_bytes());
    for e in &parsed.entries {
        let id_bytes = e.id.as_bytes();
        out.extend_from_slice(&(id_bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(id_bytes);
        out.extend_from_slice(&e.offset.to_le_bytes());
        out.extend_from_slice(&e.size.to_le_bytes());
        out.extend_from_slice(&e.expanded.to_le_bytes());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn zlib_compress(data: &[u8]) -> Vec<u8> {
        let mut enc = ZlibEncoder::new(Vec::new(), Compression::default());
        enc.write_all(data).unwrap();
        enc.finish().unwrap()
    }

    pub fn build_synthetic_qrcf(id: &str, payload: &[u8], compress_entry: bool, wrap_qrcc: bool) -> Vec<u8> {
        let stored_payload = if compress_entry {
            zlib_compress(payload)
        } else {
            payload.to_vec()
        };
        // Strings table: "id\0src\0size\0"
        let strings = b"id\0src\0size\0";
        // IDs table: [u32 BE node_offset = 0] + id + "\0"
        let mut ids = Vec::new();
        ids.extend_from_slice(&0u32.to_be_bytes());
        ids.extend_from_slice(id.as_bytes());
        ids.push(0);
        // TOC node: 28-byte header + 2 or 3 16-byte attributes
        let attr_count: u32 = if compress_entry { 3 } else { 2 };
        let mut toc = vec![0u8; 28];
        toc[4..8].copy_from_slice(&attr_count.to_be_bytes());
        // attr 0: id (name off 0, type 7, a = 0, b = 0)
        toc.extend_from_slice(&0u32.to_be_bytes());
        toc.extend_from_slice(&7u32.to_be_bytes());
        toc.extend_from_slice(&0u32.to_be_bytes());
        toc.extend_from_slice(&0u32.to_be_bytes());
        // attr 1: src (name off 3, type 6, a = 0, b = stored_payload.len())
        toc.extend_from_slice(&3u32.to_be_bytes());
        toc.extend_from_slice(&6u32.to_be_bytes());
        toc.extend_from_slice(&0u32.to_be_bytes());
        toc.extend_from_slice(&(stored_payload.len() as u32).to_be_bytes());
        if compress_entry {
            // attr 2: size (name off 7, type 1, a = payload.len(), b = 0)
            toc.extend_from_slice(&7u32.to_be_bytes());
            toc.extend_from_slice(&1u32.to_be_bytes());
            toc.extend_from_slice(&(payload.len() as u32).to_be_bytes());
            toc.extend_from_slice(&0u32.to_be_bytes());
        }

        let toc_off = 56u32;
        let ids_off = toc_off + (toc.len() as u32);
        let strings_off = ids_off + (ids.len() as u32);
        let data_off = strings_off + (strings.len() as u32);

        let mut qrcf = vec![0u8; 56];
        qrcf[0..4].copy_from_slice(b"QRCF");
        qrcf[4..8].copy_from_slice(&0x110u32.to_be_bytes());
        qrcf[8..12].copy_from_slice(&toc_off.to_be_bytes());
        qrcf[12..16].copy_from_slice(&(toc.len() as u32).to_be_bytes());
        qrcf[16..20].copy_from_slice(&ids_off.to_be_bytes());
        qrcf[20..24].copy_from_slice(&(ids.len() as u32).to_be_bytes());
        qrcf[24..28].copy_from_slice(&strings_off.to_be_bytes());
        qrcf[28..32].copy_from_slice(&(strings.len() as u32).to_be_bytes());
        qrcf[48..52].copy_from_slice(&data_off.to_be_bytes());
        qrcf[52..56].copy_from_slice(&(stored_payload.len() as u32).to_be_bytes());
        qrcf.extend_from_slice(&toc);
        qrcf.extend_from_slice(&ids);
        qrcf.extend_from_slice(strings);
        qrcf.extend_from_slice(&stored_payload);

        if wrap_qrcc {
            let compressed = zlib_compress(&qrcf);
            let mut qrcc = Vec::with_capacity(8 + compressed.len());
            qrcc.extend_from_slice(b"QRCC");
            qrcc.extend_from_slice(&(qrcf.len() as u32).to_be_bytes());
            qrcc.extend_from_slice(&compressed);
            qrcc
        } else {
            qrcf
        }
    }

    #[test]
    fn parses_qrcc_and_inflates_entry() {
        let raw = b"#MNU_1.0\nTIME:float:1.0\n";
        let qrcc = build_synthetic_qrcf("override/day/ICONS.mnu", raw, true, true);
        let parsed = parse_qrc(&qrcc).expect("QRCC should parse");
        assert_eq!(parsed.entries.len(), 1);
        let e = &parsed.entries[0];
        assert_eq!(e.id, "override/day/ICONS.mnu");
        assert_eq!(e.expanded, raw.len() as u32);
        let start = (parsed.data_base + e.offset) as usize;
        let slice = &parsed.unpacked_bytes[start..start + (e.size as usize)];
        let inflated = inflate_qrc_slice(slice, e.expanded).expect("entry should inflate");
        assert_eq!(inflated, raw);
    }

    #[test]
    fn rejects_bad_back_reference() {
        let mut qrcf = build_synthetic_qrcf("lib/raf/vert0.vpo", b"cg", false, false);
        let ids_off = u32::from_be_bytes(qrcf[16..20].try_into().unwrap()) as usize;
        qrcf[ids_off..ids_off + 4].copy_from_slice(&99u32.to_be_bytes());
        let err = parse_qrc(&qrcf).unwrap_err();
        assert_eq!(err, "Invalid QRC ID back-reference");
    }
}

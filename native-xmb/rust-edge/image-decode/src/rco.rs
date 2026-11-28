#![forbid(unsafe_code)]
//! PS3 RCO resource containers (`\0FRP`, big-endian): the image index and the English string table.
//! Mirrors `native-xmb/src/rco_parse.cpp` (`rco::parseContainer` / `rco::readEnglish`) including every error
//! message, so the C++/Rust parity test can compare refusals as well as results.

use flate2::read::ZlibDecoder;
use std::io::Read;

const RANGE: &str = "Asset offset exceeds its container";

fn range(b: &[u8], p: u64, n: u64) -> Result<(), String> {
    let len = b.len() as u64;
    if p > len || n > len - p {
        return Err(RANGE.to_string());
    }
    Ok(())
}
fn be32(b: &[u8], p: u64) -> Result<u32, String> {
    range(b, p, 4)?;
    let p = p as usize;
    Ok(u32::from_be_bytes([b[p], b[p + 1], b[p + 2], b[p + 3]]))
}
fn be16(b: &[u8], p: u64) -> Result<u16, String> {
    range(b, p, 2)?;
    let p = p as usize;
    Ok(u16::from_be_bytes([b[p], b[p + 1]]))
}
fn inflate(b: &[u8], p: u64, packed: u32, expanded: u32) -> Result<Vec<u8>, String> {
    if expanded == 0 || expanded > 32 * 1024 * 1024 {
        return Err("Asset expansion exceeds 32 MiB".to_string());
    }
    range(b, p, u64::from(packed))?;
    let src = &b[p as usize..p as usize + packed as usize];
    let mut out = Vec::with_capacity(expanded as usize);
    // One byte past the declared size is enough to prove a mismatch without inflating an unbounded stream.
    let ok = ZlibDecoder::new(src).take(u64::from(expanded) + 1).read_to_end(&mut out).is_ok();
    if !ok || out.len() != expanded as usize {
        return Err("Asset zlib length mismatch".to_string());
    }
    Ok(out)
}
fn find_nul(b: &[u8], from: u64) -> Option<u64> {
    if from >= b.len() as u64 {
        return None;
    }
    b[from as usize..].iter().position(|&c| c == 0).map(|i| from + i as u64)
}

pub struct ParsedRco {
    pub tables: Vec<u8>,
    pub labels: u32,
    pub image_data: u32,
    /// Image name -> table offset, in file order (a later duplicate replaces an earlier one, as QHash::insert does).
    pub entries: Vec<(String, u32)>,
}

pub fn parse_rco(file: &[u8]) -> Result<ParsedRco, String> {
    if be32(file, 0)? != 0x4652_5000 {
        return Err("Expected PS3 big-endian RCO".to_string());
    }
    let compression = be32(file, 12)? >> 4;
    let tables = match compression {
        0 => file.to_vec(),
        1 => {
            let packed = be32(file, 0xa4)?;
            let expanded = be32(file, 0xa8)?;
            let mut t = file[..0xa4.min(file.len())].to_vec();
            t.extend_from_slice(&inflate(file, 0xb0, packed, expanded)?);
            t
        }
        _ => return Err("Unsupported RCO header compression".to_string()),
    };
    let labels = be32(file, 0x40)?;
    let image_data = be32(file, 0x80)?;
    let label_length = be32(file, 0x44)?;
    let root = be32(file, 0x24)?;
    range(&tables, u64::from(labels), u64::from(label_length))?;
    let mut entries: Vec<(String, u32)> = Vec::new();
    if root == 0xffff_ffff {
        return Ok(ParsedRco { tables, labels, image_data, entries });
    }
    let count = be32(&tables, u64::from(root.wrapping_add(16)))?;
    if count > 65536 {
        return Err("RCO image count exceeds limit".to_string());
    }
    let mut at = u64::from(root) + 0x28;
    for i in 0..count {
        range(&tables, at, 0x38)?;
        if be16(&tables, at)? != 0x0401 {
            return Err("Unexpected RCO image entry".to_string());
        }
        let label = be32(&tables, at + 4)?;
        if label >= label_length {
            return Err("RCO label is out of range".to_string());
        }
        let start = u64::from(labels) + u64::from(label);
        let end = match find_nul(&tables, start) {
            Some(e) if e < u64::from(labels) + u64::from(label_length) => e,
            _ => return Err("Unterminated RCO label".to_string()),
        };
        let name = String::from_utf8_lossy(&tables[start as usize..end as usize]).into_owned();
        if let Some(slot) = entries.iter_mut().find(|(n, _)| *n == name) {
            slot.1 = at as u32;
        } else {
            entries.push((name, at as u32));
        }
        let next = be32(&tables, at + 20)?;
        if i + 1 < count && next < 0x38 {
            return Err("Invalid RCO image sibling offset".to_string());
        }
        at += u64::from(next);
    }
    Ok(ParsedRco { tables, labels, image_data, entries })
}

// QStringDecoder's default flags skip one leading byte-order mark when decoding UTF-16/UTF-32.
fn utf16be_units(bytes: &[u8]) -> Vec<u16> {
    let mut units: Vec<u16> = bytes.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
    if units.first() == Some(&0xfeff) {
        units.remove(0);
    }
    units
}
fn utf32be_units(bytes: &[u8]) -> Result<Vec<u16>, String> {
    // QStringDecoder(Utf32BE) replaces a code point above U+10FFFF with U+FFFD and does not report an error
    // (measured by the C++/Rust parity test), so neither does this.
    let mut units = Vec::with_capacity(bytes.len() / 2);
    let mut first = true;
    for c in bytes.chunks_exact(4) {
        let code = u32::from_be_bytes([c[0], c[1], c[2], c[3]]);
        if first && code == 0xfeff {
            first = false;
            continue;
        }
        first = false;
        if code > 0x10ffff {
            units.push(0xfffd);
        } else if code >= 0x10000 {
            let v = code - 0x10000;
            units.push(0xd800 | (v >> 10) as u16);
            units.push(0xdc00 | (v & 0x3ff) as u16);
        } else {
            units.push(code as u16);
        }
    }
    Ok(units)
}

/// The English string table: (label, UTF-16 code units with trailing NULs removed), in table order.
pub fn rco_english(file: &[u8], tables: &[u8], labels: u32) -> Result<Vec<(String, Vec<u16>)>, String> {
    let mut result: Vec<(String, Vec<u16>)> = Vec::new();
    let root = be32(file, 0x18)?;
    let text_data = be32(file, 0x38)?;
    let text_length = be32(file, 0x3c)?;
    if root == 0xffff_ffff || text_data == 0xffff_ffff || text_length == 0 {
        return Ok(result);
    }
    range(file, u64::from(text_data), u64::from(text_length))?;
    let text_end = u64::from(text_data) + u64::from(text_length);
    let compressed = (be32(file, 12)? >> 4) != 0;
    let english: Vec<u8> = if compressed {
        let mut found = Vec::new();
        let mut chunk = u64::from(text_data);
        for _ in 0..64 {
            range(file, chunk, 16)?;
            if chunk < u64::from(text_data) || chunk + 16 > text_end {
                return Err("RCO language header exceeds text section".to_string());
            }
            let lang = be16(file, chunk)?;
            let next = be32(file, chunk + 4)?;
            let packed = be32(file, chunk + 8)?;
            if u64::from(packed) > text_end - chunk - 16 {
                return Err("RCO language data exceeds text section".to_string());
            }
            if lang == 1 {
                found = inflate(file, chunk + 16, packed, be32(file, chunk + 12)?)?;
                break;
            }
            if next == 0 {
                break;
            }
            if u64::from(next) < 16 + u64::from(packed) {
                return Err("Invalid RCO language sibling offset".to_string());
            }
            chunk += u64::from(next);
        }
        found
    } else {
        file[text_data as usize..text_end as usize].to_vec()
    };
    if english.is_empty() {
        return Ok(result);
    }
    let languages = be32(tables, u64::from(root.wrapping_add(16)))?;
    if languages > 64 {
        return Err("Invalid RCO language count".to_string());
    }
    let label_section = be32(file, 0x44)?;
    let mut at = u64::from(root) + 0x28;
    for _ in 0..languages {
        range(tables, at, 0x30)?;
        let info = at + 0x28;
        if be16(tables, info)? == 1 {
            let format = be16(tables, info + 2)?;
            let count = be32(tables, info + 4)?;
            if count > 65536 {
                return Err("Invalid RCO text count".to_string());
            }
            range(tables, info + 8, u64::from(count) * 12)?;
            for i in 0..u64::from(count) {
                let index = info + 8 + i * 12;
                let label = be32(tables, index)?;
                let length = be32(tables, index + 4)?;
                let offset = be32(tables, index + 8)?;
                if label >= label_section {
                    return Err("RCO text label exceeds label section".to_string());
                }
                let label_at = u64::from(labels) + u64::from(label);
                let end = match find_nul(tables, label_at) {
                    Some(e) if e < u64::from(labels) + u64::from(label_section) => e,
                    _ => return Err("Unterminated RCO text label".to_string()),
                };
                range(&english, u64::from(offset), u64::from(length))?;
                let bytes = &english[offset as usize..offset as usize + length as usize];
                let mut value: Vec<u16> = match format {
                    0 => String::from_utf8_lossy(bytes).encode_utf16().collect(),
                    1 => utf16be_units(bytes),
                    2 => utf32be_units(bytes)?,
                    _ => return Err("Unsupported RCO text encoding".to_string()),
                };
                while value.last() == Some(&0) {
                    value.pop();
                }
                let key = String::from_utf8_lossy(&tables[label_at as usize..end as usize]).into_owned();
                if let Some(slot) = result.iter_mut().find(|(k, _)| *k == key) {
                    slot.1 = value;
                } else {
                    result.push((key, value));
                }
            }
            return Ok(result);
        }
        let next = be32(tables, at + 20)?;
        if next == 0 {
            break;
        }
        if next < 0x30 {
            return Err("Invalid RCO text sibling offset".to_string());
        }
        at += u64::from(next);
    }
    Ok(result)
}

/// `[u32 labels][u32 image_data][u32 count]` then per entry `[u32 name_len][name utf8][u32 offset]`, all LE.
pub fn encode_rco_toc(parsed: &ParsedRco) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&parsed.labels.to_le_bytes());
    out.extend_from_slice(&parsed.image_data.to_le_bytes());
    out.extend_from_slice(&(parsed.entries.len() as u32).to_le_bytes());
    for (name, at) in &parsed.entries {
        out.extend_from_slice(&(name.len() as u32).to_le_bytes());
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(&at.to_le_bytes());
    }
    out
}

/// `[u32 count]` then per entry `[u32 key_len][key utf8][u32 unit_count][u16 LE * unit_count]`.
pub fn encode_rco_english(table: &[(String, Vec<u16>)]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&(table.len() as u32).to_le_bytes());
    for (key, units) in table {
        out.extend_from_slice(&(key.len() as u32).to_le_bytes());
        out.extend_from_slice(key.as_bytes());
        out.extend_from_slice(&(units.len() as u32).to_le_bytes());
        for u in units {
            out.extend_from_slice(&u.to_le_bytes());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn put32(b: &mut [u8], at: usize, v: u32) {
        b[at..at + 4].copy_from_slice(&v.to_be_bytes());
    }

    // Header 0xb0 bytes, tables appended in place (uncompressed): an image tree at 0xb0 with one entry, a text tree
    // with one English language block, a label section and a UTF-8 text section.
    fn synthetic() -> Vec<u8> {
        let mut f = vec![0u8; 0x200];
        put32(&mut f, 0, 0x4652_5000);
        let image_root = 0xb0usize;
        let text_root = 0x110usize;
        let labels = 0x180usize;
        let text = 0x1b0usize;
        put32(&mut f, 0x24, image_root as u32);
        put32(&mut f, 0x18, text_root as u32);
        put32(&mut f, 0x40, labels as u32);
        put32(&mut f, 0x44, 0x20);
        put32(&mut f, 0x38, text as u32);
        put32(&mut f, 0x3c, 0x10);
        put32(&mut f, image_root + 16, 1);
        let entry = image_root + 0x28;
        f[entry..entry + 2].copy_from_slice(&0x0401u16.to_be_bytes());
        put32(&mut f, entry + 4, 0);
        put32(&mut f, text_root + 16, 1);
        let info = text_root + 0x28 + 0x28;
        f[info..info + 2].copy_from_slice(&1u16.to_be_bytes());
        put32(&mut f, info + 4, 1);
        put32(&mut f, info + 8, 9);
        put32(&mut f, info + 12, 6);
        put32(&mut f, info + 16, 0);
        f[labels..labels + 9].copy_from_slice(b"tex_icon\0");
        f[labels + 9..labels + 18].copy_from_slice(b"msg_test\0");
        f[text..text + 6].copy_from_slice(b"Hello\0");
        f
    }

    #[test]
    fn parses_images_and_english_and_refuses_bad_magic() {
        let f = synthetic();
        let parsed = parse_rco(&f).unwrap();
        assert_eq!(parsed.entries, vec![("tex_icon".to_string(), 0xd8)]);
        let english = rco_english(&f, &parsed.tables, parsed.labels).unwrap();
        assert_eq!(english.len(), 1);
        assert_eq!(english[0].0, "msg_test");
        assert_eq!(String::from_utf16(&english[0].1).unwrap(), "Hello");
        let mut bad = f.clone();
        bad[0] = 0;
        assert_eq!(parse_rco(&bad).err().as_deref(), Some("Expected PS3 big-endian RCO"));
        assert_eq!(parse_rco(&f[..8]).err().as_deref(), Some(RANGE));
    }
}

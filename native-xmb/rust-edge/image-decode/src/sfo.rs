#![forbid(unsafe_code)]

#[inline]
fn u16_le(b: &[u8], at: usize) -> u16 {
    if at + 2 <= b.len() {
        u16::from_le_bytes([b[at], b[at + 1]])
    } else {
        0
    }
}

#[inline]
fn u32_le(b: &[u8], at: usize) -> u32 {
    if at + 4 <= b.len() {
        u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
    } else {
        0
    }
}

/// Parses a Sony `PARAM.SFO` (`\0PSF`) binary buffer into a packed little-endian key-value payload:
///   `[u32 entry_count LE]`
///   per entry:
///     `[u32 key_len LE][u8 * key_len][u32 val_len LE][u8 * val_len]`
///
/// Matches C++ `readSfo()` in `native-xmb/src/shell.cpp`:
/// - Requires at least 20 bytes and magic `\0PSF`.
/// - Bounds entry count to 256.
/// - Format `0x0204` (UTF-8 string) and `0x0004` (UTF-8 special): decodes UTF-8 and strips `\0` bytes.
/// - Format `0x0404` (`u32 LE` integer): formats as decimal ASCII string.
pub fn parse_sfo_packed(b: &[u8]) -> Result<Vec<u8>, String> {
    if b.len() < 20 || &b[0..4] != b"\0PSF" {
        return Err("invalid or truncated PARAM.SFO header".to_string());
    }
    let keys_off = u32_le(b, 8) as usize;
    let data_off = u32_le(b, 12) as usize;
    let count = u32_le(b, 16).min(256) as usize;

    let mut entries: Vec<(String, String)> = Vec::with_capacity(count);
    for i in 0..count {
        let e = 20 + i * 16;
        if e + 16 > b.len() {
            break;
        }
        let key_rel = u16_le(b, e) as usize;
        let format = u16_le(b, e + 2);
        let length = u32_le(b, e + 4) as usize;
        let data_rel = u32_le(b, e + 12) as usize;

        let Some(key_at) = keys_off.checked_add(key_rel) else {
            continue;
        };
        let Some(data_at) = data_off.checked_add(data_rel) else {
            continue;
        };
        let Some(data_end) = data_at.checked_add(length) else {
            continue;
        };
        if key_at >= b.len() || data_end > b.len() {
            continue;
        }

        let key_slice = &b[key_at..];
        let key_nul = key_slice.iter().position(|&c| c == 0).unwrap_or(key_slice.len());
        let key: String = key_slice[..key_nul].iter().map(|&c| char::from(c)).collect();

        let val = if format == 0x0204 || format == 0x0004 {
            let raw = String::from_utf8_lossy(&b[data_at..data_end]);
            raw.chars().filter(|&ch| ch != '\0').collect()
        } else if format == 0x0404 {
            if data_at + 4 > b.len() {
                continue;
            }
            u32_le(b, data_at).to_string()
        } else {
            continue;
        };
        entries.push((key, val));
    }

    let mut out = Vec::with_capacity(4 + entries.len() * 32);
    out.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    for (k, v) in entries {
        let kb = k.as_bytes();
        let vb = v.as_bytes();
        out.extend_from_slice(&(kb.len() as u32).to_le_bytes());
        out.extend_from_slice(kb);
        out.extend_from_slice(&(vb.len() as u32).to_le_bytes());
        out.extend_from_slice(vb);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn build_sample_sfo() -> Vec<u8> {
        // 3 entries: TITLE (0x0204), TITLE_ID (0x0004), ATTRIBUTE (0x0404)
        let keys = b"TITLE\0TITLE_ID\0ATTRIBUTE\0";
        let val_title = b"Demon's Souls\0\0\0";
        let val_id = b"BLUS30443\0";
        let val_attr = 0x0000_00A5u32.to_le_bytes();

        let header_and_index_len = 20 + 3 * 16; // 68
        let keys_off = header_and_index_len;
        let data_off = (keys_off + keys.len() + 3) & !3; // 4-byte aligned

        let mut b = vec![0u8; data_off + val_title.len() + val_id.len() + val_attr.len()];
        b[0..4].copy_from_slice(b"\0PSF");
        b[4..8].copy_from_slice(&0x0000_0101u32.to_le_bytes());
        b[8..12].copy_from_slice(&(keys_off as u32).to_le_bytes());
        b[12..16].copy_from_slice(&(data_off as u32).to_le_bytes());
        b[16..20].copy_from_slice(&3u32.to_le_bytes());

        // Entry 0: TITLE (key_rel=0, fmt=0x0204, len=14, max=16, data_rel=0)
        b[20..22].copy_from_slice(&0u16.to_le_bytes());
        b[22..24].copy_from_slice(&0x0204u16.to_le_bytes());
        b[24..28].copy_from_slice(&(val_title.len() as u32).to_le_bytes());
        b[28..32].copy_from_slice(&(val_title.len() as u32).to_le_bytes());
        b[32..36].copy_from_slice(&0u32.to_le_bytes());

        // Entry 1: TITLE_ID (key_rel=6, fmt=0x0004, len=10, max=10, data_rel=16)
        b[36..38].copy_from_slice(&6u16.to_le_bytes());
        b[38..40].copy_from_slice(&0x0004u16.to_le_bytes());
        b[40..44].copy_from_slice(&(val_id.len() as u32).to_le_bytes());
        b[44..48].copy_from_slice(&(val_id.len() as u32).to_le_bytes());
        b[48..52].copy_from_slice(&(val_title.len() as u32).to_le_bytes());

        // Entry 2: ATTRIBUTE (key_rel=15, fmt=0x0404, len=4, max=4, data_rel=26)
        b[52..54].copy_from_slice(&15u16.to_le_bytes());
        b[54..56].copy_from_slice(&0x0404u16.to_le_bytes());
        b[56..60].copy_from_slice(&4u32.to_le_bytes());
        b[60..64].copy_from_slice(&4u32.to_le_bytes());
        b[64..68].copy_from_slice(&((val_title.len() + val_id.len()) as u32).to_le_bytes());

        b[keys_off..keys_off + keys.len()].copy_from_slice(keys);
        let mut d = data_off;
        b[d..d + val_title.len()].copy_from_slice(val_title);
        d += val_title.len();
        b[d..d + val_id.len()].copy_from_slice(val_id);
        d += val_id.len();
        b[d..d + val_attr.len()].copy_from_slice(&val_attr);
        b
    }

    #[test]
    fn parses_strings_and_u32_entries() {
        let sfo = build_sample_sfo();
        let packed = parse_sfo_packed(&sfo).expect("valid SFO");
        let count = u32_le(&packed, 0);
        assert_eq!(count, 3);
    }

    #[test]
    fn rejects_invalid_magic_or_short_header() {
        assert!(parse_sfo_packed(b"NOT_SFO").is_err());
        assert!(parse_sfo_packed(b"\0PSFshort").is_err());
    }
}

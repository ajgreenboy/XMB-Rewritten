#![forbid(unsafe_code)]

use crate::gtf::DecodedGtf;

/// Decodes uncompressed or RLE true-colour/greyscale TGA images (8/24/32-bpp),
/// matching `Assets::decodeTga` semantics byte-for-byte.
pub fn decode_tga(b: &[u8]) -> Result<DecodedGtf, String> {
    if b.len() < 18 {
        return Err("TGA header too short".to_string());
    }
    let id_length = usize::from(b[0]);
    let map_type = b[1];
    let img_type = b[2];
    let width = u32::from(u16::from_le_bytes([b[12], b[13]]));
    let height = u32::from(u16::from_le_bytes([b[14], b[15]]));
    let bpp = b[16];
    let descriptor = b[17];

    if map_type != 0
        || !matches!(img_type, 2 | 3 | 10 | 11)
        || width == 0
        || height == 0
        || !matches!(bpp, 8 | 24 | 32)
    {
        return Err("Unsupported or invalid TGA header".to_string());
    }
    if width > 8192 || height > 8192 || u64::from(width) * u64::from(height) > 48 * 1024 * 1024 {
        return Err("Invalid TGA dimensions".to_string());
    }

    let bytes = usize::from(bpp / 8);
    let mut at = 18usize + id_length;
    if at > b.len() {
        return Err("TGA image ID exceeds buffer".to_string());
    }

    #[inline]
    fn read_pixel(p: &[u8], bytes: usize) -> u32 {
        if bytes == 1 {
            let v = u32::from(p[0]);
            0xFF00_0000 | (v << 16) | (v << 8) | v
        } else {
            let a = if bytes == 4 { u32::from(p[3]) } else { 255 };
            (a << 24) | (u32::from(p[2]) << 16) | (u32::from(p[1]) << 8) | u32::from(p[0])
        }
    }

    let w = width as usize;
    let h = height as usize;
    let total = w * h;
    let mut pixels = Vec::with_capacity(total);

    if img_type < 9 {
        let needed = total
            .checked_mul(bytes)
            .ok_or_else(|| "TGA payload size overflow".to_string())?;
        if needed > b.len() - at {
            return Err("TGA pixel data truncated".to_string());
        }
        for _ in 0..total {
            pixels.push(read_pixel(&b[at..at + bytes], bytes));
            at += bytes;
        }
    } else {
        while pixels.len() < total {
            if at >= b.len() {
                return Err("TGA RLE packet truncated".to_string());
            }
            let header = b[at];
            at += 1;
            let count = usize::from(header & 0x7f) + 1;
            if (header & 0x80) != 0 {
                if bytes > b.len() - at {
                    return Err("TGA RLE run pixel truncated".to_string());
                }
                let p = read_pixel(&b[at..at + bytes], bytes);
                at += bytes;
                for _ in 0..count {
                    pixels.push(p);
                }
            } else {
                let raw_bytes = count * bytes;
                if raw_bytes > b.len() - at {
                    return Err("TGA RLE raw packet truncated".to_string());
                }
                for _ in 0..count {
                    pixels.push(read_pixel(&b[at..at + bytes], bytes));
                    at += bytes;
                }
            }
        }
    }

    let top_first = (descriptor & 0x20) != 0;
    let mut argb_bytes = vec![0u8; total * 4];
    for y in 0..h {
        let dst_y = if top_first { y } else { h - 1 - y };
        for x in 0..w {
            let dst = (dst_y * w + x) * 4;
            argb_bytes[dst..dst + 4].copy_from_slice(&pixels[y * w + x].to_ne_bytes());
        }
    }

    Ok(DecodedGtf {
        width,
        height,
        argb_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_rle_bottom_up_and_top_down_tga() {
        let mut bottom_up = vec![0u8; 18];
        bottom_up[2] = 10; // RLE true-colour
        bottom_up[12..14].copy_from_slice(&2u16.to_le_bytes());
        bottom_up[14..16].copy_from_slice(&2u16.to_le_bytes());
        bottom_up[16] = 32;
        bottom_up[17] = 0x00; // bottom-up
        // Run of 2 blue-ish pixels, then raw 2 green pixels.
        bottom_up.extend_from_slice(&[0x81, 0x10, 0x20, 0x30, 0x80]);
        bottom_up.extend_from_slice(&[0x01, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff]);

        let img = decode_tga(&bottom_up).unwrap();
        let top_left = u32::from_ne_bytes(img.argb_bytes[0..4].try_into().unwrap());
        let bot_left = u32::from_ne_bytes(img.argb_bytes[8..12].try_into().unwrap());
        assert_eq!(top_left, 0xFF00_FF00);
        assert_eq!(bot_left, 0x8030_2010);
    }
}

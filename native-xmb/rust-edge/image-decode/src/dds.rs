#![forbid(unsafe_code)]

use crate::gtf::DecodedGtf;

#[inline]
fn le_u16(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

#[inline]
fn le_u32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

#[inline]
fn argb(r: u32, g: u32, b: u32, a: u32) -> u32 {
    (a << 24) | (r << 16) | (g << 8) | b
}

#[inline]
fn colour565_components(value: u16) -> [u32; 4] {
    let r = u32::from(value >> 11);
    let g = u32::from((value >> 5) & 63);
    let b = u32::from(value & 31);
    [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255]
}

fn decode_dxt_blocks(b: &[u8], w: u32, h: u32, format: u32) -> Result<DecodedGtf, String> {
    let block_bytes = if format == 8 { 8u64 } else { 16u64 };
    let blocks_x = u64::from(w.div_ceil(4));
    let blocks_y = u64::from(h.div_ceil(4));
    let stride = blocks_x
        .checked_mul(block_bytes)
        .ok_or_else(|| "DXT blocks exceed GIM image".to_string())?;
    let length = stride
        .checked_mul(blocks_y)
        .ok_or_else(|| "DXT blocks exceed GIM image".to_string())?;
    let available = (b.len() as u64).saturating_sub(128);
    if length > available {
        return Err("DXT blocks exceed GIM image".to_string());
    }

    let width = w as usize;
    let height = h as usize;
    let mut argb_bytes = vec![0u8; width * height * 4];
    let stride_usize = stride as usize;
    let block_usize = block_bytes as usize;

    for by in (0..height).step_by(4) {
        for bx in (0..width).step_by(4) {
            let block_off = 128 + (by / 4) * stride_usize + (bx / 4) * block_usize;
            let block = &b[block_off..block_off + block_usize];
            let colours = if format == 8 { block } else { &block[8..] };

            let c0 = le_u16(colours, 0);
            let c1 = le_u16(colours, 2);
            let p0 = colour565_components(c0);
            let p1 = colour565_components(c1);
            let mut palette = [p0, p1, [0, 0, 0, 255], [0, 0, 0, 255]];

            if format != 8 || c0 > c1 {
                for i in 0..2u32 {
                    let w0 = 2 - i;
                    let w1 = 1 + i;
                    palette[(2 + i) as usize] = [
                        (w0 * p0[0] + w1 * p1[0]) / 3,
                        (w0 * p0[1] + w1 * p1[1]) / 3,
                        (w0 * p0[2] + w1 * p1[2]) / 3,
                        255,
                    ];
                }
            } else {
                palette[2] = [
                    (p0[0] + p1[0]) / 2,
                    (p0[1] + p1[1]) / 2,
                    (p0[2] + p1[2]) / 2,
                    255,
                ];
                palette[3] = [0, 0, 0, 0];
            }

            let indices = le_u32(colours, 4);
            let mut alphas = [0u32; 8];
            let mut alpha_indices = 0u64;
            if format == 10 {
                alphas[0] = u32::from(block[0]);
                alphas[1] = u32::from(block[1]);
                if alphas[0] > alphas[1] {
                    for i in 1..=6u32 {
                        alphas[(i + 1) as usize] = ((7 - i) * alphas[0] + i * alphas[1]) / 7;
                    }
                } else {
                    for i in 1..=4u32 {
                        alphas[(i + 1) as usize] = ((5 - i) * alphas[0] + i * alphas[1]) / 5;
                    }
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                for (i, &byte) in block[2..8].iter().enumerate() {
                    alpha_indices |= u64::from(byte) << (i * 8);
                }
            }

            let y_end = (by + 4).min(height);
            let x_end = (bx + 4).min(width);
            for (ly, py) in (by..y_end).enumerate() {
                for (lx, px) in (bx..x_end).enumerate() {
                    let pixel = ly * 4 + lx;
                    let colour = palette[((indices >> (pixel * 2)) & 3) as usize];
                    let alpha = if format == 10 {
                        alphas[((alpha_indices >> (pixel * 3)) & 7) as usize]
                    } else if format == 9 {
                        ((u32::from(block[pixel / 2]) >> ((pixel & 1) * 4)) & 15) * 17
                    } else {
                        colour[3]
                    };
                    let val = argb(colour[0], colour[1], colour[2], alpha);
                    let dst = (py * width + px) * 4;
                    argb_bytes[dst..dst + 4].copy_from_slice(&val.to_ne_bytes());
                }
            }
        }
    }

    Ok(DecodedGtf {
        width: w,
        height: h,
        argb_bytes,
    })
}

/// Decodes a DirectDraw Surface (.dds) texture (DXT1/3/5 or 32-bit bitmask uncompressed),
/// matching `Assets::decodeDds` semantics byte-for-byte.
pub fn decode_dds(b: &[u8]) -> Result<DecodedGtf, String> {
    if b.len() < 128 {
        return Err("Asset offset exceeds its container".to_string());
    }
    if &b[0..4] != b"DDS " || le_u32(b, 4) != 124 || le_u32(b, 76) != 32 {
        return Err("Invalid DDS header".to_string());
    }
    let h = le_u32(b, 12);
    let w = le_u32(b, 16);
    let flags = le_u32(b, 80);
    let fourcc = le_u32(b, 84);
    if w == 0 || h == 0 || w > 4096 || h > 4096 {
        return Err("Invalid DDS dimensions".to_string());
    }

    if (flags & 4) != 0 {
        let format = match fourcc {
            0x31545844 => 8,  // DXT1
            0x33545844 => 9,  // DXT3
            0x35545844 => 10, // DXT5
            _ => return Err("Unsupported DDS compression".to_string()),
        };
        return decode_dxt_blocks(b, w, h, format);
    }

    if (flags & 0x40) == 0 || le_u32(b, 88) != 32 {
        return Err("Unsupported DDS pixel format".to_string());
    }
    let stride = if (le_u32(b, 8) & 8) != 0 {
        u64::from(le_u32(b, 20))
    } else {
        u64::from(w) * 4
    };
    if stride < u64::from(w) * 4 {
        return Err("Invalid DDS row pitch".to_string());
    }
    let data_bytes = stride
        .checked_mul(u64::from(h))
        .ok_or_else(|| "Asset offset exceeds its container".to_string())?;
    let available = (b.len() as u64).saturating_sub(128);
    if data_bytes > available {
        return Err("Asset offset exceeds its container".to_string());
    }

    let masks = [
        le_u32(b, 92),
        le_u32(b, 96),
        le_u32(b, 100),
        le_u32(b, 104),
    ];
    let mut shifts = [0u32; 4];
    let mut maxima = [0u32; 4];
    let mut used = 0u32;
    for i in 0..4 {
        let mask = masks[i];
        if mask == 0 {
            if i < 3 {
                return Err("Missing DDS colour mask".to_string());
            }
            continue;
        }
        if (used & mask) != 0 {
            return Err("Overlapping DDS colour masks".to_string());
        }
        used |= mask;
        let shift = mask.trailing_zeros();
        let maximum = mask >> shift;
        if (maximum & maximum.wrapping_add(1)) != 0 {
            return Err("Non-contiguous DDS colour mask".to_string());
        }
        shifts[i] = shift;
        maxima[i] = maximum;
    }

    let width = w as usize;
    let height = h as usize;
    let stride_usize = stride as usize;
    let mut argb_bytes = vec![0u8; width * height * 4];

    for y in 0..height {
        let row_base = 128 + y * stride_usize;
        for x in 0..width {
            let pixel = le_u32(b, row_base + x * 4);
            let mut v = [0u32, 0u32, 0u32, 255u32];
            for c in 0..4 {
                if masks[c] != 0 {
                    let raw = u64::from((pixel & masks[c]) >> shifts[c]);
                    v[c] = ((raw * 255) / u64::from(maxima[c])) as u32;
                }
            }
            let val = argb(v[0], v[1], v[2], v[3]);
            let dst = (y * width + x) * 4;
            argb_bytes[dst..dst + 4].copy_from_slice(&val.to_ne_bytes());
        }
    }

    Ok(DecodedGtf {
        width: w,
        height: h,
        argb_bytes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_dds_header(
        w: u32,
        h: u32,
        flags08: u32,
        pitch: u32,
        pf_flags: u32,
        fourcc: u32,
        bpp: u32,
        masks: [u32; 4],
    ) -> Vec<u8> {
        let mut out = vec![0u8; 128];
        out[0..4].copy_from_slice(b"DDS ");
        out[4..8].copy_from_slice(&124u32.to_le_bytes());
        out[8..12].copy_from_slice(&flags08.to_le_bytes());
        out[12..16].copy_from_slice(&h.to_le_bytes());
        out[16..20].copy_from_slice(&w.to_le_bytes());
        out[20..24].copy_from_slice(&pitch.to_le_bytes());
        out[76..80].copy_from_slice(&32u32.to_le_bytes());
        out[80..84].copy_from_slice(&pf_flags.to_le_bytes());
        out[84..88].copy_from_slice(&fourcc.to_le_bytes());
        out[88..92].copy_from_slice(&bpp.to_le_bytes());
        for (i, m) in masks.iter().enumerate() {
            let off = 92 + i * 4;
            out[off..off + 4].copy_from_slice(&m.to_le_bytes());
        }
        out
    }

    #[test]
    fn decodes_dxt1_and_rejects_bad_masks() {
        let mut dds = make_dds_header(4, 4, 0, 0, 4, 0x31545844, 0, [0; 4]);
        dds.extend_from_slice(&[0x00, 0xf8, 0x1f, 0x00, 0x00, 0x55, 0xaa, 0xff]);
        let img = decode_dds(&dds).unwrap();
        assert_eq!(img.width, 4);
        assert_eq!(img.height, 4);
        let p0 = u32::from_ne_bytes(img.argb_bytes[0..4].try_into().unwrap());
        assert_eq!(p0, 0xFFFF_0000);

        let bad = make_dds_header(2, 2, 0, 0, 0x40, 0, 32, [0xFF, 0xFF, 0xFF00, 0]);
        let mut bad_full = bad;
        bad_full.extend_from_slice(&[0u8; 16]);
        assert_eq!(
            decode_dds(&bad_full).err().as_deref(),
            Some("Overlapping DDS colour masks")
        );
    }
}

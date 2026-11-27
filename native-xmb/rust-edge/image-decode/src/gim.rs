//! Safe Rust decoder for PS3/PSP big-endian GIM (`.GIM1.00`) textures
//! (`Assets::decodeGim` in `native-xmb/src/assets.cpp` and `P3t::gim` in `native-xmb/src/p3t.cpp`).
//!
//! Supports RGBA8888 (0x03), INDEX4/8/16 (0x04..=0x06) with RGB565/RGBA5551/RGBA4444/RGBA8888
//! palettes or palette-less alpha coverage masks, and DXT1/3/5 (0x08..=0x0A).

#![forbid(unsafe_code)]

use crate::gtf::DecodedGtf as DecodedImage;

#[inline]
fn argb(a: u8, r: u8, g: u8, b: u8) -> u32 {
    ((a as u32) << 24) | ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

#[inline]
fn a_of(c: u32) -> u8 {
    ((c >> 24) & 0xff) as u8
}
#[inline]
fn r_of(c: u32) -> u8 {
    ((c >> 16) & 0xff) as u8
}
#[inline]
fn g_of(c: u32) -> u8 {
    ((c >> 8) & 0xff) as u8
}
#[inline]
fn b_of(c: u32) -> u8 {
    (c & 0xff) as u8
}

fn check_range(b: &[u8], p: u64, n: u64) -> Result<(), String> {
    let len = b.len() as u64;
    if p > len || n > len - p {
        return Err("Asset offset exceeds its container".to_string());
    }
    Ok(())
}

fn u16_be(b: &[u8], p: u64) -> Result<u16, String> {
    check_range(b, p, 2)?;
    let idx = p as usize;
    Ok(u16::from_be_bytes([b[idx], b[idx + 1]]))
}

fn u32_be(b: &[u8], p: u64) -> Result<u32, String> {
    check_range(b, p, 4)?;
    let idx = p as usize;
    Ok(u32::from_be_bytes([b[idx], b[idx + 1], b[idx + 2], b[idx + 3]]))
}

#[inline]
fn gim_colour_565(v: u16) -> u32 {
    let r = ((v >> 11) & 31) as u8;
    let g = ((v >> 5) & 63) as u8;
    let b = (v & 31) as u8;
    argb(255, (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
}

#[inline]
fn gim_colour_5551(v: u16) -> u32 {
    let r = ((v >> 10) & 31) as u8;
    let g = ((v >> 5) & 31) as u8;
    let b = (v & 31) as u8;
    let a = if (v & 0x8000) != 0 { 255 } else { 0 };
    argb(a, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))
}

#[inline]
fn gim_colour_4444(v: u16) -> u32 {
    let r = (((v >> 12) & 15) as u8).wrapping_mul(17);
    let g = (((v >> 8) & 15) as u8).wrapping_mul(17);
    let b = (((v >> 4) & 15) as u8).wrapping_mul(17);
    let a = ((v & 15) as u8).wrapping_mul(17);
    argb(a, r, g, b)
}

struct GimPalette {
    colours: Vec<u32>,
}

fn read_gim_palette(b: &[u8], begin: u64, end: u64) -> Result<Option<GimPalette>, String> {
    let mut block = begin;
    let file_len = b.len() as u64;
    while block.saturating_add(16) <= end && block.saturating_add(16) <= file_len {
        let btype = u16_be(b, block)?;
        let size = u32_be(b, block + 4)? as u64;
        let data = u32_be(b, block + 12)? as u64;
        if size < 16 || data < 16 || data >= size || block.saturating_add(size) > end || block.saturating_add(size) > file_len {
            if btype == 5 {
                return Err("Invalid GIM palette block".to_string());
            }
            return Ok(None);
        }
        if btype != 5 {
            block += size;
            continue;
        }
        let info = block + data;
        check_range(b, info, 0x30)?;
        let format = u16_be(b, info + 4)?;
        let order = u16_be(b, info + 6)?;
        let count = (u16_be(b, info + 8)? as u64) * (u16_be(b, info + 10)? as u64);
        let entry_bytes: u64 = if format <= 2 {
            2
        } else if format == 3 {
            4
        } else {
            0
        };
        if entry_bytes == 0 {
            return Err(format!("Unsupported GIM palette format 0x{:x}", format));
        }
        if order != 0 {
            return Err("Unsupported GIM palette order".to_string());
        }
        if count == 0 || count > 65536 {
            return Err("Invalid GIM palette dimensions".to_string());
        }
        let start = info.saturating_add(u32_be(b, info + 0x1c)? as u64);
        let block_end = block + size;
        if start < info + 0x30 || start > block_end || count * entry_bytes > block_end - start {
            return Err("GIM palette exceeds its block".to_string());
        }
        check_range(b, start, count * entry_bytes)?;
        let mut colours = Vec::with_capacity(count as usize);
        for i in 0..count {
            let at = start + i * entry_bytes;
            if format == 3 {
                let idx = at as usize;
                colours.push(argb(b[idx + 3], b[idx], b[idx + 1], b[idx + 2]));
            } else {
                let val = u16_be(b, at)?;
                let col = match format {
                    0 => gim_colour_565(val),
                    1 => gim_colour_5551(val),
                    _ => gim_colour_4444(val),
                };
                colours.push(col);
            }
        }
        return Ok(Some(GimPalette { colours }));
    }
    Ok(None)
}

fn decode_indexed(
    bytes: &[u8],
    pixels: u64,
    end: u64,
    width: u32,
    height: u32,
    format: u16,
    alignment: u64,
    palette: Option<&GimPalette>,
) -> Result<Vec<u32>, String> {
    let bits: u64 = match format {
        4 => 4,
        5 => 8,
        _ => 16,
    };
    let row_bytes = ((width as u64) * bits + 7) / 8;
    let stride = ((row_bytes + alignment - 1) / alignment) * alignment;
    let length = stride * (height as u64);
    if pixels > end || length > end - pixels {
        return Err("GIM index rows exceed image block".to_string());
    }
    check_range(bytes, pixels, length)?;
    let mut out = vec![0u32; (width as usize) * (height as usize)];
    let entries = palette.map_or(0, |p| p.colours.len());
    for y in 0..height {
        let line_off = (pixels + (y as u64) * stride) as usize;
        let row_out = (y as usize) * (width as usize);
        for x in 0..width {
            let xu = x as usize;
            let index: usize = if format == 4 {
                let packed = bytes[line_off + (xu >> 1)] as usize;
                if (xu & 1) != 0 { packed & 15 } else { packed >> 4 }
            } else if format == 5 {
                bytes[line_off + xu] as usize
            } else {
                u16::from_be_bytes([bytes[line_off + xu * 2], bytes[line_off + xu * 2 + 1]]) as usize
            };
            if let Some(pal) = palette {
                if index >= entries {
                    return Err(format!("GIM palette index {} exceeds {} entries", index, entries));
                }
                out[row_out + xu] = pal.colours[index];
            } else {
                let coverage: u8 = if format == 4 {
                    (index as u8).wrapping_mul(17)
                } else if format == 5 {
                    index as u8
                } else {
                    (index >> 8) as u8
                };
                out[row_out + xu] = argb(coverage, 255, 255, 255);
            }
        }
    }
    Ok(out)
}

fn decode_blocks(
    bytes: &[u8],
    pixels: u64,
    end: u64,
    width: u32,
    height: u32,
    format: u16,
    alignment: u64,
) -> Result<Vec<u32>, String> {
    let block_bytes: u64 = if format == 8 { 8 } else { 16 };
    let row_bytes = (((width as u64) + 3) / 4) * block_bytes;
    let stride = ((row_bytes + alignment - 1) / alignment) * alignment;
    let length = stride * (((height as u64) + 3) / 4);
    if pixels > end || length > end - pixels {
        return Err("DXT blocks exceed GIM image".to_string());
    }
    check_range(bytes, pixels, length)?;
    let mut out = vec![0u32; (width as usize) * (height as usize)];
    for by in (0..height).step_by(4) {
        for bx in (0..width).step_by(4) {
            let block_off = (pixels + ((by / 4) as u64) * stride + ((bx / 4) as u64) * block_bytes) as usize;
            let block = &bytes[block_off..block_off + (block_bytes as usize)];
            let colours = if format == 8 { block } else { &block[8..16] };
            let c0 = u16::from_le_bytes([colours[0], colours[1]]);
            let c1 = u16::from_le_bytes([colours[2], colours[3]]);
            let mut palette = [gim_colour_565(c0), gim_colour_565(c1), 0, 0];
            if format != 8 || c0 > c1 {
                for i in 0..2u32 {
                    let w0 = 2 - i;
                    let w1 = 1 + i;
                    let r = ((w0 * (r_of(palette[0]) as u32) + w1 * (r_of(palette[1]) as u32)) / 3) as u8;
                    let g = ((w0 * (g_of(palette[0]) as u32) + w1 * (g_of(palette[1]) as u32)) / 3) as u8;
                    let b = ((w0 * (b_of(palette[0]) as u32) + w1 * (b_of(palette[1]) as u32)) / 3) as u8;
                    palette[(2 + i) as usize] = argb(255, r, g, b);
                }
            } else {
                let r = (((r_of(palette[0]) as u32) + (r_of(palette[1]) as u32)) / 2) as u8;
                let g = (((g_of(palette[0]) as u32) + (g_of(palette[1]) as u32)) / 2) as u8;
                let b = (((b_of(palette[0]) as u32) + (b_of(palette[1]) as u32)) / 2) as u8;
                palette[2] = argb(255, r, g, b);
                palette[3] = argb(0, 0, 0, 0);
            }
            let indices = u32::from_le_bytes([colours[4], colours[5], colours[6], colours[7]]);
            let mut alphas = [0u8; 8];
            let mut alpha_indices = 0u64;
            if format == 10 {
                alphas[0] = block[0];
                alphas[1] = block[1];
                let a0 = alphas[0] as u32;
                let a1 = alphas[1] as u32;
                if a0 > a1 {
                    for i in 1..=6u32 {
                        alphas[(i + 1) as usize] = (((7 - i) * a0 + i * a1) / 7) as u8;
                    }
                } else {
                    for i in 1..=4u32 {
                        alphas[(i + 1) as usize] = (((5 - i) * a0 + i * a1) / 5) as u8;
                    }
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                for i in 0..6 {
                    alpha_indices |= (block[2 + i] as u64) << (i * 8);
                }
            }
            for y in 0..4u32 {
                if by + y >= height {
                    break;
                }
                for x in 0..4u32 {
                    if bx + x >= width {
                        break;
                    }
                    let pixel = (y * 4 + x) as usize;
                    let colour = palette[((indices >> (pixel * 2)) & 3) as usize];
                    let alpha = if format == 10 {
                        alphas[((alpha_indices >> (pixel * 3)) & 7) as usize]
                    } else if format == 9 {
                        ((block[pixel / 2] >> ((pixel & 1) * 4)) & 15).wrapping_mul(17)
                    } else {
                        a_of(colour)
                    };
                    let dst = ((by + y) as usize) * (width as usize) + ((bx + x) as usize);
                    out[dst] = argb(alpha, r_of(colour), g_of(colour), b_of(colour));
                }
            }
        }
    }
    Ok(out)
}

fn pack_argb(width: u32, height: u32, pixels: &[u32]) -> DecodedImage {
    let mut argb_bytes = Vec::with_capacity(pixels.len() * 4);
    for &px in pixels {
        argb_bytes.extend_from_slice(&px.to_ne_bytes());
    }
    DecodedImage {
        width,
        height,
        argb_bytes,
    }
}

pub fn decode_gim(b: &[u8]) -> Result<DecodedImage, String> {
    if !b.starts_with(b".GIM1.00") {
        return Err("Unsupported GIM byte order".to_string());
    }
    let mut at: u64 = 0x10;
    let mut picture_data: u64 = 0;
    let mut picture_end: u64 = b.len() as u64;
    for _ in 0..128 {
        let btype = u16_be(b, at)?;
        let size = u32_be(b, at + 4)? as u64;
        let data = u32_be(b, at + 12)? as u64;
        if size < 16 || data < 16 || data >= size {
            return Err("Invalid GIM block".to_string());
        }
        check_range(b, at, size)?;
        if btype == 2 || btype == 3 {
            if btype == 3 {
                picture_data = at + data;
                picture_end = at + size;
            }
            at += data;
            continue;
        }
        if btype != 4 {
            at += size;
            continue;
        }
        let info = at + data;
        check_range(b, info, 0x30)?;
        let format = u16_be(b, info + 4)?;
        let order = u16_be(b, info + 6)?;
        let w = u16_be(b, info + 8)? as u32;
        let h = u16_be(b, info + 10)? as u32;
        let alignment = u16_be(b, info + 14)? as u64;
        if order != 0 {
            return Err("Unsupported GIM pixel order".to_string());
        }
        if w == 0 || h == 0 || w > 4096 || h > 4096 || alignment == 0 || alignment > 4096 {
            return Err("Invalid GIM dimensions".to_string());
        }
        let pixels = info + (u32_be(b, info + 0x1c)? as u64);
        if pixels < info + 0x30 {
            return Err("GIM pixels overlap image metadata".to_string());
        }
        let block_end = at + size;
        if (8..=10).contains(&format) {
            let decoded = decode_blocks(b, pixels, block_end, w, h, format, alignment)?;
            return Ok(pack_argb(w, h, &decoded));
        }
        if (4..=6).contains(&format) {
            let pal_start = if picture_data != 0 { picture_data } else { block_end };
            let palette = read_gim_palette(b, pal_start, picture_end)?;
            let decoded = decode_indexed(b, pixels, block_end, w, h, format, alignment, palette.as_ref())?;
            return Ok(pack_argb(w, h, &decoded));
        }
        if format != 3 {
            return Err(format!("Unsupported GIM pixel format 0x{:x}", format));
        }
        let stride = (((w as u64) * 4 + alignment - 1) / alignment) * alignment;
        if pixels < info || pixels.saturating_add(stride * (h as u64)) > block_end {
            return Err("GIM pixels exceed image block".to_string());
        }
        check_range(b, pixels, stride * (h as u64))?;
        let mut out = Vec::with_capacity((w as usize) * (h as usize));
        for y in 0..h {
            let row_off = pixels + (y as u64) * stride;
            for x in 0..w {
                let v = u32_be(b, row_off + (x as u64) * 4)?;
                out.push(argb((v & 0xff) as u8, (v >> 24) as u8, ((v >> 16) & 0xff) as u8, ((v >> 8) & 0xff) as u8));
            }
        }
        return Ok(pack_argb(w, h, &out));
    }
    Err("GIM image block not found".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_rgba_gim(w: u16, h: u16, rgba_pixels: &[u8]) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(b".GIM1.00\0\0\0\0\0\0\0\0");
        let img_payload_len = 0x30 + rgba_pixels.len();
        let img_block_size = (16 + img_payload_len) as u32;
        // Root block (type 2) + Image block (type 4)
        let root_size = 16 + img_block_size;
        b.extend_from_slice(&2u16.to_be_bytes());
        b.extend_from_slice(&0u16.to_be_bytes());
        b.extend_from_slice(&root_size.to_be_bytes());
        b.extend_from_slice(&16u32.to_be_bytes());
        b.extend_from_slice(&16u32.to_be_bytes());
        // Image block (type 4)
        b.extend_from_slice(&4u16.to_be_bytes());
        b.extend_from_slice(&0u16.to_be_bytes());
        b.extend_from_slice(&img_block_size.to_be_bytes());
        b.extend_from_slice(&16u32.to_be_bytes());
        b.extend_from_slice(&16u32.to_be_bytes());
        // 0x30 image info header
        let mut info = [0u8; 0x30];
        info[4..6].copy_from_slice(&3u16.to_be_bytes()); // format 3 = RGBA8888
        info[6..8].copy_from_slice(&0u16.to_be_bytes()); // order 0
        info[8..10].copy_from_slice(&w.to_be_bytes());
        info[10..12].copy_from_slice(&h.to_be_bytes());
        info[14..16].copy_from_slice(&4u16.to_be_bytes()); // alignment 4
        info[0x1c..0x20].copy_from_slice(&0x30u32.to_be_bytes()); // pixel offset
        b.extend_from_slice(&info);
        b.extend_from_slice(rgba_pixels);
        b
    }

    #[test]
    fn decodes_rgba8888_gim() {
        let gim = make_rgba_gim(2, 1, &[0x10, 0x20, 0x30, 0x40, 0xff, 0x00, 0x80, 0xff]);
        let img = decode_gim(&gim).expect("RGBA8888 GIM should decode");
        assert_eq!((img.width, img.height), (2, 1));
        let p0 = u32::from_ne_bytes(img.argb_bytes[0..4].try_into().unwrap());
        let p1 = u32::from_ne_bytes(img.argb_bytes[4..8].try_into().unwrap());
        assert_eq!(p0, argb(0x40, 0x10, 0x20, 0x30));
        assert_eq!(p1, argb(0xff, 0xff, 0x00, 0x80));
    }

    #[test]
    fn rejects_invalid_magic() {
        assert!(decode_gim(b"MIG.00.1PSP\0\0\0\0\0").is_err());
    }
}

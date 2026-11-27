#![forbid(unsafe_code)]

const MAX_GTF_DIM: u32 = 16384;
const MAX_GTF_PIXELS: u64 = 64 * 1024 * 1024;

#[inline]
fn be16(d: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([d[o], d[o + 1]])
}

#[inline]
fn be32(d: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

#[inline]
fn le16(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}

#[inline]
fn le32(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

#[inline]
const fn qrgba(r: u32, g: u32, b: u32, a: u32) -> u32 {
    ((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff)
}

#[inline]
const fn qrgb(r: u32, g: u32, b: u32) -> u32 {
    qrgba(r, g, b, 0xff)
}

#[inline]
const fn qred(c: u32) -> u32 {
    (c >> 16) & 0xff
}

#[inline]
const fn qgreen(c: u32) -> u32 {
    (c >> 8) & 0xff
}

#[inline]
const fn qblue(c: u32) -> u32 {
    c & 0xff
}

#[inline]
const fn qalpha(c: u32) -> u32 {
    (c >> 24) & 0xff
}

fn morton(x: u32, y: u32, width: u32, height: u32) -> u64 {
    let mut index = 0u64;
    let mut bit = 1u64;
    let mut mask = 1u32;
    while mask < width || mask < height {
        if mask < width {
            if (x & mask) != 0 {
                index |= bit;
            }
            bit <<= 1;
        }
        if mask < height {
            if (y & mask) != 0 {
                index |= bit;
            }
            bit <<= 1;
        }
        mask <<= 1;
    }
    index
}

fn colour565(value: u16) -> u32 {
    let r = u32::from(value >> 11);
    let g = u32::from((value >> 5) & 63);
    let b = u32::from(value & 31);
    qrgb((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
}

pub struct DecodedGtf {
    pub width: u32,
    pub height: u32,
    pub argb_bytes: Vec<u8>,
}

pub fn decode_gtf(gtf: &[u8]) -> Result<DecodedGtf, String> {
    if gtf.len() < 0x30 {
        return Err("truncated GTF header".to_string());
    }
    if be32(gtf, 8) == 0 {
        return Err("GTF has no textures".to_string());
    }
    let offset = u64::from(be32(gtf, 0x10));
    let declared = u64::from(be32(gtf, 0x14));
    let code = gtf[0x18];
    let base = code & !0x60;
    let linear = (code & 0x20) != 0;
    let width = u32::from(be16(gtf, 0x20));
    let height = u32::from(be16(gtf, 0x22));
    let pitch = u64::from(be32(gtf, 0x28));
    if width == 0 || height == 0 {
        return Err("invalid GTF dimensions".to_string());
    }
    if width > MAX_GTF_DIM || height > MAX_GTF_DIM || u64::from(width) * u64::from(height) > MAX_GTF_PIXELS {
        return Err("cannot allocate GTF image".to_string());
    }
    let b8 = base == 0x81;
    let argb = base == 0x82 || base == 0x85;
    let dxt = (0x86..=0x88).contains(&base);
    if !b8 && !argb && !dxt {
        return Err(format!("unsupported GTF format 0x{:02x}", code));
    }
    let end = gtf.len() as u64;
    if offset > end || declared > end - offset {
        return Err("GTF texture exceeds file".to_string());
    }
    let available = declared;
    let stride: u64;
    let required: u64;
    if dxt {
        let row = u64::from((width + 3) / 4) * if base == 0x86 { 8 } else { 16 };
        stride = if linear && pitch != 0 { pitch } else { row };
        if stride < row {
            return Err("invalid DXT pitch".to_string());
        }
        required = stride * u64::from((height + 3) / 4);
    } else if linear {
        let row = u64::from(width) * if b8 { 1 } else { 4 };
        stride = if pitch != 0 { pitch } else { row };
        if stride < row {
            return Err("invalid GTF pitch".to_string());
        }
        required = stride * u64::from(height);
    } else {
        if !width.is_power_of_two() || !height.is_power_of_two() {
            return Err("swizzled GTF dimensions must be powers of two".to_string());
        }
        required = u64::from(width) * u64::from(height) * if b8 { 1 } else { 4 };
        stride = 0;
    }
    if required > available {
        return Err("truncated GTF pixels".to_string());
    }

    let pixels = &gtf[offset as usize..(offset + declared) as usize];
    let total_px = (width as usize) * (height as usize);
    let mut out = vec![0u8; total_px * 4];

    if !dxt {
        let bpp = if b8 { 1u64 } else { 4u64 };
        for y in 0..height {
            let row_off = (y as usize) * (width as usize);
            for x in 0..width {
                let pos = if linear {
                    u64::from(y) * stride + u64::from(x) * bpp
                } else {
                    morton(x, y, width, height) * bpp
                } as usize;
                let px = if b8 {
                    let v = u32::from(pixels[pos]);
                    qrgba(v, v, v, v)
                } else {
                    qrgba(
                        u32::from(pixels[pos + 1]),
                        u32::from(pixels[pos + 2]),
                        u32::from(pixels[pos + 3]),
                        u32::from(pixels[pos]),
                    )
                };
                let dst = (row_off + x as usize) * 4;
                out[dst..dst + 4].copy_from_slice(&px.to_ne_bytes());
            }
        }
        return Ok(DecodedGtf {
            width,
            height,
            argb_bytes: out,
        });
    }

    let block_bytes = if base == 0x86 { 8u64 } else { 16u64 };
    let mut by = 0u32;
    while by < height {
        let mut bx = 0u32;
        while bx < width {
            let b_off = (u64::from(by / 4) * stride + u64::from(bx / 4) * block_bytes) as usize;
            let block = &pixels[b_off..b_off + block_bytes as usize];
            let colours = if base == 0x86 { block } else { &block[8..] };
            let c0 = le16(colours, 0);
            let c1 = le16(colours, 2);
            let mut palette = [colour565(c0), colour565(c1), 0, 0];
            if base != 0x86 || c0 > c1 {
                for i in 0..2u32 {
                    palette[2 + i as usize] = qrgb(
                        ((2 - i) * qred(palette[0]) + (1 + i) * qred(palette[1])) / 3,
                        ((2 - i) * qgreen(palette[0]) + (1 + i) * qgreen(palette[1])) / 3,
                        ((2 - i) * qblue(palette[0]) + (1 + i) * qblue(palette[1])) / 3,
                    );
                }
            } else {
                palette[2] = qrgb(
                    (qred(palette[0]) + qred(palette[1])) / 2,
                    (qgreen(palette[0]) + qgreen(palette[1])) / 2,
                    (qblue(palette[0]) + qblue(palette[1])) / 2,
                );
                palette[3] = qrgba(0, 0, 0, 0);
            }
            let indices = le32(colours, 4);
            let mut alphas = [0u32; 8];
            let mut alpha_indices = 0u64;
            if base == 0x88 {
                alphas[0] = u32::from(block[0]);
                alphas[1] = u32::from(block[1]);
                if alphas[0] > alphas[1] {
                    for i in 1..=6u32 {
                        alphas[1 + i as usize] = ((7 - i) * alphas[0] + i * alphas[1]) / 7;
                    }
                } else {
                    for i in 1..=4u32 {
                        alphas[1 + i as usize] = ((5 - i) * alphas[0] + i * alphas[1]) / 5;
                    }
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                for i in 0..6usize {
                    alpha_indices |= u64::from(block[2 + i]) << (i * 8);
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
                    let n = (y * 4 + x) as usize;
                    let c = palette[((indices >> (n * 2)) & 3) as usize];
                    let a = if base == 0x88 {
                        alphas[((alpha_indices >> (n * 3)) & 7) as usize]
                    } else if base == 0x87 {
                        u32::from((block[n / 2] >> ((n & 1) * 4)) & 15) * 17
                    } else {
                        qalpha(c)
                    };
                    let px = qrgba(qred(c), qgreen(c), qblue(c), a);
                    let dst = (((by + y) as usize) * (width as usize) + (bx + x) as usize) * 4;
                    out[dst..dst + 4].copy_from_slice(&px.to_ne_bytes());
                }
            }
            bx += 4;
        }
        by += 4;
    }

    Ok(DecodedGtf {
        width,
        height,
        argb_bytes: out,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_short_header() {
        assert_eq!(decode_gtf(&[0u8; 16]).err().as_deref(), Some("truncated GTF header"));
    }
}

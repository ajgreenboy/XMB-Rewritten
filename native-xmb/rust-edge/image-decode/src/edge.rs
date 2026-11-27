#![forbid(unsafe_code)]

const D1: [u8; 24] = [
    0x02, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x01,
    0x0c, 0x00, 0x00, 0x00, 0x0c, 0x06, 0x01, 0x02, 0x04, 0x02, 0x00, 0x00,
];
const D2: [u8; 16] = [
    0x01, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x05,
    0x04, 0x08, 0x00, 0x00,
];

#[inline]
fn valid(b: &[u8], o: u64, n: u64) -> bool {
    let end = b.len() as u64;
    o <= end && n <= end - o
}

#[inline]
fn u16_be(b: &[u8], o: u64) -> u16 {
    let i = o as usize;
    u16::from_be_bytes([b[i], b[i + 1]])
}

#[inline]
fn u32_be(b: &[u8], o: u64) -> u32 {
    let i = o as usize;
    u32::from_be_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

fn half_to_f32(h: u16) -> f32 {
    let e = i32::from((h >> 10) & 31);
    let m = u32::from(h & 1023);
    let s = if (h >> 15) != 0 { -1.0f32 } else { 1.0f32 };
    if e == 31 {
        if m != 0 {
            f32::NAN
        } else {
            s * f32::INFINITY
        }
    } else if e != 0 {
        s * (1.0f32 + (m as f32) / 1024.0f32) * 2.0f32.powi(e - 15)
    } else {
        s * (m as f32) * 2.0f32.powi(-24)
    }
}

fn snorm(v: u32, bits: u32) -> f32 {
    let sign = 1i32 << (bits - 1);
    let mask = (1u32 << bits) - 1;
    let n = (v & mask) as i32;
    (((n ^ sign) - sign) as f32) / ((sign - 1) as f32)
}

pub fn decode_edge_packed(b: &[u8]) -> Result<Vec<u8>, String> {
    if !valid(b, 0, 8) {
        return Err("truncated Edge header".to_string());
    }
    let seg_count = u32_be(b, 0);
    if seg_count == 0 || seg_count > 64 {
        return Err("unsupported Edge segment count".to_string());
    }
    let base_seg = u64::from(u32_be(b, 4));

    let mut total_vertices = 0u32;
    let mut total_indices = 0u32;
    let mut vertex_bytes: Vec<u8> = Vec::new();
    let mut all_indices: Vec<u32> = Vec::new();

    for s in 0..seg_count {
        let seg = base_seg + u64::from(s) * 0x78;
        if !valid(b, seg, 0x78) {
            return Err("truncated segment header".to_string());
        }
        let count = u32::from(u16_be(b, seg + 8));
        let index_count = u32::from(u16_be(b, seg + 10));
        let index = u64::from(u32_be(b, seg + 0x10));
        let index_size = u64::from(u16_be(b, seg + 0x14));
        let pos = u64::from(u32_be(b, seg + 0x18));
        let normal = u64::from(u32_be(b, seg + 0x1c));
        let uv = u64::from(u32_be(b, seg + 0x2c));
        let desc = u64::from(u32_be(b, seg + 0x70));
        let uv_desc = u64::from(u32_be(b, seg + 0x74));

        if s > 0 && (count == 0 || index_count == 0 || desc == 0 || uv_desc == 0) {
            return Err("invalid or unpopulated Edge segment".to_string());
        }

        if !valid(b, desc, D1.len() as u64)
            || !valid(b, uv_desc, D2.len() as u64)
            || b[desc as usize..desc as usize + D1.len()] != D1
            || b[uv_desc as usize..uv_desc as usize + D2.len()] != D2
        {
            return Err("unsupported vertex stream format".to_string());
        }
        if count == 0
            || index_count == 0
            || index_count % 3 != 0
            || !valid(b, pos, u64::from(count) * 12)
            || !valid(b, normal, u64::from(count) * 4)
            || !valid(b, uv, u64::from(count) * 4)
        {
            return Err("invalid or truncated vertex streams".to_string());
        }
        if index_size < 8 || !valid(b, index, index_size) {
            return Err("invalid index stream bounds".to_string());
        }

        let delta_count = u32::from(u16_be(b, index));
        let bias = u32::from(u16_be(b, index + 2));
        let one_bytes = u32::from(u16_be(b, index + 4));
        let bits = u32::from(b[(index + 6) as usize]);
        let triangles = index_count / 3;
        let two_bytes = (triangles * 2 + 7) / 8;
        let delta_bytes = (delta_count * bits + 7) / 8;
        if bits > 16
            || b[(index + 7) as usize] != 0
            || 8u64 + u64::from(one_bytes) + u64::from(two_bytes) + u64::from(delta_bytes) > index_size
        {
            return Err("invalid compressed index header".to_string());
        }

        let one = index + 8;
        let two = one + u64::from(one_bytes);
        let nbit = two + u64::from(two_bytes);
        let mut delta = Vec::with_capacity(delta_count as usize);
        for i in 0..delta_count {
            let mut value = 0u32;
            for j in 0..bits {
                let bit = i * bits + j;
                let byte = b[(nbit + u64::from(bit / 8)) as usize];
                value = (value << 1) | u32::from((byte >> (7 - (bit % 8))) & 1);
            }
            let mut v = (value.wrapping_sub(bias)) as u16;
            if i >= 8 {
                v = v.wrapping_add(delta[(i - 8) as usize]);
            }
            delta.push(v);
        }

        let base_vertex = total_vertices;
        let mut sequential = 0u32;
        let mut explicit_index = 0u32;
        let mut selector = 0u32;
        let mut tri = [0u32; 3];

        let mut next = |out: &mut u32| -> bool {
            if selector >= one_bytes * 8 {
                return false;
            }
            let byte = b[(one + u64::from(selector / 8)) as usize];
            let explicit_val = (byte & (0x80u8 >> (selector % 8))) != 0;
            selector += 1;
            if explicit_val {
                if explicit_index >= delta_count {
                    return false;
                }
                *out = u32::from(delta[explicit_index as usize]);
                explicit_index += 1;
            } else {
                *out = sequential;
                sequential += 1;
            }
            *out < count
        };

        for i in 0..triangles {
            let byte = b[(two + u64::from(i / 4)) as usize];
            let code = (byte >> (6 - 2 * (i % 4))) & 3;
            if i == 0 && code != 3 {
                return Err("first triangle reuses an unavailable edge".to_string());
            }
            if code == 0 {
                tri[1] = tri[2];
            } else if code == 1 {
                tri[0] = tri[2];
            } else if code == 2 {
                tri.swap(0, 1);
            }
            if code == 3 && (!next(&mut tri[0]) || !next(&mut tri[1])) {
                return Err("invalid index selector stream".to_string());
            }
            if !next(&mut tri[2]) {
                return Err("invalid index selector stream".to_string());
            }
            all_indices.push(base_vertex + tri[0]);
            all_indices.push(base_vertex + tri[1]);
            all_indices.push(base_vertex + tri[2]);
        }

        vertex_bytes.reserve((count as usize) * 32);
        for i in 0..count {
            let p_off = pos + u64::from(i) * 12;
            let px = f32::from_bits(u32_be(b, p_off));
            let py = f32::from_bits(u32_be(b, p_off + 4));
            let pz = f32::from_bits(u32_be(b, p_off + 8));
            let n = u32_be(b, normal + u64::from(i) * 4);
            let nx = snorm(n, 11);
            let ny = snorm(n >> 11, 11);
            let nz = snorm(n >> 22, 10);
            let u_off = uv + u64::from(i) * 4;
            let tu = half_to_f32(u16_be(b, u_off));
            let tv = half_to_f32(u16_be(b, u_off + 2));
            for f in [px, py, pz, nx, ny, nz, tu, tv] {
                vertex_bytes.extend_from_slice(&f.to_le_bytes());
            }
        }
        total_vertices += count;
        total_indices += index_count;
    }

    let mut out = Vec::with_capacity(8 + vertex_bytes.len() + all_indices.len() * 4);
    out.extend_from_slice(&total_vertices.to_le_bytes());
    out.extend_from_slice(&total_indices.to_le_bytes());
    out.extend_from_slice(&vertex_bytes);
    for idx in all_indices {
        out.extend_from_slice(&idx.to_le_bytes());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_short_edge() {
        assert_eq!(decode_edge_packed(&[0u8; 4]).err().as_deref(), Some("truncated Edge header"));
    }
}

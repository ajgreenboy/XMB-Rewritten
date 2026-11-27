//! Safe Rust decoder for PlayStation ADPCM VAG (`.vag` / `VAGp`) audio streams to 16-bit PCM RIFF WAVE
//! (`P3t::vagToWav` in `native-xmb/src/p3t.cpp`).
//!
//! Zero `unsafe`.

#![forbid(unsafe_code)]

const K_FILTERS: [[f64; 2]; 5] = [
    [0.0, 0.0],
    [60.0, 0.0],
    [115.0, -52.0],
    [98.0, -55.0],
    [122.0, -60.0],
];

// Hard bound on decoded samples per channel (16M samples = ~6 minutes at 44.1 kHz, 32 MiB PCM)
// so a huge untrusted stream cannot exhaust heap memory.
const MAX_SAMPLES_PER_CHANNEL: usize = 16 * 1024 * 1024;

fn be32(d: &[u8], at: usize) -> u32 {
    if at + 4 <= d.len() {
        u32::from_be_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
    } else {
        0xFFFF_FFFF
    }
}

fn decode_channel(v: &[u8]) -> (Vec<i16>, u32) {
    let mut p = 0usize;
    let mut rate = 44100u32;
    if v.starts_with(b"VAGp") {
        let r = be32(v, 16);
        if r != 0 && r != 0xFFFF_FFFF {
            rate = r;
        }
        p = 48;
    }
    let mut s1 = 0.0f64;
    let mut s2 = 0.0f64;
    let mut out = Vec::new();
    while p + 16 <= v.len() && out.len() + 28 <= MAX_SAMPLES_PER_CHANNEL {
        let b = &v[p..p + 16];
        p += 16;
        if b[1] == 7 {
            break; // end of the sound
        }
        let shift = (b[0] & 15).min(12) as i32;
        let c = K_FILTERS[((b[0] >> 4) as usize).min(4)];
        for i in 0..28usize {
            let nibble = if (i & 1) != 0 {
                (b[2 + i / 2] >> 4) & 15
            } else {
                b[2 + i / 2] & 15
            };
            let raw = (((nibble as u16) << 12) as i16) >> 4; // sign-extended, scaled to 12 bits
            let predicted = (((raw as i32) * 16) >> shift) as f64 + (s1 * c[0] + s2 * c[1]) / 64.0;
            // Match C++ `std::round` (round half away from zero) and `std::clamp(-32768., 32767.)`.
            let x = predicted.round().clamp(-32768.0, 32767.0);
            out.push(x as i16);
            s2 = s1;
            s1 = x;
        }
        if b[1] == 1 {
            break; // last block, no loop
        }
    }
    (out, rate)
}

pub fn vag_to_wav(left: &[u8], right: &[u8]) -> Vec<u8> {
    let (l, rate) = decode_channel(left);
    if l.is_empty() {
        return Vec::new();
    }
    let r = if right.is_empty() {
        Vec::new()
    } else {
        decode_channel(right).0
    };
    let channels: u16 = if r.is_empty() { 1 } else { 2 };
    let frames = if r.is_empty() { l.len() } else { l.len().min(r.len()) };
    let Some(data_bytes_usize) = frames.checked_mul(channels as usize).and_then(|n| n.checked_mul(2)) else {
        return Vec::new();
    };
    let Ok(bytes) = u32::try_from(data_bytes_usize) else {
        return Vec::new();
    };
    let mut w = Vec::with_capacity(44 + data_bytes_usize);
    w.extend_from_slice(b"RIFF");
    w.extend_from_slice(&36u32.saturating_add(bytes).to_le_bytes());
    w.extend_from_slice(b"WAVEfmt ");
    w.extend_from_slice(&16u32.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes());
    w.extend_from_slice(&channels.to_le_bytes());
    w.extend_from_slice(&rate.to_le_bytes());
    w.extend_from_slice(&rate.saturating_mul((channels as u32) * 2).to_le_bytes());
    w.extend_from_slice(&(channels * 2).to_le_bytes());
    w.extend_from_slice(&16u16.to_le_bytes());
    w.extend_from_slice(b"data");
    w.extend_from_slice(&bytes.to_le_bytes());
    for i in 0..frames {
        w.extend_from_slice(&(l[i] as u16).to_le_bytes());
        if channels == 2 {
            w.extend_from_slice(&(r[i] as u16).to_le_bytes());
        }
    }
    w
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_vagp_mono_and_stereo_blocks() {
        let mut vag = vec![0u8; 48];
        vag[0..4].copy_from_slice(b"VAGp");
        vag[16..20].copy_from_slice(&48000u32.to_be_bytes());
        let mut block = [0u8; 16];
        block[0] = 0x12; // filter 1, shift 2
        block[1] = 1;    // end after this block
        for i in 0..14 {
            block[2 + i] = ((i as u8) & 15) | ((((15 - i) as u8) & 15) << 4);
        }
        vag.extend_from_slice(&block);
        let wav_mono = vag_to_wav(&vag, &[]);
        assert_eq!(&wav_mono[0..4], b"RIFF");
        assert_eq!(wav_mono.len(), 44 + 28 * 2);
        let wav_stereo = vag_to_wav(&vag, &vag);
        assert_eq!(wav_stereo.len(), 44 + 28 * 4);
    }
}

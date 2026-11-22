//! Pure-Rust implementations of the three firmware crypto primitives.
//!
//! These are the byte-for-byte parity targets for the C++ reference in
//! `native-xmb/src/firmware_crypto.cpp`. The AES *mode* loops (CTR / CBC) are
//! implemented manually so the control flow — including the big-endian 16-byte
//! CTR counter increment and the CBC IV-chaining order — matches the C++
//! exactly. Only the raw AES block cipher (and HMAC-SHA1) come from RustCrypto.
//!
//! The C++ reference ports its AES and key schedule from RPCS3/PolarSSL; it is
//! a standard FIPS-197 AES, so RustCrypto's `aes::Aes128`/`Aes256` produce
//! byte-identical block outputs.

use aes::cipher::consts::U16;
use aes::cipher::{Array, BlockCipherDecrypt, BlockCipherEncrypt, KeyInit};
use aes::{Aes128, Aes256};
use hmac::{Hmac, Mac};
use sha1::Sha1;

type HmacSha1 = Hmac<Sha1>;

/// HMAC-SHA1 (RFC 2104).
///
/// Matches the C++ `hmacSha1`: keys longer than the 64-byte block size are
/// pre-hashed with SHA-1, then zero-padded to 64 bytes; the result is the
/// standard inner/outer XOR + double hash.
///
/// Returns the 20-byte MAC.
pub fn hmac_sha1(key: &[u8], data: &[u8]) -> Vec<u8> {
    let mut mac = HmacSha1::new_from_slice(key).expect("HMAC accepts any key length");
    mac.update(data);
    mac.finalize().into_bytes().to_vec()
}

/// AES-128-CTR, in place.
///
/// `key` and `iv` must both be 16 bytes. The 16-byte counter is a big-endian
/// integer incremented after each block (matching the C++ reference's
/// least-significant-byte-first carry loop). The final (possibly partial) block
/// uses only the leading bytes of the keystream block.
///
/// Returns `false` (and leaves `data` untouched) on invalid arguments.
pub fn aes_ctr(data: &mut [u8], key: &[u8], iv: &[u8]) -> bool {
    if key.len() != 16 || iv.len() != 16 {
        return false;
    }
    let cipher = Aes128::new_from_slice(key).expect("16-byte key validated");
    let mut counter: [u8; 16] = iv.try_into().expect("16-byte iv validated");

    for at in (0..data.len()).step_by(16) {
        let mut stream: Array<u8, U16> = Array::from(counter);
        cipher.encrypt_block(&mut stream);

        let n = (data.len() - at).min(16);
        for i in 0..n {
            data[at + i] ^= stream[i];
        }

        // Big-endian increment of the 16-byte counter (carry propagates from
        // the least-significant byte, index 15, toward index 0).
        for i in (0..16).rev() {
            counter[i] = counter[i].wrapping_add(1);
            if counter[i] != 0 {
                break;
            }
        }
    }
    true
}

/// AES-128/256-CBC, in place, raw (no padding).
///
/// `key` must be 16 or 32 bytes, `iv` 16 bytes, and `data.len()` a multiple of
/// 16. On encrypt each block is XORed with the IV before encryption and the
/// resulting ciphertext becomes the next IV; on decrypt the block is decrypted
/// then XORed with the IV, and the *previous* ciphertext becomes the next IV.
/// This mirrors the C++ control flow exactly.
///
/// Returns `false` (and leaves `data` untouched) on invalid arguments.
pub fn aes_cbc(data: &mut [u8], key: &[u8], iv: &[u8], encrypt: bool) -> bool {
    if (key.len() != 16 && key.len() != 32) || iv.len() != 16 || data.len() % 16 != 0 {
        return false;
    }
    match key.len() {
        16 => {
            let cipher = Aes128::new_from_slice(key).expect("16-byte key validated");
            cbc_loop!(data, iv, encrypt, cipher);
        }
        32 => {
            let cipher = Aes256::new_from_slice(key).expect("32-byte key validated");
            cbc_loop!(data, iv, encrypt, cipher);
        }
        _ => unreachable!("key length already validated"),
    }
    true
}

/// Concrete CBC block loop. Written as a macro (rather than a generic helper)
/// so the block array `Array<u8, U16>` resolves against the concrete `Aes128`
/// / `Aes256` block-size type directly.
macro_rules! cbc_loop {
    ($data:expr, $iv:expr, $encrypt:expr, $cipher:expr) => {{
        let mut state: [u8; 16] = ($iv).try_into().expect("16-byte iv validated");
        for at in (0..($data).len()).step_by(16) {
            let mut block: [u8; 16] = ($data)[at..at + 16].try_into().expect("16-byte block");
            let saved = block;

            if $encrypt {
                for i in 0..16 {
                    block[i] ^= state[i];
                }
            }

            let mut b: Array<u8, U16> = Array::from(block);
            if $encrypt {
                ($cipher).encrypt_block(&mut b);
            } else {
                ($cipher).decrypt_block(&mut b);
            }
            let mut out: [u8; 16] = b.into();

            if !$encrypt {
                for i in 0..16 {
                    out[i] ^= state[i];
                }
            }

            ($data)[at..at + 16].copy_from_slice(&out);
            state = if $encrypt { out } else { saved };
        }
    }};
}
use cbc_loop;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ctr_empty_is_noop_and_valid() {
        let mut data: Vec<u8> = vec![];
        assert!(aes_ctr(&mut data, &[0u8; 16], &[0u8; 16]));
        assert!(data.is_empty());
    }

    #[test]
    fn ctr_rejects_bad_lengths() {
        let mut data = vec![0u8; 16];
        assert!(!aes_ctr(&mut data, &[0u8; 15], &[0u8; 16]));
        assert!(!aes_ctr(&mut data, &[0u8; 16], &[0u8; 15]));
    }

    #[test]
    fn cbc_rejects_bad_lengths() {
        let mut key_bad = vec![0u8; 24];
        assert!(!aes_cbc(&mut key_bad, &[0u8; 24], &[0u8; 16], true));
        let mut iv_bad = vec![0u8; 16];
        assert!(!aes_cbc(&mut iv_bad, &[0u8; 16], &[0u8; 15], true));
        let mut unaligned = vec![0u8; 15];
        assert!(!aes_cbc(&mut unaligned, &[0u8; 16], &[0u8; 16], true));
    }
}

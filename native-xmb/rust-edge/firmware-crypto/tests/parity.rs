//! Byte-parity and known-answer tests for the `firmware-crypto` pure functions.
//!
//! Two layers:
//!
//! 1. **Known-answer vectors** (NIST / RFC) — prove the Rust implementation is
//!    *standard* (not merely mutually-consistent with the C++ reference).
//! 2. **Captured C++ parity vectors** — the hex output of
//!    `native-xmb/src/firmware_crypto.cpp`, captured by `parity/build.sh` into
//!    `parity/outputs.txt`. These prove byte-for-byte parity with the C++
//!    reference over the exact same inputs.

use firmware_crypto::{aes_cbc, aes_ctr, hmac_sha1};

/// Deterministic byte sequence: `n` bytes starting at `start`, wrapping at
/// 0xff. Mirrors the `seq()` helper in `parity/harness.cpp`.
fn seq(n: usize, start: u8) -> Vec<u8> {
    (0..n).map(|i| start.wrapping_add(i as u8)).collect()
}

fn hx(s: &str) -> Vec<u8> {
    hex::decode(s).expect("valid hex literal")
}

// ---------------------------------------------------------------------------
// Known-answer vectors (both implementations are standard)
// ---------------------------------------------------------------------------

#[test]
fn known_answer_hmac_sha1_rfc2202() {
    // RFC 2202 HMAC-SHA1 test vectors.
    let cases: &[(&[u8], &[u8], &str)] = &[
        (
            &[0x0b; 20],
            b"Hi There",
            "b617318655057264e28bc0b6fb378c8ef146be00",
        ),
        (
            b"Jefe",
            b"what do ya want for nothing?",
            "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
        ),
        (
            &[0xaa; 20],
            &[0xdd; 50],
            "125d7342b9ac11cd91a39af48aa17b4f63f175d3",
        ),
    ];
    for (key, data, expected) in cases {
        let mac = hmac_sha1(key, data);
        assert_eq!(hex::encode(&mac), *expected, "RFC 2202 HMAC-SHA1 mismatch");
    }
}

#[test]
fn known_answer_aes_block_fips197() {
    // FIPS-197 Appendix C.1 (AES-128) and C.3 (AES-256). A single CBC block
    // with a zero IV is exactly the raw block cipher, so this also pins the
    // block-cipher output (not just the mode loop).
    let key128 = hx("000102030405060708090a0b0c0d0e0f");
    let pt = hx("00112233445566778899aabbccddeeff");
    let ct128 = hx("69c4e0d86a7b0430d8cdb78070b4c55a");

    let mut block = pt.clone();
    assert!(aes_cbc(&mut block, &key128, &[0u8; 16], true));
    assert_eq!(hex::encode(&block), hex::encode(&ct128), "AES-128 FIPS-197 C.1");

    let key256 = hx("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    let ct256 = hx("8ea2b7ca516745bfeafc49904b496089");
    let mut block = pt;
    assert!(aes_cbc(&mut block, &key256, &[0u8; 16], true));
    assert_eq!(hex::encode(&block), hex::encode(&ct256), "AES-256 FIPS-197 C.3");
}

#[test]
fn known_answer_ctr_nist_sp800_38a_f5_1() {
    // NIST SP 800-38A F.5.1 (AES-128-CTR). The IV is the initial counter block.
    let key = hx("2b7e151628aed2a6abf7158809cf4f3c");
    let iv = hx("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    let plaintext = hx(concat!(
        "6bc1bee22e409f96e93d7e117393172a",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "f69f2445df4f9b17ad2b417be66c3710",
    ));
    let expected = hx(concat!(
        "874d6191b620e3261bef6864990db6ce",
        "9806f66b7970fdff8617187bb9fffdff",
        "5ae4df3edbd5d35e5b4f09020db03eab",
        "1e031dda2fbe03d1792170a0f3009cee",
    ));

    let mut data = plaintext;
    assert!(aes_ctr(&mut data, &key, &iv));
    assert_eq!(hex::encode(&data), hex::encode(&expected), "NIST F.5.1 CTR");
}

#[test]
fn known_answer_cbc_nist_sp800_38a() {
    let plaintext = hx(concat!(
        "6bc1bee22e409f96e93d7e117393172a",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "f69f2445df4f9b17ad2b417be66c3710",
    ));
    let iv = hx("000102030405060708090a0b0c0d0e0f");

    // F.2.2 AES-128-CBC
    let key128 = hx("2b7e151628aed2a6abf7158809cf4f3c");
    let expected128 = hx(concat!(
        "7649abac8119b246cee98e9b12e9197d",
        "5086cb9b507219ee95db113a917678b2",
        "73bed6b8e3c1743b7116e69e22229516",
        "3ff1caa1681fac09120eca307586e1a7",
    ));
    let mut data = plaintext.clone();
    assert!(aes_cbc(&mut data, &key128, &iv, true));
    assert_eq!(hex::encode(&data), hex::encode(&expected128), "NIST F.2.2 CBC-128");

    // F.2.6 AES-256-CBC
    let key256 = hx(concat!(
        "603deb1015ca71be2b73aef0857d7781",
        "1f352c073b6108d72d9810a30914dff4",
    ));
    let expected256 = hx(concat!(
        "f58c4c04d6e5f1ba779eabfb5f7bfbd6",
        "9cfc4e967edb808d679f777bc6702c7d",
        "39f23369a9d9bacfa530e26304231461",
        "b2eb05e2c39be9fcda6c19078c6a9d1b",
    ));
    let mut data = plaintext;
    assert!(aes_cbc(&mut data, &key256, &iv, true));
    assert_eq!(hex::encode(&data), hex::encode(&expected256), "NIST F.2.6 CBC-256");
}

// ---------------------------------------------------------------------------
// Captured C++ parity vectors (from parity/outputs.txt)
// ---------------------------------------------------------------------------

#[test]
fn parity_hmac_sha1() {
    let cases: &[(&[u8], &[u8], &str)] = &[
        (&[], &[], "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d"),
        (
            b"Jefe",
            b"what do ya want for nothing?",
            "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
        ),
        (
            &seq(16, 0x00),
            &seq(16, 0x20),
            "cfcf5b5cfbc920f07020307b993ca8687373531e",
        ),
        (
            &seq(32, 0x00),
            &seq(32, 0x20),
            "860de445fd57f943c268a6f817b185b84c4806ab",
        ),
        (
            &seq(64, 0x00),
            &seq(48, 0x40),
            "93bb516bae9a2e66ca7d353d65b2b6ecc240b859",
        ),
        (
            &seq(65, 0x00),
            &seq(16, 0x00),
            "2451f4762832f92d5456dcb5564faec173add84d",
        ),
        (
            &seq(100, 0x00),
            &[0xaa],
            "c0d48bbaedbd15bf6074b1908f92a320e2ba64c1",
        ),
    ];
    for (i, (key, data, expected)) in cases.iter().enumerate() {
        let mac = hmac_sha1(key, data);
        assert_eq!(
            hex::encode(&mac),
            *expected,
            "parity HMAC-SHA1 case {i} (key_len={}, data_len={})",
            key.len(),
            data.len()
        );
    }
}

#[test]
fn parity_aes_ctr() {
    let key = seq(16, 0x00);
    let iv = seq(16, 0x10);

    // len 0
    let mut data: Vec<u8> = vec![];
    assert!(aes_ctr(&mut data, &key, &iv));
    assert_eq!(data, Vec::<u8>::new());

    // len 16
    let mut data = seq(16, 0x20);
    assert!(aes_ctr(&mut data, &key, &iv));
    assert_eq!(hex::encode(&data), "27dfcd57c5f02549b827c43aa2b9bcbc");

    // len 32
    let mut data = seq(32, 0x20);
    assert!(aes_ctr(&mut data, &key, &iv));
    assert_eq!(
        hex::encode(&data),
        "27dfcd57c5f02549b827c43aa2b9bcbca2f3f737ec39b4357e404759b7c4e037"
    );

    // len 20 (non-multiple of 16)
    let mut data = seq(20, 0x20);
    assert!(aes_ctr(&mut data, &key, &iv));
    assert_eq!(hex::encode(&data), "27dfcd57c5f02549b827c43aa2b9bcbca2f3f737");

    // counter carry (iv = 0xff*16)
    let iv_ff = [0xffu8; 16];
    let mut data = seq(48, 0x00);
    assert!(aes_ctr(&mut data, &key, &iv_ff));
    assert_eq!(
        hex::encode(&data),
        "3c451d31ca0284246cdea892025db51cd6b02924939a4d9577569b79bdd5c666536731b6b1e59239615297c849d90325"
    );
}

#[test]
fn parity_aes_cbc_128() {
    let key = seq(16, 0x00);
    let iv = seq(16, 0x10);
    let plaintext = seq(32, 0x20);
    let ciphertext_hex = "9bb5f601884fcd6f6e29b23f82cca77aed83e78428f8510691163c71b04ed6f3";

    // encrypt
    let mut data = plaintext.clone();
    assert!(aes_cbc(&mut data, &key, &iv, true));
    assert_eq!(hex::encode(&data), ciphertext_hex, "AES-128-CBC encrypt parity");

    // decrypt round-trip
    let mut roundtrip = hx(ciphertext_hex);
    assert!(aes_cbc(&mut roundtrip, &key, &iv, false));
    assert_eq!(roundtrip, plaintext, "AES-128-CBC decrypt round-trip");
}

#[test]
fn parity_aes_cbc_256() {
    let key = seq(32, 0x00);
    let iv = seq(16, 0x20);
    let plaintext = seq(48, 0x30);
    let ciphertext_hex =
        "9f3b7504926f8bd36e3118e903a4cd4a73d4c81a0b792dad9bf8de7568449964faf4b76f409d1d3766a960c15d54e10f";

    // encrypt
    let mut data = plaintext.clone();
    assert!(aes_cbc(&mut data, &key, &iv, true));
    assert_eq!(hex::encode(&data), ciphertext_hex, "AES-256-CBC encrypt parity");

    // decrypt round-trip
    let mut roundtrip = hx(ciphertext_hex);
    assert!(aes_cbc(&mut roundtrip, &key, &iv, false));
    assert_eq!(roundtrip, plaintext, "AES-256-CBC decrypt round-trip");
}

//! C ABI smoke tests, exercised directly from Rust (no C needed).
//!
//! These call the `#[no_mangle] extern "C"` exports exactly as a C caller
//! would: raw pointers + lengths, owned buffers via `fw_take_buffer` /
//! `fw_free_buffer`, `int` error codes, and the `fw_last_error` string.

use firmware_crypto::ffi::{
    fw_aes_cbc, fw_aes_ctr, fw_free_buffer, fw_hmac_sha1, fw_hmac_sha1_owned, fw_last_error,
    fw_take_buffer, FwBuffer, FW_ERR_ARG, FW_ERR_DATA, FW_ERR_IV, FW_ERR_KEY, FW_OK,
};
use std::ffi::CStr;
use std::ptr;

fn seq(n: usize, start: u8) -> Vec<u8> {
    (0..n).map(|i| start.wrapping_add(i as u8)).collect()
}

#[test]
fn ffi_hmac_sha1_into_caller_buffer() {
    let key = b"key".to_vec();
    let data = b"The quick brown fox jumps over the lazy dog".to_vec();
    let mut out = vec![0u8; 32];
    let code = fw_hmac_sha1(
        key.as_ptr(),
        key.len(),
        data.as_ptr(),
        data.len(),
        out.as_mut_ptr(),
        out.len(),
    );
    assert_eq!(code, 20, "hmac returns 20 bytes written");
    // 20 bytes were written; compare against the pure function for consistency.
    let mac = firmware_crypto::hmac_sha1(&key, &data);
    assert_eq!(&out[..20], mac.as_slice());
}

#[test]
fn ffi_hmac_sha1_rejects_small_buffer() {
    let key = b"key".to_vec();
    let mut out = vec![0u8; 19];
    let code = fw_hmac_sha1(
        key.as_ptr(),
        key.len(),
        ptr::null(),
        0,
        out.as_mut_ptr(),
        out.len(),
    );
    assert_eq!(code, FW_ERR_ARG);
}

#[test]
fn ffi_aes_ctr_in_place() {
    let key = seq(16, 0x00);
    let iv = seq(16, 0x10);
    let mut data = seq(16, 0x20);
    let code = fw_aes_ctr(
        data.as_mut_ptr(),
        data.len(),
        key.as_ptr(),
        key.len(),
        iv.as_ptr(),
        iv.len(),
    );
    assert_eq!(code, FW_OK);
    assert_eq!(hex::encode(&data), "27dfcd57c5f02549b827c43aa2b9bcbc");
}

#[test]
fn ffi_aes_cbc_in_place_and_errors() {
    let key = seq(16, 0x00);
    let iv = seq(16, 0x10);
    let plaintext = seq(32, 0x20);

    let mut data = plaintext.clone();
    let code = fw_aes_cbc(
        data.as_mut_ptr(),
        data.len(),
        key.as_ptr(),
        key.len(),
        iv.as_ptr(),
        iv.len(),
        1,
    );
    assert_eq!(code, FW_OK);
    assert_eq!(
        hex::encode(&data),
        "9bb5f601884fcd6f6e29b23f82cca77aed83e78428f8510691163c71b04ed6f3"
    );

    // error codes
    let mut bad = vec![0u8; 16];
    let key24 = seq(24, 0x00);
    let code = fw_aes_cbc(
        bad.as_mut_ptr(),
        bad.len(),
        key24.as_ptr(),
        key24.len(),
        iv.as_ptr(),
        iv.len(),
        1,
    );
    assert_eq!(code, FW_ERR_KEY);

    let code = fw_aes_cbc(
        bad.as_mut_ptr(),
        bad.len(),
        key.as_ptr(),
        key.len(),
        iv.as_ptr(),
        15,
        1,
    );
    assert_eq!(code, FW_ERR_IV);

    let mut unaligned = vec![0u8; 15];
    let code = fw_aes_cbc(
        unaligned.as_mut_ptr(),
        unaligned.len(),
        key.as_ptr(),
        key.len(),
        iv.as_ptr(),
        iv.len(),
        1,
    );
    assert_eq!(code, FW_ERR_DATA);
}

#[test]
fn ffi_owned_buffer_lifecycle() {
    let key = b"Jefe".to_vec();
    let data = b"what do ya want for nothing?".to_vec();

    let mut buf: FwBuffer = fw_hmac_sha1_owned(key.as_ptr(), key.len(), data.as_ptr(), data.len());
    assert_eq!(buf.len, 20);
    assert!(!buf.data.is_null());

    // Take ownership out of the struct (this nulls it).
    let mut out_data: *mut u8 = ptr::null_mut();
    let mut out_len: usize = 0;
    let code = fw_take_buffer(&mut buf, &mut out_data, &mut out_len);
    assert_eq!(code, FW_OK);
    assert_eq!(out_len, 20);
    assert!(buf.data.is_null(), "take_buffer must null the source struct");
    assert_eq!(buf.len, 0);

    // Read the taken bytes and free them.
    let taken = unsafe { std::slice::from_raw_parts(out_data, out_len) };
    assert_eq!(hex::encode(taken), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    fw_free_buffer(out_data, out_len);

    // Double-take is now a no-op (already nulled); take on a nulled buffer
    // yields NULL/0 and is still a valid (arg-checked) call.
    let mut out_data2: *mut u8 = ptr::null_mut();
    let mut out_len2: usize = 0;
    let code = fw_take_buffer(&mut buf, &mut out_data2, &mut out_len2);
    assert_eq!(code, FW_OK);
    assert!(out_data2.is_null());
    assert_eq!(out_len2, 0);
}

#[test]
fn ffi_last_error_string() {
    let key = seq(16, 0x00);
    let iv = seq(16, 0x10);
    let mut data = seq(16, 0x20);

    // A successful call clears any prior error.
    let code = fw_aes_ctr(
        data.as_mut_ptr(),
        data.len(),
        key.as_ptr(),
        key.len(),
        iv.as_ptr(),
        iv.len(),
    );
    assert_eq!(code, FW_OK);
    assert!(fw_last_error().is_null());

    // A bad key length sets the error string.
    let mut data = seq(16, 0x20);
    let key15 = seq(15, 0x00);
    let code = fw_aes_ctr(
        data.as_mut_ptr(),
        data.len(),
        key15.as_ptr(),
        key15.len(),
        iv.as_ptr(),
        iv.len(),
    );
    assert_eq!(code, FW_ERR_KEY);
    let msg_ptr = fw_last_error();
    assert!(!msg_ptr.is_null());
    let msg = unsafe { CStr::from_ptr(msg_ptr) }.to_str().expect("utf-8 error");
    assert!(msg.contains("16-byte key"), "unexpected error message: {msg}");
}

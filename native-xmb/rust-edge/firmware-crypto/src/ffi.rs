//! C ABI layer.
//!
//! Boundary rules (reconstructed from `native-xmb/audit/RUST-HARDENING.md` §3.1,
//! which did not yet exist when this milestone was cut — see the crate README):
//!
//! * Owned, length-prefixed buffers — inputs are `const uint8_t*` + `size_t`.
//! * Outputs the caller must release are returned as owned buffers and freed
//!   via `fw_take_buffer` / `fw_free_buffer`.
//! * Errors are `int` codes; an optional thread-local error string is available
//!   via `fw_last_error`.
//! * No Qt types cross the boundary.
//! * Every `#[no_mangle]` export is wrapped in `catch_unwind`.

use crate::crypto;
use std::cell::RefCell;
use std::os::raw::c_char;
use std::slice;

/// Error codes. Must stay in sync with `include/firmware_crypto_abi.h`.
pub const FW_OK: i32 = 0;
pub const FW_ERR_ARG: i32 = -1;
pub const FW_ERR_KEY: i32 = -2;
pub const FW_ERR_IV: i32 = -3;
pub const FW_ERR_DATA: i32 = -4;
pub const FW_ERR_ALLOC: i32 = -5;
pub const FW_ERR_PANIC: i32 = -6;

/// Owned output buffer. `data` is heap-allocated (length `len`); the caller
/// takes ownership and must release it with `fw_free_buffer` (or move it out
/// with `fw_take_buffer`).
#[repr(C)]
pub struct FwBuffer {
    pub data: *mut u8,
    pub len: usize,
}

thread_local! {
    static LAST_ERROR: RefCell<Vec<u8>> = RefCell::new(Vec::new());
}

fn set_last_error(msg: &str) {
    let mut v = msg.as_bytes().to_vec();
    v.push(0); // NUL-terminate for C
    LAST_ERROR.with(|c| *c.borrow_mut() = v);
}

fn clear_last_error() {
    LAST_ERROR.with(|c| c.borrow_mut().clear());
}

/// Convert a possibly-null C pointer + length into a Rust slice.
///
/// # Safety
/// `ptr` must be valid for reads of `len` bytes whenever `len > 0`.
unsafe fn in_slice<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        &[]
    } else {
        slice::from_raw_parts(ptr, len)
    }
}

/// Convert a possibly-null C pointer + length into a mutable Rust slice.
///
/// # Safety
/// `ptr` must be valid for reads/writes of `len` bytes whenever `len > 0`.
unsafe fn in_slice_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if len == 0 {
        &mut []
    } else {
        slice::from_raw_parts_mut(ptr, len)
    }
}

/// Leak a `Vec<u8>` into an owned raw buffer for C consumption.
fn vec_into_owned(mut v: Vec<u8>) -> FwBuffer {
    if v.is_empty() {
        return FwBuffer {
            data: std::ptr::null_mut(),
            len: 0,
        };
    }
    v.shrink_to_fit(); // guarantee capacity == len so from_raw_parts(len, len) is valid
    let data = v.as_mut_ptr();
    let len = v.len();
    std::mem::forget(v);
    FwBuffer { data, len }
}

/// Reconstruct and drop a buffer produced by `vec_into_owned`.
///
/// # Safety
/// `data` must have been returned by `vec_into_owned` with the same `len`.
unsafe fn free_owned(data: *mut u8, len: usize) {
    if !data.is_null() && len > 0 {
        drop(Vec::from_raw_parts(data, len, len));
    }
}

/// Run a fallible FFI body, mapping any panic to `FW_ERR_PANIC`.
fn guard<F: FnOnce() -> i32>(f: F) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(code) => code,
        Err(_) => {
            set_last_error("internal panic caught at FFI boundary");
            FW_ERR_PANIC
        }
    }
}

/// Validate that an input pointer is non-null whenever its length is nonzero.
fn check_in(ptr: *const u8, len: usize, what: &str) -> i32 {
    if ptr.is_null() && len != 0 {
        set_last_error(what);
        return FW_ERR_ARG;
    }
    FW_OK
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

/// HMAC-SHA1 into a caller-provided buffer. Writes exactly 20 bytes to `out`
/// (which must have capacity >= 20). Returns 20 on success, or a negative
/// error code.
#[no_mangle]
pub extern "C" fn fw_hmac_sha1(
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    guard(|| {
        clear_last_error();
        let mut code = check_in(key, key_len, "null key pointer");
        if code != FW_OK {
            return code;
        }
        code = check_in(data, data_len, "null data pointer");
        if code != FW_OK {
            return code;
        }
        if out.is_null() || out_cap < 20 {
            set_last_error("output buffer too small for 20-byte HMAC");
            return FW_ERR_ARG;
        }
        let key_s = unsafe { in_slice(key, key_len) };
        let data_s = unsafe { in_slice(data, data_len) };
        let mac = crypto::hmac_sha1(key_s, data_s);
        unsafe { std::ptr::copy_nonoverlapping(mac.as_ptr(), out, 20) };
        20
    })
}

/// Owned-buffer variant of HMAC-SHA1. Returns a caller-owned 20-byte MAC; on
/// error `data` is NULL and `len` is 0. Free with `fw_free_buffer` (or move out
/// with `fw_take_buffer`).
#[no_mangle]
pub extern "C" fn fw_hmac_sha1_owned(
    key: *const u8,
    key_len: usize,
    data: *const u8,
    data_len: usize,
) -> FwBuffer {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        clear_last_error();
        if check_in(key, key_len, "null key pointer") != FW_OK
            || check_in(data, data_len, "null data pointer") != FW_OK
        {
            return FwBuffer {
                data: std::ptr::null_mut(),
                len: 0,
            };
        }
        let key_s = unsafe { in_slice(key, key_len) };
        let data_s = unsafe { in_slice(data, data_len) };
        vec_into_owned(crypto::hmac_sha1(key_s, data_s))
    }));
    match result {
        Ok(buf) => buf,
        Err(_) => {
            set_last_error("internal panic caught at FFI boundary");
            FwBuffer {
                data: std::ptr::null_mut(),
                len: 0,
            }
        }
    }
}

/// AES-128-CTR, in place. `key`/`iv` must be 16 bytes. Returns 0 on success.
#[no_mangle]
pub extern "C" fn fw_aes_ctr(
    data: *mut u8,
    data_len: usize,
    key: *const u8,
    key_len: usize,
    iv: *const u8,
    iv_len: usize,
) -> i32 {
    guard(|| {
        clear_last_error();
        let mut code = check_in(data, data_len, "null data pointer");
        if code != FW_OK {
            return code;
        }
        code = check_in(key, key_len, "null key pointer");
        if code != FW_OK {
            return code;
        }
        code = check_in(iv, iv_len, "null iv pointer");
        if code != FW_OK {
            return code;
        }
        if key_len != 16 {
            set_last_error("aes_ctr requires a 16-byte key");
            return FW_ERR_KEY;
        }
        if iv_len != 16 {
            set_last_error("aes_ctr requires a 16-byte iv");
            return FW_ERR_IV;
        }
        let data_s = unsafe { in_slice_mut(data, data_len) };
        let key_s = unsafe { in_slice(key, key_len) };
        let iv_s = unsafe { in_slice(iv, iv_len) };
        crypto::aes_ctr(data_s, key_s, iv_s);
        FW_OK
    })
}

/// AES-128/256-CBC, in place, raw (no padding). `key` must be 16 or 32 bytes,
/// `iv` 16 bytes, `data_len` a multiple of 16. `encrypt` is a C-style truthy
/// flag (nonzero == encrypt). Returns 0 on success.
#[no_mangle]
pub extern "C" fn fw_aes_cbc(
    data: *mut u8,
    data_len: usize,
    key: *const u8,
    key_len: usize,
    iv: *const u8,
    iv_len: usize,
    encrypt: i32,
) -> i32 {
    guard(|| {
        clear_last_error();
        let mut code = check_in(data, data_len, "null data pointer");
        if code != FW_OK {
            return code;
        }
        code = check_in(key, key_len, "null key pointer");
        if code != FW_OK {
            return code;
        }
        code = check_in(iv, iv_len, "null iv pointer");
        if code != FW_OK {
            return code;
        }
        if key_len != 16 && key_len != 32 {
            set_last_error("aes_cbc requires a 16- or 32-byte key");
            return FW_ERR_KEY;
        }
        if iv_len != 16 {
            set_last_error("aes_cbc requires a 16-byte iv");
            return FW_ERR_IV;
        }
        if data_len % 16 != 0 {
            set_last_error("aes_cbc data length must be a multiple of 16");
            return FW_ERR_DATA;
        }
        let data_s = unsafe { in_slice_mut(data, data_len) };
        let key_s = unsafe { in_slice(key, key_len) };
        let iv_s = unsafe { in_slice(iv, iv_len) };
        crypto::aes_cbc(data_s, key_s, iv_s, encrypt != 0);
        FW_OK
    })
}

/// Move ownership out of a returned `FwBuffer`, nulling it so it cannot be
/// taken twice. Returns 0 on success.
#[no_mangle]
pub extern "C" fn fw_take_buffer(
    buf: *mut FwBuffer,
    out_data: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    guard(|| {
        clear_last_error();
        if buf.is_null() || out_data.is_null() || out_len.is_null() {
            set_last_error("fw_take_buffer: null argument");
            return FW_ERR_ARG;
        }
        unsafe {
            *out_data = (*buf).data;
            *out_len = (*buf).len;
            (*buf).data = std::ptr::null_mut();
            (*buf).len = 0;
        }
        FW_OK
    })
}

/// Free an owned buffer returned by the library (e.g. from `fw_hmac_sha1_owned`).
#[no_mangle]
pub extern "C" fn fw_free_buffer(data: *mut u8, len: usize) {
    // No panic path here, but kept behind catch_unwind for symmetry with the
    // other exports.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        unsafe { free_owned(data, len) };
    }));
}

/// Last error message (thread-local). Returns a NUL-terminated string, or NULL
/// if no error is recorded on this thread. Valid until the next FFI call on
/// the same thread.
#[no_mangle]
pub extern "C" fn fw_last_error() -> *const c_char {
    LAST_ERROR.with(|c| {
        let c = c.borrow();
        if c.is_empty() {
            std::ptr::null()
        } else {
            c.as_ptr() as *const c_char
        }
    })
}

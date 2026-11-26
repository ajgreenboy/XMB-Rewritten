//! C ABI exports for `firmware-extract`.
//!
//! All `#[no_mangle]` entry points catch panics via `catch_unwind`, validate
//! every pointer/length pair before constructing slices, and store thread-local
//! UTF-8 diagnostics retrievable through `fw_extract_last_error`.

use crate::extract::{self, ExtractError, Keys};
use std::cell::RefCell;
use std::os::raw::c_char;
use std::path::Path;
use std::slice;

pub const FW_EXTRACT_OK: i32 = 0;
pub const FW_EXTRACT_ERR_ARG: i32 = -1;
pub const FW_EXTRACT_ERR_KEYS: i32 = -2;
pub const FW_EXTRACT_ERR_IO: i32 = -3;
pub const FW_EXTRACT_ERR_FMT: i32 = -4;
pub const FW_EXTRACT_ERR_PATH: i32 = -5;
pub const FW_EXTRACT_ERR_PANIC: i32 = -6;

#[repr(C)]
pub struct FwExtractStats {
    pub entries_verified: u64,
    pub packages: u64,
    pub bytes_written: u64,
    pub explore_rco_present: i32,
}

thread_local! {
    static LAST_EXTRACT_ERROR: RefCell<Vec<u8>> = RefCell::new(Vec::new());
}

fn set_error(msg: &str) {
    let mut bytes = msg.as_bytes().to_vec();
    bytes.push(0);
    LAST_EXTRACT_ERROR.with(|slot| *slot.borrow_mut() = bytes);
}

fn clear_error() {
    LAST_EXTRACT_ERROR.with(|slot| slot.borrow_mut().clear());
}

fn map_error(err: ExtractError) -> i32 {
    let code = match &err {
        ExtractError::Arg(_) => FW_EXTRACT_ERR_ARG,
        ExtractError::Keys(_) => FW_EXTRACT_ERR_KEYS,
        ExtractError::Io(_) => FW_EXTRACT_ERR_IO,
        ExtractError::Format(_) => FW_EXTRACT_ERR_FMT,
        ExtractError::Path(_) => FW_EXTRACT_ERR_PATH,
    };
    set_error(err.message());
    code
}

fn guard<F: FnOnce() -> i32>(f: F) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(rc) => rc,
        Err(_) => {
            set_error("internal panic caught at firmware-extract FFI boundary");
            FW_EXTRACT_ERR_PANIC
        }
    }
}

unsafe fn bytes_arg<'a>(ptr: *const u8, len: usize, label: &str) -> Result<&'a [u8], i32> {
    if len == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        set_error(label);
        return Err(FW_EXTRACT_ERR_ARG);
    }
    Ok(slice::from_raw_parts(ptr, len))
}

unsafe fn utf8_arg<'a>(ptr: *const u8, len: usize, label: &str) -> Result<&'a str, i32> {
    let bytes = bytes_arg(ptr, len, label)?;
    std::str::from_utf8(bytes).map_err(|_| {
        set_error("path argument is not valid UTF-8");
        FW_EXTRACT_ERR_ARG
    })
}

#[no_mangle]
pub extern "C" fn fw_extract_pup(
    pup_path: *const u8,
    pup_path_len: usize,
    dest_dir: *const u8,
    dest_dir_len: usize,
    pup_key: *const u8,
    pup_key_len: usize,
    erk: *const u8,
    erk_len: usize,
    riv: *const u8,
    riv_len: usize,
    out_stats: *mut FwExtractStats,
) -> i32 {
    guard(|| {
        clear_error();
        let pup_str = match unsafe { utf8_arg(pup_path, pup_path_len, "null pup_path pointer") } {
            Ok(s) => s,
            Err(rc) => return rc,
        };
        let dest_str = match unsafe { utf8_arg(dest_dir, dest_dir_len, "null dest_dir pointer") } {
            Ok(s) => s,
            Err(rc) => return rc,
        };
        let pk = match unsafe { bytes_arg(pup_key, pup_key_len, "null pup_key pointer") } {
            Ok(b) => b,
            Err(rc) => return rc,
        };
        let ek = match unsafe { bytes_arg(erk, erk_len, "null erk pointer") } {
            Ok(b) => b,
            Err(rc) => return rc,
        };
        let rv = match unsafe { bytes_arg(riv, riv_len, "null riv pointer") } {
            Ok(b) => b,
            Err(rc) => return rc,
        };
        let keys = match Keys::from_slices(pk, ek, rv) {
            Ok(k) => k,
            Err(e) => return map_error(e),
        };
        match extract::extract_pup(Path::new(pup_str), Path::new(dest_str), &keys) {
            Ok(stats) => {
                if !out_stats.is_null() {
                    unsafe {
                        *out_stats = FwExtractStats {
                            entries_verified: stats.entries_verified,
                            packages: stats.packages,
                            bytes_written: stats.bytes_written,
                            explore_rco_present: i32::from(stats.explore_rco_present),
                        };
                    }
                }
                FW_EXTRACT_OK
            }
            Err(e) => map_error(e),
        }
    })
}

#[no_mangle]
pub extern "C" fn fw_pup_version(
    pup_path: *const u8,
    pup_path_len: usize,
    out: *mut u8,
    out_cap: usize,
) -> i32 {
    guard(|| {
        clear_error();
        let pup_str = match unsafe { utf8_arg(pup_path, pup_path_len, "null pup_path pointer") } {
            Ok(s) => s,
            Err(rc) => return rc,
        };
        if out.is_null() || out_cap == 0 {
            set_error("null or zero-capacity version output buffer");
            return FW_EXTRACT_ERR_ARG;
        }
        match extract::pup_version(Path::new(pup_str)) {
            Ok(ver) => {
                let bytes = ver.as_bytes();
                if bytes.len() > out_cap || bytes.len() > i32::MAX as usize {
                    set_error("version output buffer too small");
                    return FW_EXTRACT_ERR_ARG;
                }
                unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()) };
                bytes.len() as i32
            }
            Err(e) => map_error(e),
        }
    })
}

#[no_mangle]
pub extern "C" fn fw_extract_last_error() -> *const c_char {
    let res = std::panic::catch_unwind(|| {
        LAST_EXTRACT_ERROR.with(|slot| {
            let b = slot.borrow();
            if b.is_empty() {
                std::ptr::null()
            } else {
                b.as_ptr() as *const c_char
            }
        })
    });
    res.unwrap_or(std::ptr::null())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_rejects_null_pointers_and_reports_error() {
        let rc = fw_extract_pup(
            std::ptr::null(),
            4,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            std::ptr::null_mut(),
        );
        assert_eq!(rc, FW_EXTRACT_ERR_ARG);
        assert!(!fw_extract_last_error().is_null());
    }
}

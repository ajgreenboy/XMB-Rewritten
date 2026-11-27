use std::cell::RefCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;

use crate::vdf::{encode_entries, parse, Options};

thread_local! {
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn set_error(msg: impl Into<String>) {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = msg.into());
}

fn clear_error() {
    LAST_ERROR.with(|slot| slot.borrow_mut().clear());
}

#[repr(C)]
pub struct SvdfOptions {
    pub skip_bom: u8,
    pub quoted_braces_are_structural: u8,
    pub max_token_bytes: usize,
    pub max_tokens: usize,
    pub max_entries: usize,
    pub max_depth: i32,
}

#[no_mangle]
pub unsafe extern "C" fn svdf_parse(
    input_ptr: *const u8,
    input_len: usize,
    options: *const SvdfOptions,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_buf.is_null() || out_len.is_null() || options.is_null() {
            set_error("null pointer argument");
            return -1;
        }
        unsafe {
            *out_buf = std::ptr::null_mut();
            *out_len = 0;
        }
        if input_len > 0 && input_ptr.is_null() {
            set_error("null input pointer with non-zero length");
            return -1;
        }
        let bytes: &[u8] = if input_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(input_ptr, input_len) }
        };
        let raw_opts = unsafe { &*options };
        let opts = Options {
            skip_bom: raw_opts.skip_bom != 0,
            quoted_braces_are_structural: raw_opts.quoted_braces_are_structural != 0,
            max_token_bytes: raw_opts.max_token_bytes,
            max_tokens: raw_opts.max_tokens,
            max_entries: raw_opts.max_entries,
            max_depth: raw_opts.max_depth,
        };
        match parse(bytes, &opts) {
            Ok(entries) => {
                let mut encoded = encode_entries(&entries).into_boxed_slice();
                let len = encoded.len();
                let ptr = encoded.as_mut_ptr();
                std::mem::forget(encoded);
                unsafe {
                    *out_buf = ptr;
                    *out_len = len;
                }
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }));
    match result {
        Ok(code) => code,
        Err(_) => {
            set_error("internal panic caught at FFI boundary");
            -2
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn svdf_free_buffer(ptr: *mut u8, len: usize) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !ptr.is_null() {
            unsafe {
                let slice_ptr = std::ptr::slice_from_raw_parts_mut(ptr, len);
                drop(Box::from_raw(slice_ptr));
            }
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn svdf_last_error(buf: *mut u8, buf_cap: usize) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        LAST_ERROR.with(|slot| {
            let err = slot.borrow();
            let bytes = err.as_bytes();
            if !buf.is_null() && buf_cap > 0 {
                let n = bytes.len().min(buf_cap);
                unsafe {
                    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, n);
                }
            }
            bytes.len()
        })
    }))
    .unwrap_or(0)
}

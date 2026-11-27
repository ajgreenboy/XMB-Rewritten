use std::cell::RefCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;

use crate::edge::decode_edge_packed;
use crate::gim::decode_gim;
use crate::gtf::decode_gtf;
use crate::qrc::{encode_parsed_qrc_toc, inflate_qrc_slice, parse_qrc};
use crate::subtitles::{encode_cues, parse_pgs, parse_vobsub};
use crate::vag::vag_to_wav;

thread_local! {
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn set_error(msg: impl Into<String>) {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = msg.into());
}

fn clear_error() {
    LAST_ERROR.with(|slot| slot.borrow_mut().clear());
}

unsafe fn hand_off(vec: Vec<u8>, out_buf: *mut *mut u8, out_len: *mut usize) {
    let mut boxed = vec.into_boxed_slice();
    let len = boxed.len();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    unsafe {
        *out_buf = ptr;
        *out_len = len;
    }
}

#[no_mangle]
pub unsafe extern "C" fn img_decode_gtf(
    gtf_ptr: *const u8,
    gtf_len: usize,
    out_width: *mut u32,
    out_height: *mut u32,
    out_pixels: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_width.is_null() || out_height.is_null() || out_pixels.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_width = 0;
            *out_height = 0;
            *out_pixels = std::ptr::null_mut();
            *out_len = 0;
        }
        if gtf_len > 0 && gtf_ptr.is_null() {
            set_error("null GTF input pointer");
            return -1;
        }
        let bytes = if gtf_len == 0 { &[] } else { unsafe { slice::from_raw_parts(gtf_ptr, gtf_len) } };
        match decode_gtf(bytes) {
            Ok(decoded) => {
                unsafe {
                    *out_width = decoded.width;
                    *out_height = decoded.height;
                    hand_off(decoded.argb_bytes, out_pixels, out_len);
                }
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_decode_edge(
    edge_ptr: *const u8,
    edge_len: usize,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_buf.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_buf = std::ptr::null_mut();
            *out_len = 0;
        }
        if edge_len > 0 && edge_ptr.is_null() {
            set_error("null Edge input pointer");
            return -1;
        }
        let bytes = if edge_len == 0 { &[] } else { unsafe { slice::from_raw_parts(edge_ptr, edge_len) } };
        match decode_edge_packed(bytes) {
            Ok(packed) => {
                unsafe { hand_off(packed, out_buf, out_len) };
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_parse_pgs(
    sup_ptr: *const u8,
    sup_len: usize,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_buf.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_buf = std::ptr::null_mut();
            *out_len = 0;
        }
        if sup_len > 0 && sup_ptr.is_null() {
            set_error("null PGS input pointer");
            return -1;
        }
        let bytes = if sup_len == 0 { &[] } else { unsafe { slice::from_raw_parts(sup_ptr, sup_len) } };
        match parse_pgs(bytes) {
            Ok((cues, reason)) => {
                set_error(reason);
                unsafe { hand_off(encode_cues(&cues), out_buf, out_len) };
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_parse_vobsub(
    idx_ptr: *const u8,
    idx_len: usize,
    sub_ptr: *const u8,
    sub_len: usize,
    track: i32,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_buf.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_buf = std::ptr::null_mut();
            *out_len = 0;
        }
        if (idx_len > 0 && idx_ptr.is_null()) || (sub_len > 0 && sub_ptr.is_null()) {
            set_error("null VobSub input pointer");
            return -1;
        }
        let idx = if idx_len == 0 { &[] } else { unsafe { slice::from_raw_parts(idx_ptr, idx_len) } };
        let sub = if sub_len == 0 { &[] } else { unsafe { slice::from_raw_parts(sub_ptr, sub_len) } };
        match parse_vobsub(idx, sub, track) {
            Ok((cues, reason)) => {
                set_error(reason);
                unsafe { hand_off(encode_cues(&cues), out_buf, out_len) };
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_decode_gim(
    gim_ptr: *const u8,
    gim_len: usize,
    out_width: *mut u32,
    out_height: *mut u32,
    out_pixels: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_width.is_null() || out_height.is_null() || out_pixels.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_width = 0;
            *out_height = 0;
            *out_pixels = std::ptr::null_mut();
            *out_len = 0;
        }
        if gim_len > 0 && gim_ptr.is_null() {
            set_error("null GIM input pointer");
            return -1;
        }
        let bytes = if gim_len == 0 { &[] } else { unsafe { slice::from_raw_parts(gim_ptr, gim_len) } };
        match decode_gim(bytes) {
            Ok(decoded) => {
                unsafe {
                    *out_width = decoded.width;
                    *out_height = decoded.height;
                    hand_off(decoded.argb_bytes, out_pixels, out_len);
                }
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_vag_to_wav(
    left_ptr: *const u8,
    left_len: usize,
    right_ptr: *const u8,
    right_len: usize,
    out_wav: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_wav.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_wav = std::ptr::null_mut();
            *out_len = 0;
        }
        if (left_len > 0 && left_ptr.is_null()) || (right_len > 0 && right_ptr.is_null()) {
            set_error("null VAG input pointer");
            return -1;
        }
        let left = if left_len == 0 { &[] } else { unsafe { slice::from_raw_parts(left_ptr, left_len) } };
        let right = if right_len == 0 { &[] } else { unsafe { slice::from_raw_parts(right_ptr, right_len) } };
        let wav = vag_to_wav(left, right);
        if !wav.is_empty() {
            unsafe { hand_off(wav, out_wav, out_len) };
        }
        0
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_parse_qrc(
    qrc_ptr: *const u8,
    qrc_len: usize,
    out_unpacked_buf: *mut *mut u8,
    out_unpacked_len: *mut usize,
    out_toc_buf: *mut *mut u8,
    out_toc_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_unpacked_buf.is_null() || out_unpacked_len.is_null() || out_toc_buf.is_null() || out_toc_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_unpacked_buf = std::ptr::null_mut();
            *out_unpacked_len = 0;
            *out_toc_buf = std::ptr::null_mut();
            *out_toc_len = 0;
        }
        if qrc_len > 0 && qrc_ptr.is_null() {
            set_error("null QRC input pointer");
            return -1;
        }
        let bytes = if qrc_len == 0 { &[] } else { unsafe { slice::from_raw_parts(qrc_ptr, qrc_len) } };
        match parse_qrc(bytes) {
            Ok(parsed) => {
                let toc = encode_parsed_qrc_toc(&parsed);
                unsafe {
                    hand_off(parsed.unpacked_bytes, out_unpacked_buf, out_unpacked_len);
                    hand_off(toc, out_toc_buf, out_toc_len);
                }
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_inflate_qrc_entry(
    data_ptr: *const u8,
    data_len: usize,
    expanded_len: u32,
    out_buf: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error();
        if out_buf.is_null() || out_len.is_null() {
            set_error("null output pointer");
            return -1;
        }
        unsafe {
            *out_buf = std::ptr::null_mut();
            *out_len = 0;
        }
        if data_len > 0 && data_ptr.is_null() {
            set_error("null QRC entry pointer");
            return -1;
        }
        let slice = if data_len == 0 { &[] } else { unsafe { slice::from_raw_parts(data_ptr, data_len) } };
        match inflate_qrc_slice(slice, expanded_len) {
            Ok(inflated) => {
                unsafe { hand_off(inflated, out_buf, out_len) };
                0
            }
            Err(e) => {
                set_error(e);
                1
            }
        }
    }))
    .unwrap_or_else(|_| {
        set_error("internal panic caught at FFI boundary");
        -2
    })
}

#[no_mangle]
pub unsafe extern "C" fn img_free_buffer(ptr: *mut u8, len: usize) {
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
pub unsafe extern "C" fn img_last_error(buf: *mut u8, buf_cap: usize) -> usize {
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

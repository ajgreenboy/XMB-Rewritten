/*
 * firmware_extract_abi.h — C ABI for the `firmware-extract` Rust staticlib.
 *
 * Safe Rust reimplementation of `xmb::extractPup` and `xmb::pupVersion`
 * (`native-xmb/src/firmware_extract.cpp`), following the boundary rules in
 * `native-xmb/audit/RUST-HARDENING.md` §3.1:
 *   * Owned, length-prefixed byte slices only (`const uint8_t*` + `size_t`).
 *   * No Qt types cross the wire.
 *   * Errors return `int32_t` codes; thread-local detail is exposed via
 *     `fw_extract_last_error()`.
 *   * Panics are caught at every `#[no_mangle]` export via `catch_unwind`.
 */
#ifndef FIRMWARE_EXTRACT_ABI_H
#define FIRMWARE_EXTRACT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FW_EXTRACT_OK        =  0,
    FW_EXTRACT_ERR_ARG   = -1, /* null pointer, invalid UTF-8, or buffer too small */
    FW_EXTRACT_ERR_KEYS  = -2, /* missing or invalid key material */
    FW_EXTRACT_ERR_IO    = -3, /* file read/write or staging directory failure */
    FW_EXTRACT_ERR_FMT   = -4, /* invalid PUP/SCE/TAR format, bounds, or HMAC mismatch */
    FW_EXTRACT_ERR_PATH  = -5, /* unsafe TAR path or destination link/reparse point */
    FW_EXTRACT_ERR_PANIC = -6  /* internal panic caught at the FFI boundary */
};

typedef struct fw_extract_stats {
    uint64_t entries_verified;
    uint64_t packages;
    uint64_t bytes_written;
    int32_t  explore_rco_present;
} fw_extract_stats;

/*
 * Verify every entry of `pup_path` with HMAC-SHA1 (`pup_key`, 64 bytes),
 * decrypt `dev_flash_*` SCE packages inside update TAR entry 0x300 (`erk` 32 B,
 * `riv` 16 B), validate all ustar TAR entries, and transactionally write the
 * unpacked `dev_flash` tree into `dest_dir`.
 *
 * `pup_path` and `dest_dir` are UTF-8 byte slices (not required to be NUL-terminated).
 * `out_stats` may be NULL. Returns 0 (`FW_EXTRACT_OK`) on success, or a negative
 * `FW_EXTRACT_ERR_*` code (call `fw_extract_last_error()` for the diagnostic).
 */
int32_t fw_extract_pup(const uint8_t *pup_path, size_t pup_path_len,
                       const uint8_t *dest_dir, size_t dest_dir_len,
                       const uint8_t *pup_key, size_t pup_key_len,
                       const uint8_t *erk, size_t erk_len,
                       const uint8_t *riv, size_t riv_len,
                       fw_extract_stats *out_stats);

/*
 * Read the firmware version string (PUP entry 0x100, e.g. "4.93") without
 * decrypting or reading package payloads. Writes the UTF-8 version string (no
 * NUL terminator) into `out` (`out_cap` bytes) and returns the number of bytes
 * written (> 0) on success, or a negative `FW_EXTRACT_ERR_*` code on error.
 */
int32_t fw_pup_version(const uint8_t *pup_path, size_t pup_path_len,
                       uint8_t *out, size_t out_cap);

/*
 * Thread-local NUL-terminated UTF-8 error message from the last `fw_extract_*`
 * or `fw_pup_version` call on the calling thread, or NULL if the last call
 * succeeded. Valid until the next FFI call on the same thread.
 */
const char *fw_extract_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_EXTRACT_ABI_H */

/*
 * firmware_crypto_abi.h — C ABI for the `firmware-crypto` Rust staticlib.
 *
 * Byte-for-byte parity reimplementation of the native XMB firmware crypto
 * primitives (HMAC-SHA1, AES-128-CTR, AES-128/256-CBC). See the crate README
 * for the boundary rules, error semantics, and the parity test procedure.
 *
 * Conventions:
 *   * Inputs are owned, length-prefixed buffers (`const uint8_t*` + `size_t`).
 *   * Outputs the caller must release are returned as `fw_buffer` and freed
 *     via `fw_free_buffer` (or moved out via `fw_take_buffer`).
 *   * Errors are `int32_t` codes; a thread-local error string is available via
 *     `fw_last_error`.
 *   * No Qt types cross the boundary.
 */
#ifndef FIRMWARE_CRYPTO_ABI_H
#define FIRMWARE_CRYPTO_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (0 == success). */
enum {
    FW_OK        =  0,
    FW_ERR_ARG   = -1, /* null pointer, or output buffer too small */
    FW_ERR_KEY   = -2, /* invalid key length */
    FW_ERR_IV    = -3, /* invalid IV length */
    FW_ERR_DATA  = -4, /* invalid data length (e.g. CBC not block-aligned) */
    FW_ERR_ALLOC = -5, /* allocation failed */
    FW_ERR_PANIC = -6  /* internal panic was caught at the boundary */
};

/*
 * Owned output buffer. The library allocates `data` (length `len`) and the
 * caller takes ownership. Free with `fw_free_buffer(data, len)`.
 * On error, `data` is NULL and `len` is 0.
 */
typedef struct fw_buffer {
    uint8_t *data;
    size_t   len;
} fw_buffer;

/*
 * HMAC-SHA1 (RFC 2104) into a caller-provided buffer. Writes exactly 20 bytes
 * to `out` (which must have `out_cap >= 20`). Returns 20 on success, or a
 * negative error code. Keys longer than 64 bytes are pre-hashed with SHA-1.
 */
int32_t fw_hmac_sha1(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     uint8_t *out, size_t out_cap);

/*
 * HMAC-SHA1 returning a caller-owned buffer (the 20-byte MAC). `data` is NULL
 * on error. Free with `fw_free_buffer`.
 */
fw_buffer fw_hmac_sha1_owned(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len);

/*
 * AES-128-CTR, in place. `key` and `iv` must both be 16 bytes. The 16-byte
 * counter is a big-endian integer incremented after each block. Returns 0 on
 * success.
 */
int32_t fw_aes_ctr(uint8_t *data, size_t data_len,
                   const uint8_t *key, size_t key_len,
                   const uint8_t *iv, size_t iv_len);

/*
 * AES-128/256-CBC, in place, raw (no padding). `key` must be 16 or 32 bytes,
 * `iv` 16 bytes, `data_len` a multiple of 16. `encrypt` is a truthy flag
 * (nonzero == encrypt). Returns 0 on success.
 */
int32_t fw_aes_cbc(uint8_t *data, size_t data_len,
                   const uint8_t *key, size_t key_len,
                   const uint8_t *iv, size_t iv_len,
                   int encrypt);

/*
 * Move ownership out of `buf` into `*out_data`/`*out_len`, nulling `buf` so it
 * cannot be taken twice. Returns 0 on success.
 */
int32_t fw_take_buffer(fw_buffer *buf, uint8_t **out_data, size_t *out_len);

/* Free an owned buffer returned by the library. */
void fw_free_buffer(uint8_t *data, size_t len);

/*
 * Last error message (thread-local). Returns a NUL-terminated string, or NULL
 * if no error is recorded on this thread. Valid until the next FFI call on the
 * same thread.
 */
const char *fw_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_CRYPTO_ABI_H */

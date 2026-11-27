#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct svdf_options {
    uint8_t skip_bom;
    uint8_t quoted_braces_are_structural;
    size_t max_token_bytes;
    size_t max_tokens;
    size_t max_entries;
    int32_t max_depth;
};

/* Parses UTF-8 KeyValues bytes into an owned binary buffer:
 *   [u32 root_entry_count LE]
 *   followed by `Entry` records:
 *     [u8 is_block (0 = key/value, 1 = block)]
 *     [u32 key_len LE][key UTF-8 bytes]
 *     if is_block == 0: [u32 val_len LE][val UTF-8 bytes]
 *     if is_block == 1: [u32 child_count LE][child Entry records...]
 *
 * Returns 0 on success (setting *out_buf and *out_len; caller frees with svdf_free_buffer),
 * or non-zero on error (retrievable via svdf_last_error). Panics never cross the boundary.
 */
int svdf_parse(
    const uint8_t* input_ptr,
    size_t input_len,
    const struct svdf_options* options,
    uint8_t** out_buf,
    size_t* out_len
);

void svdf_free_buffer(uint8_t* ptr, size_t len);

size_t svdf_last_error(uint8_t* buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif

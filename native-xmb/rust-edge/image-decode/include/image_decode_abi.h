#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes level 0 of a PS3 GTF texture into native-endian ARGB32 (QRgb: 0xAARRGGBB per pixel).
 * Returns 0 on success (caller frees *out_pixels with img_free_buffer), non-zero on error.
 */
int img_decode_gtf(
    const uint8_t* gtf_ptr,
    size_t gtf_len,
    uint32_t* out_width,
    uint32_t* out_height,
    uint8_t** out_pixels,
    size_t* out_len
);

/* Decodes a single-segment PS3 Edge mesh (`plane.edge` descriptor layout) into a packed little-endian buffer:
 *   [u32 vertex_count][u32 index_count]
 *   [vertex_count * 8 f32 LE: pos.xyz, normal.xyz, uv.xy]
 *   [index_count * u32 LE]
 * Returns 0 on success (caller frees *out_buf with img_free_buffer), non-zero on error.
 */
int img_decode_edge(
    const uint8_t* edge_ptr,
    size_t edge_len,
    uint8_t** out_buf,
    size_t* out_len
);

/* Decodes a Blu-ray PGS (.sup) stream into packed Indexed8 cues:
 *   [u32 cue_count LE]
 *   per cue:
 *     [i64 start_ms LE][i64 end_ms LE]
 *     [i32 rect_x LE][i32 rect_y LE][i32 rect_w LE][i32 rect_h LE]
 *     [i32 video_w LE][i32 video_h LE]
 *     [u8 forced]
 *     [u16 palette_len LE][u32 ARGB LE * palette_len]
 *     [u32 pixels_len LE][u8 * pixels_len (rect_w * rect_h)]
 * On success returns 0; any non-fatal skip summary is retrievable via img_last_error.
 * On failure returns non-zero with the failure reason in img_last_error.
 */
int img_parse_pgs(
    const uint8_t* sup_ptr,
    size_t sup_len,
    uint8_t** out_buf,
    size_t* out_len
);

/* Decodes one track of a DVD VobSub (.idx + .sub) pair into the same packed Indexed8 cue format as img_parse_pgs. */
int img_parse_vobsub(
    const uint8_t* idx_ptr,
    size_t idx_len,
    const uint8_t* sub_ptr,
    size_t sub_len,
    int32_t track,
    uint8_t** out_buf,
    size_t* out_len
);

/* Decodes a PS3 big-endian .GIM1.00 image into native-endian ARGB32 (QRgb: 0xAARRGGBB per pixel).
 * Returns 0 on success (caller frees *out_pixels with img_free_buffer), non-zero on error.
 */
int img_decode_gim(
    const uint8_t* gim_ptr,
    size_t gim_len,
    uint32_t* out_width,
    uint32_t* out_height,
    uint8_t** out_pixels,
    size_t* out_len
);

/* Decodes mono or stereo PlayStation ADPCM VAG (.vag / VAGp) audio streams into a 16-bit PCM RIFF WAVE buffer.
 * Returns 0 on success (even if *out_len == 0 for empty input; caller frees non-null *out_wav with img_free_buffer).
 */
int img_vag_to_wav(
    const uint8_t* left_ptr,
    size_t left_len,
    const uint8_t* right_ptr,
    size_t right_len,
    uint8_t** out_wav,
    size_t* out_len
);

void img_free_buffer(uint8_t* ptr, size_t len);

size_t img_last_error(uint8_t* buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif

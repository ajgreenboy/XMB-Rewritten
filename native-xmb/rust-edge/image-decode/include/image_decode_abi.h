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

/* Parses and validates a Sony CXML QRC container (QRCC or QRCF v0x110).
 * On success (0):
 *   *out_unpacked_buf / *out_unpacked_len holds the unpacked QRCF container bytes.
 *   *out_toc_buf / *out_toc_len holds:
 *     [u32 data_base LE][u32 entry_count LE]
 *     per entry: [u32 id_len LE][u8 * id_len][u32 offset LE][u32 size LE][u32 expanded LE]
 * Caller frees both buffers with img_free_buffer.
 */
int img_parse_qrc(
    const uint8_t* qrc_ptr,
    size_t qrc_len,
    uint8_t** out_unpacked_buf,
    size_t* out_unpacked_len,
    uint8_t** out_toc_buf,
    size_t* out_toc_len
);

/* Inflates a single zlib-compressed QRC entry bounded to expanded_len (<= 32 MiB). */
int img_inflate_qrc_entry(
    const uint8_t* data_ptr,
    size_t data_len,
    uint32_t expanded_len,
    uint8_t** out_buf,
    size_t* out_len
);

/* Parses and validates a Sony CXML P3TF / RAFO / _RAF container (.p3t theme or .raf scene).
 * On success (0):
 *   *out_unpacked_buf / *out_unpacked_len holds the unpacked P3TF or RAFO container bytes.
 *   *out_tree_buf / *out_tree_len holds the encoded header sections and depth-first element tree.
 * Caller frees both buffers with img_free_buffer.
 */
int img_parse_cxml(
    const uint8_t* cxml_ptr,
    size_t cxml_len,
    uint8_t** out_unpacked_buf,
    size_t* out_unpacked_len,
    uint8_t** out_tree_buf,
    size_t* out_tree_len
);

/* Inflates a zlib-compressed CXML file entry (bounded to 256 MiB), or returns the raw slice bytes
 * unchanged if not a valid zlib stream (matching P3t::file behaviour).
 */
int img_inflate_cxml_file(
    const uint8_t* data_ptr,
    size_t data_len,
    uint8_t** out_buf,
    size_t* out_len
);

/* Parses a Sony PARAM.SFO (\0PSF) binary buffer into packed key-value entries:
 *   [u32 entry_count LE]
 *   per entry: [u32 key_len LE][u8 * key_len][u32 val_len LE][u8 * val_len]
 * Returns 0 on success (caller frees *out_buf with img_free_buffer), non-zero on error.
 */
int img_parse_sfo(
    const uint8_t* sfo_ptr,
    size_t sfo_len,
    uint8_t** out_buf,
    size_t* out_len
);

/* Decodes a DDS texture (DXT1/3/5 or 32-bit bitmask uncompressed) into native-endian ARGB32. */
int img_decode_dds(
    const uint8_t* dds_ptr,
    size_t dds_len,
    uint32_t* out_width,
    uint32_t* out_height,
    uint8_t** out_pixels,
    size_t* out_len
);

/* Decodes an uncompressed or RLE TGA texture (8/24/32-bpp) into native-endian ARGB32. */
int img_decode_tga(
    const uint8_t* tga_ptr,
    size_t tga_len,
    uint32_t* out_width,
    uint32_t* out_height,
    uint8_t** out_pixels,
    size_t* out_len
);

void img_free_buffer(uint8_t* ptr, size_t len);

/* Parses a PS3 RCO resource container (\0FRP, big-endian).
 * On success (0): *out_tables holds the table section (the file, or the 0xA4 header + inflated tables) and *out_toc:
 *   [u32 labels LE][u32 image_data LE][u32 count LE] then per entry [u32 name_len LE][name utf8][u32 offset LE].
 * Caller frees both buffers with img_free_buffer.
 */
int img_parse_rco(
    const uint8_t* rco_ptr,
    size_t rco_len,
    uint8_t** out_tables_buf,
    size_t* out_tables_len,
    uint8_t** out_toc_buf,
    size_t* out_toc_len
);

/* The English (language 1) string table of an RCO parsed by img_parse_rco:
 *   [u32 count LE] then per entry [u32 key_len LE][key utf8][u32 unit_count LE][u16 LE UTF-16 units * unit_count].
 */
int img_rco_english(
    const uint8_t* file_ptr,
    size_t file_len,
    const uint8_t* tables_ptr,
    size_t tables_len,
    uint32_t labels,
    uint8_t** out_buf,
    size_t* out_len
);

size_t img_last_error(uint8_t* buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif

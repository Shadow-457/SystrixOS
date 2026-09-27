/* ================================================================
 *  Systrix OS — kernel/image.h
 *
 *  Format-independent decoded image container plus the decoders.
 *
 *  Every decoder produces the same thing: 8-bit interleaved samples,
 *  row-major, tightly packed, no padding.  Callers therefore never have
 *  to care whether the source was a PNG, a JPEG or a BMP — which is
 *  what makes a single image viewer, a thumbnailer and a video player
 *  possible instead of one of each.
 *
 *  Pixels are in the framebuffer's channel order (R,G,B[,A]) rather than
 *  PNG's, so converting to the GUI's 0x00RRGGBB is a straight copy.
 * ================================================================ */
#ifndef SYSTRIX_IMAGE_H
#define SYSTRIX_IMAGE_H

#include "kernel.h"

/* ── Formats ─────────────────────────────────────────────────── */
typedef enum {
    IMG_UNKNOWN = 0,
    IMG_PNG,
    IMG_JPEG,
    IMG_BMP,
    IMG_TGA,
    IMG_PCX,
    IMG_QOI,
    IMG_ICO,
} image_fmt;

/* ── Error codes (negative, from the decoders) ───────────────── */
#define IMG_OK             0
#define IMG_ERR_FORMAT   (-1)   /* not a file of this kind              */
#define IMG_ERR_UNSUPPORTED (-2)/* recognised but variant not handled   */
#define IMG_ERR_CORRUPT  (-3)   /* structurally broken                  */
#define IMG_ERR_MEMORY   (-4)   /* out of memory                        */
#define IMG_ERR_TRUNCATED (-5)  /* file ended early                     */
#define IMG_ERR_TOOBIG   (-6)   /* exceeds IMG_MAX_DIM                  */
#define IMG_ERR_IO       (-7)   /* could not read the file              */

/* Guard rails.  A 4096x4096 RGBA image is 64 MiB, which already
 * exceeds the 2 MiB kernel heap, so callers get a clean error rather
 * than a failed allocation half way through decoding. */
#define IMG_MAX_DIM   4096
#define IMG_MAX_PIXELS (IMG_MAX_DIM * IMG_MAX_DIM)

/* ── Decoded image ───────────────────────────────────────────── */
typedef struct {
    int    width;
    int    height;
    int    channels;        /* 1 = grey, 3 = RGB, 4 = RGBA          */
    int    has_alpha;
    u8    *pixels;          /* width*height*channels bytes          */
    usize  size;            /* allocation size, for image_free()     */
    u32    stride;          /* bytes per row = width*channels       */
} image_t;

/* ── Public API ──────────────────────────────────────────────── */

/* Identify a format from magic bytes.  Returns IMG_UNKNOWN if none match. */
image_fmt image_probe(const u8 *data, usize len);

/* Human-readable names, for error messages and the file manager. */
const char *image_fmt_name(image_fmt f);
const char *image_fmt_ext(image_fmt f);
const char *image_error(i64 err);

/* Decode a buffer that is already in memory.  On success `out` owns a
 * heap allocation; release it with image_free(). */
i64 image_decode(const u8 *data, usize len, image_t *out);

/* Read a file off the FAT32 volume and decode it. */
i64 image_load(const char *path, image_t *out);

void image_free(image_t *img);

/* Convert to the GUI's packed 0x00RRGGBB words (alpha discarded).
 * `dst` must hold width*height u32s. */
void image_to_xrgb32(const image_t *src, u32 *dst);

/* Allocate and fill a 0x00RRGGBB buffer.  Returns NULL on failure. */
u32 *image_to_xrgb32_alloc(const image_t *src);

/* Box-filter downscale (or nearest upscale) into a new image. */
i64 image_scale(const image_t *src, int w, int h, image_t *out);

/* ── Individual decoders (exposed for testing) ───────────────── */
i64 png_decode (const u8 *d, usize n, image_t *out);
i64 jpeg_decode(const u8 *d, usize n, image_t *out);
i64 bmp_decode (const u8 *d, usize n, image_t *out);
i64 tga_decode (const u8 *d, usize n, image_t *out);
i64 pcx_decode (const u8 *d, usize n, image_t *out);
i64 qoi_decode (const u8 *d, usize n, image_t *out);

/* ── DEFLATE (kernel/inflate.c) ──────────────────────────────── */
int  inflate_raw_stream(const u8 *in, usize in_len, u8 *out, usize out_len);
int  zlib_inflate(const u8 *in, usize in_len, u8 *out, usize out_len);
int  zlib_inflate_buf(const u8 *in, usize in_len, u8 *out, usize out_cap, usize *written);

#endif /* SYSTRIX_IMAGE_H */

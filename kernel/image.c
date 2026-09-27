/* ================================================================
 *  Systrix OS — kernel/image.c
 *
 *  Format dispatch and the shared post-processing helpers.
 *
 *  Each format lives in its own file and exposes a single
 *  `xxx_decode(data, len, out)` entry point.  This module sniffs the
 *  magic bytes, calls the right decoder, and owns the generic
 *  conversions (XRGB32 packing, rescaling) that every caller wants.
 * ================================================================ */
#include "image.h"

/* ── Magic-byte sniffing ─────────────────────────────────────── */

image_fmt image_probe(const u8 *d, usize n)
{
    if (!d) return IMG_UNKNOWN;

    /* PNG: 89 50 4E 47 0D 0A 1A 0A */
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A)
        return IMG_PNG;

    /* JPEG: FF D8 FF */
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return IMG_JPEG;

    /* BMP: "BM" */
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return IMG_BMP;

    /* QOI: "qoif" */
    if (n >= 4 && d[0] == 'q' && d[1] == 'o' && d[2] == 'i' && d[3] == 'f')
        return IMG_QOI;

    /* PCX: 0x0A, version, encoding, bpp */
    if (n >= 4 && d[0] == 0x0A && (d[1] == 0 || d[1] == 2 || d[1] == 3 ||
                                    d[1] == 4 || d[1] == 5))
        return IMG_PCX;

    /* TGA has no magic: footer "TRUEVISION-XFILE.\0" at the end, or an
     * old-style header with no extension area.  Check the footer first,
     * then fall back to a plausibility test on the header. */
    if (n >= 18) {
        const char *foot = "TRUEVISION-XFILE.";
        if (n >= 26 && memcmp(d + n - 18, foot, 18) == 0) return IMG_TGA;
        u8  idlen   = d[0];
        u8  cmaptype= d[1];
        u8  imgtype = d[2];
        int ok = 1;
        if (imgtype != 1 && imgtype != 2 && imgtype != 3 &&
            imgtype != 9 && imgtype != 10 && imgtype != 11) ok = 0;
        if (cmaptype > 1) ok = 0;
        if (d[1] == 0 && d[2] == 0) ok = 0;         /* colourmapped w/o map */
        if (n >= 18 + idlen && ok) {
            int w = d[12] | (d[13] << 8);
            int h = d[14] | (d[15] << 8);
            if (w > 0 && h > 0) return IMG_TGA;
        }
    }

    /* ICO: 00 00 01 00 */
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0)
        return IMG_ICO;

    return IMG_UNKNOWN;
}

const char *image_fmt_name(image_fmt f)
{
    switch (f) {
    case IMG_PNG:  return "PNG";
    case IMG_JPEG: return "JPEG";
    case IMG_BMP:  return "BMP";
    case IMG_TGA:  return "TGA";
    case IMG_PCX:  return "PCX";
    case IMG_QOI:  return "QOI";
    case IMG_ICO:  return "ICO";
    default:       return "unknown";
    }
}

const char *image_fmt_ext(image_fmt f)
{
    switch (f) {
    case IMG_PNG:  return "PNG";
    case IMG_JPEG: return "JPG";
    case IMG_BMP:  return "BMP";
    case IMG_TGA:  return "TGA";
    case IMG_PCX:  return "PCX";
    case IMG_QOI:  return "QOI";
    case IMG_ICO:  return "ICO";
    default:       return "???";
    }
}

const char *image_error(i64 e)
{
    switch (e) {
    case IMG_OK:               return "ok";
    case IMG_ERR_FORMAT:       return "not a supported image format";
    case IMG_ERR_UNSUPPORTED:  return "variant of this format is not supported";
    case IMG_ERR_CORRUPT:      return "file is corrupt";
    case IMG_ERR_MEMORY:       return "out of memory";
    case IMG_ERR_TRUNCATED:    return "file is truncated";
    case IMG_ERR_TOOBIG:       return "image is too large";
    case IMG_ERR_IO:           return "cannot read file";
    default:                   return "unknown error";
    }
}

/* ── Dispatch ────────────────────────────────────────────────── */

i64 image_decode(const u8 *data, usize len, image_t *out)
{
    if (!data || !out) return IMG_ERR_FORMAT;
    out->pixels = NULL; out->size = 0; out->width = out->height = 0;
    out->channels = out->has_alpha = 0; out->stride = 0;

    switch (image_probe(data, len)) {
    case IMG_PNG:  return png_decode(data, len, out);
    case IMG_JPEG: return jpeg_decode(data, len, out);
    case IMG_BMP:  return bmp_decode(data, len, out);
    case IMG_TGA:  return tga_decode(data, len, out);
    case IMG_PCX:  return pcx_decode(data, len, out);
    case IMG_QOI:  return qoi_decode(data, len, out);
    default:       return IMG_ERR_FORMAT;
    }
}

/* ── File loading ────────────────────────────────────────────── */

#define IMG_MAX_FILE (48 * 1024 * 1024)

i64 image_load(const char *path, image_t *out)
{
    if (!path || !out) return IMG_ERR_FORMAT;
    out->pixels = NULL; out->size = 0; out->width = out->height = 0;
    out->channels = out->has_alpha = 0; out->stride = 0;

    i64 fd = vfs_open(path);
    if (fd < 0) return IMG_ERR_IO;

    u8 *buf = (u8 *)heap_malloc(IMG_MAX_FILE);
    if (!buf) { vfs_close((u64)fd); return IMG_ERR_MEMORY; }

    usize total = 0;
    for (;;) {
        i64 n = vfs_read((u64)fd, buf + total, 8192);
        if (n <= 0) break;
        total += (usize)n;
        if (total >= IMG_MAX_FILE) break;
    }
    vfs_close((u64)fd);

    if (total == 0) { heap_free(buf); return IMG_ERR_TRUNCATED; }

    i64 rc = image_decode(buf, total, out);
    heap_free(buf);
    return rc;
}

void image_free(image_t *img)
{
    if (!img) return;
    if (img->pixels) heap_free(img->pixels);
    img->pixels = NULL;
    img->size = 0;
    img->width = img->height = 0;
    img->channels = img->has_alpha = 0;
    img->stride = 0;
}

/* ── Conversions ─────────────────────────────────────────────── */

void image_to_xrgb32(const image_t *src, u32 *dst)
{
    if (!src || !src->pixels || !dst) return;

    int ch = src->channels;
    usize n = (usize)src->width * src->height;

    if (ch == 3) {
        const u8 *p = src->pixels;
        for (usize i = 0; i < n; i++, p += 3)
            dst[i] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
    } else if (ch == 4) {
        const u8 *p = src->pixels;
        for (usize i = 0; i < n; i++, p += 4) {
            /* Composite against a mid-dark backdrop so PNGs with
             * transparency read sensibly on the desktop. */
            u32 a = p[3];
            u32 r = (((u32)p[0] * a) + 0x1A * (255 - a)) / 255;
            u32 g = (((u32)p[1] * a) + 0x1E * (255 - a)) / 255;
            u32 b = (((u32)p[2] * a) + 0x24 * (255 - a)) / 255;
            dst[i] = (r << 16) | (g << 8) | b;
        }
    } else { /* 1 = grey */
        const u8 *p = src->pixels;
        for (usize i = 0; i < n; i++, p++) {
            u32 v = p[0];
            dst[i] = (v << 16) | (v << 8) | v;
        }
    }
}

u32 *image_to_xrgb32_alloc(const image_t *src)
{
    if (!src || !src->pixels) return NULL;
    u32 *d = (u32 *)heap_malloc((usize)src->width * src->height * 4);
    if (!d) return NULL;
    image_to_xrgb32(src, d);
    return d;
}

/* ── Scaling ─────────────────────────────────────────────────── */

/* Box filter when downscaling (keeps detail instead of point-sampling),
 * nearest neighbour when upscaling. */
i64 image_scale(const image_t *src, int w, int h, image_t *out)
{
    if (!src || !src->pixels || !out || w <= 0 || h <= 0) return IMG_ERR_FORMAT;
    if ((i64)w * h > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;

    usize need = (usize)w * h * (usize)src->channels;
    u8 *dst = (u8 *)heap_malloc(need);
    if (!dst) return IMG_ERR_MEMORY;

    int ch = src->channels;
    if (w == src->width && h == src->height) {
        memcpy(dst, src->pixels, need);
    } else if (w < src->width || h < src->height) {
        for (int y = 0; y < h; y++) {
            int y0 = (int)(((i64)y * src->height) / h);
            int y1 = (int)(((i64)(y + 1) * src->height) / h);
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > src->height) y1 = src->height;
            for (int x = 0; x < w; x++) {
                int x0 = (int)(((i64)x * src->width) / w);
                int x1 = (int)(((i64)(x + 1) * src->width) / w);
                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > src->width) x1 = src->width;

                u32 acc[4] = { 0, 0, 0, 0 };
                u32 cnt = 0;
                for (int yy = y0; yy < y1; yy++) {
                    const u8 *row = src->pixels + (usize)yy * src->stride;
                    for (int xx = x0; xx < x1; xx++) {
                        const u8 *p = row + (usize)xx * ch;
                        for (int c = 0; c < ch; c++) acc[c] += p[c];
                        cnt++;
                    }
                }
                if (!cnt) cnt = 1;
                u8 *o = dst + ((usize)y * w + x) * ch;
                for (int c = 0; c < ch; c++) o[c] = (u8)(acc[c] / cnt);
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            int sy = (int)(((i64)y * src->height) / h);
            const u8 *row = src->pixels + (usize)sy * src->stride;
            u8 *o = dst + (usize)y * w * ch;
            for (int x = 0; x < w; x++) {
                int sx = (int)(((i64)x * src->width) / w);
                memcpy(o + (usize)x * ch, row + (usize)sx * ch, (usize)ch);
            }
        }
    }

    out->width = w; out->height = h;
    out->channels = ch; out->has_alpha = src->has_alpha;
    out->pixels = dst; out->size = need;
    out->stride = (u32)(w * ch);
    return IMG_OK;
}

/* ================================================================
 *  Systrix OS — kernel/png.c
 *
 *  PNG decoder (ISO/IEC 15948).
 *
 *  Supported, i.e. everything a real encoder produces:
 *    * colour types 0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha),
 *      6 (RGBA)
 *    * bit depths 1/2/4/8/16
 *    * all five scanline filters (None, Sub, Up, Average, Paeth)
 *    * tRNS transparency for types 0, 2 and 3
 *    * interlace: none and Adam7
 *    * multiple IDAT chunks, arbitrary ancillary chunks ignored
 *
 *  The old viewer only handled 8-bit RGB with *stored* deflate blocks,
 *  which in practice meant it could open almost nothing.  The real
 *  blocker was kernel/inflate.c.
 * ================================================================ */
#include "image.h"

/* ── CRC-32 (PNG uses the standard IEEE polynomial) ───────────── */

static u32 png_crc_table[256];
static int png_crc_ready = 0;

static void png_crc_init(void)
{
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        png_crc_table[n] = c;
    }
    png_crc_ready = 1;
}

static u32 png_crc(const u8 *buf, usize len)
{
    if (!png_crc_ready) png_crc_init();
    u32 c = 0xFFFFFFFFu;
    for (usize i = 0; i < len; i++)
        c = png_crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── Chunk walking ───────────────────────────────────────────── */

static u32 png_rd32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Header of the image we are assembling. */
typedef struct {
    u32 width, height;
    int bit_depth;
    int color_type;      /* 0,2,3,4,6                        */
    int interlace;       /* 0 = none, 1 = Adam7              */
    int channels;        /* samples per pixel before packing */
    int bits_per_px;     /* channels * bit_depth              */
    int filter_bpp;      /* filter offset: bytes per pixel, >= 1 */
} png_hdr;

static int hdr_channels(int color_type)
{
    switch (color_type) {
    case 0: return 1;   /* grey            */
    case 2: return 3;   /* RGB             */
    case 3: return 1;   /* palette index   */
    case 4: return 2;   /* grey + alpha    */
    case 6: return 4;   /* RGBA            */
    default: return 0;
    }
}

/* ── Scanline reconstruction ─────────────────────────────────── */

static int paeth(int a, int b, int c)
{
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Undo the per-scanline filter.  `prev` may be NULL for the first row. */
static int png_unfilter(int filter, u8 *line, const u8 *prev, usize len, int bpp)
{
    switch (filter) {
    case 0:
        break;
    case 1:
        for (usize i = (usize)bpp; i < len; i++)
            line[i] = (u8)(line[i] + line[i - bpp]);
        break;
    case 2:
        if (prev) for (usize i = 0; i < len; i++) line[i] = (u8)(line[i] + prev[i]);
        break;
    case 3:
        for (usize i = 0; i < len; i++) {
            int a = (i >= (usize)bpp) ? line[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            line[i] = (u8)(line[i] + ((a + b) >> 1));
        }
        break;
    case 4:
        for (usize i = 0; i < len; i++) {
            int a = (i >= (usize)bpp) ? line[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (usize)bpp) ? prev[i - bpp] : 0;
            line[i] = (u8)(line[i] + paeth(a, b, c));
        }
        break;
    default:
        return IMG_ERR_CORRUPT;
    }
    return IMG_OK;
}

/* ── Sample extraction ───────────────────────────────────────── */

/* Read sample `i` of `depth` bits from a packed scanline. */
static inline u32 get_sample(const u8 *row, usize i, int depth)
{
    switch (depth) {
    case 8:  return row[i];
    case 16: return ((u32)row[i * 2] << 8) | row[i * 2 + 1];
    case 1:  return (row[i >> 3] >> (7 - (i & 7))) & 1;
    case 2:  return (row[i >> 2] >> (6 - ((i & 3) * 2))) & 3;
    case 4:  return (row[i >> 1] >> ((i & 1) ? 0 : 4)) & 0xF;
    default: return 0;
    }
}

/* Scale an n-bit sample up to 8 bits (bit replication, so 0b101 -> 0xAA). */
static inline u8 scale_to8(u32 v, int depth)
{
    switch (depth) {
    case 1:  return (u8)(v ? 0xFF : 0x00);
    case 2:  return (u8)(v * 85);
    case 4:  return (u8)(v * 17);
    case 8:  return (u8)v;
    case 16: return (u8)(v >> 8);
    default: return (u8)v;
    }
}

/* Write one final RGBA (or RGB) pixel. */
static void put_pixel(u8 *dst, usize idx, int out_ch, const u32 *s, int depth,
                      int grey_is_rgb)
{
    if (out_ch >= 3 && grey_is_rgb) {
        u8 g = scale_to8(s[0], depth);
        dst[idx * out_ch + 0] = g;
        dst[idx * out_ch + 1] = g;
        dst[idx * out_ch + 2] = g;
        if (out_ch == 4) dst[idx * out_ch + 3] = 0xFF;
        return;
    }
    for (int c = 0; c < out_ch && c < 4; c++)
        dst[idx * out_ch + c] = scale_to8(s[c], depth);
}

/* ── Interlace (Adam7) ───────────────────────────────────────── */

/* x origin, y origin, x step, y step for each of the 7 passes. */
static const u8 adam7_x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const u8 adam7_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const u8 adam7_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const u8 adam7_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

/* ── Decoder ─────────────────────────────────────────────────── */

i64 png_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 8) return IMG_ERR_TRUNCATED;

    png_hdr h;
    h.width = h.height = 0;
    h.bit_depth = 8; h.color_type = 6; h.interlace = 0;

    const u8 *idat = NULL;        /* concatenated IDAT payload  */
    usize     idat_len = 0;
    usize     idat_cap = 0;

    u8 palette[256 * 3];
    u8 palette_alpha[256];
    int have_palette = 0, have_palpha = 0;
    u32 trns_key[3] = { 0, 0, 0 };
    int  trns_key_valid = 0;
    int  have_trns = 0;

    usize pos = 8;
    int got_ihdr = 0, got_iend = 0;

    while (pos + 8 <= n) {
        u32 clen = png_rd32(d + pos);
        if (clen > 0x7FFFFFFFu) return IMG_ERR_CORRUPT;
        const u8 *ctype = d + pos + 4;
        const u8 *cdata = d + pos + 8;
        if (pos + 12 + (usize)clen > n) return IMG_ERR_TRUNCATED;

        /* Verify the chunk CRC — cheap, and it catches truncated
         * downloads that would otherwise decode into noise. */
        if (png_crc(ctype, (usize)clen + 4) != png_rd32(cdata + clen))
            return IMG_ERR_CORRUPT;

        if (memcmp(ctype, "IHDR", 4) == 0) {
            if (clen < 13) return IMG_ERR_CORRUPT;
            h.width      = png_rd32(cdata);
            h.height     = png_rd32(cdata + 4);
            h.bit_depth  = cdata[8];
            h.color_type = cdata[9];
            if (cdata[10] != 0) return IMG_ERR_UNSUPPORTED;  /* compression */
            if (cdata[11] != 0) return IMG_ERR_UNSUPPORTED;  /* filter      */
            h.interlace  = cdata[12];
            h.channels   = hdr_channels(h.color_type);
            if (!h.channels) return IMG_ERR_FORMAT;
            if (h.interlace > 1) return IMG_ERR_UNSUPPORTED;
            if (h.width == 0 || h.height == 0) return IMG_ERR_CORRUPT;
            if (h.width > IMG_MAX_DIM || h.height > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
            if ((u64)h.width * h.height > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;
            /* Legal depth/colour-type combinations only. */
            switch (h.color_type) {
            case 0:
                if (h.bit_depth != 1 && h.bit_depth != 2 && h.bit_depth != 4 &&
                    h.bit_depth != 8 && h.bit_depth != 16) return IMG_ERR_FORMAT;
                break;
            case 3:
                if (h.bit_depth != 1 && h.bit_depth != 2 &&
                    h.bit_depth != 4 && h.bit_depth != 8) return IMG_ERR_FORMAT;
                break;
            case 2: case 4: case 6:
                if (h.bit_depth != 8 && h.bit_depth != 16) return IMG_ERR_FORMAT;
                break;
            }
            h.bits_per_px = h.channels * h.bit_depth;
            /* Filter "bpp" is bytes per complete pixel, rounded up to 1:
             * 1 for every sub-byte depth, channels for 8-bit, and
             * channels*2 for 16-bit. */
            h.filter_bpp = (h.bits_per_px + 7) / 8;
            if (h.filter_bpp < 1) h.filter_bpp = 1;
            got_ihdr = 1;
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            if (clen % 3 || clen > 768) return IMG_ERR_CORRUPT;
            memcpy(palette, cdata, clen);
            have_palette = 1;
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            if (h.color_type == 3) {
                if (clen > 256) return IMG_ERR_CORRUPT;
                memset(palette_alpha, 0xFF, sizeof palette_alpha);
                memcpy(palette_alpha, cdata, clen);
                have_palpha = 1;
                have_trns = 1;
            } else if (h.color_type == 0 && clen >= 2) {
                trns_key[0] = trns_key[1] = ((u32)cdata[0] << 8) | cdata[1];
                trns_key_valid = 1; have_trns = 1;
            } else if (h.color_type == 2 && clen >= 6) {
                trns_key[0] = ((u32)cdata[0] << 8) | cdata[1];
                trns_key[1] = ((u32)cdata[2] << 8) | cdata[3];
                trns_key[2] = ((u32)cdata[4] << 8) | cdata[5];
                trns_key_valid = 1; have_trns = 1;
            }
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (!idat) { idat_cap = clen * 2 + 1024; idat = (const u8 *)heap_malloc(idat_cap); }
            else if (idat_len + clen > idat_cap) {
                usize nc = idat_cap * 2;
                while (nc < idat_len + clen) nc *= 2;
                const u8 *nd = (const u8 *)heap_malloc(nc);
                memcpy((void *)nd, idat, idat_len);
                heap_free((void *)idat);
                idat = nd; idat_cap = nc;
            }
            memcpy((void *)(idat + idat_len), cdata, clen);
            idat_len += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            got_iend = 1;
            break;
        }

        pos += 12 + (usize)clen;
    }

    if (!got_ihdr) return IMG_ERR_TRUNCATED;
    if (!idat || idat_len == 0) return IMG_ERR_TRUNCATED;
    if (h.color_type == 3 && !have_palette) return IMG_ERR_CORRUPT;
    (void)got_iend;

    /* ── Output buffer ──────────────────────────────────────── */
    /* image_t promises 1, 3 or 4 channels, so grey is expanded to RGB
     * and grey+alpha to RGBA.  Keeping the channel count in that set
     * means image_to_xrgb32() needs no 2-channel special case. */
    int out_ch;
    switch (h.color_type) {
    case 0:  out_ch = 3; break;                  /* grey  -> RGB  */
    case 2:  out_ch = 3; break;                  /* RGB            */
    case 3:  out_ch = have_palpha ? 4 : 3; break;/* palette +/- a  */
    case 4:  out_ch = 4; break;                  /* grey+a -> RGBA */
    default: out_ch = 4; break;                  /* RGBA           */
    }

    usize out_px = (usize)h.width * h.height;
    usize out_len = out_px * (usize)out_ch;
    u8 *pixels = (u8 *)heap_malloc(out_len);
    if (!pixels) { heap_free((void *)idat); return IMG_ERR_MEMORY; }
    memset(pixels, 0xFF, out_len);

    /* ── Inflate the whole IDAT stream ──────────────────────── */
    /* Pass 1: find the decompressed size by inflating into a buffer
     * sized from the header.  Worst case (bit depth 16, 4 channels) the
     * raw stream is height*(1+width*8) for each interlace pass; the
     * Adam7 total is ceil(w/8)*ceil(h/8)*8 + ... which is bounded by
     * (w+7)/8 * (h+7)/8 * 64, so this generous bound always fits. */
    usize max_raw = 0;
    {
        u64 sum_w = 0, sum_h = 0;
        if (!h.interlace) { sum_w = h.width; sum_h = h.height; }
        else {
            for (int p = 0; p < 7; p++) {
                u64 pw = (h.width  + adam7_dx[p] - 1 - adam7_x0[p]) / adam7_dx[p];
                u64 ph = (h.height + adam7_dy[p] - 1 - adam7_y0[p]) / adam7_dy[p];
                sum_w += pw; sum_h += ph;
            }
        }
        u64 per_row = (sum_w * h.bits_per_px + 7) / 8 + 1;
        u64 need = sum_h * per_row + 64;
        if (need > 128ull * 1024 * 1024) { heap_free(pixels); heap_free((void *)idat); return IMG_ERR_TOOBIG; }
        max_raw = (usize)need;
    }

    u8 *raw = (u8 *)heap_malloc(max_raw);
    if (!raw) { heap_free(pixels); heap_free((void *)idat); return IMG_ERR_MEMORY; }

    int got = zlib_inflate(idat, idat_len, raw, max_raw);
    heap_free((void *)idat);
    if (got < 0) { heap_free(raw); heap_free(pixels); return (got == -2) ? IMG_ERR_MEMORY : IMG_ERR_CORRUPT; }
    usize raw_len = (usize)got;

    /* ── Walk the (possibly interlaced) passes ─────────────── */
    usize rp = 0;                        /* cursor into `raw` */
    int bpp = h.filter_bpp;              /* filter offset, >= 1 */

    for (int pass = 0; pass < (h.interlace ? 7 : 1); pass++) {
        u32 pw, ph, x0, y0, dx, dy;
        if (h.interlace) {
            x0 = adam7_x0[pass]; y0 = adam7_y0[pass];
            dx = adam7_dx[pass]; dy = adam7_dy[pass];
            pw = (h.width  + dx - 1 - x0) / dx;
            ph = (h.height + dy - 1 - y0) / dy;
        } else {
            x0 = y0 = 0; dx = dy = 1;
            pw = h.width; ph = h.height;
        }
        if (pw == 0 || ph == 0) continue;

        /* Packed scanlines: sub-byte depths and 16-bit samples both
         * mean the row is ceil(width * bits_per_pixel / 8) bytes. */
        usize line_len = ((usize)pw * (usize)h.bits_per_px + 7) / 8;
        if (line_len == 0) continue;

        /* Two scanline buffers, swapped each row.  Both start zeroed:
         * for the first row of a pass the filters that reference the
         * row above treat it as all-zero, which is exactly right. */
        u8 *rowbuf[2];
        rowbuf[0] = (u8 *)heap_malloc(line_len);
        rowbuf[1] = (u8 *)heap_malloc(line_len);
        if (!rowbuf[0] || !rowbuf[1]) {
            heap_free(rowbuf[0]); heap_free(rowbuf[1]);
            heap_free(raw); heap_free(pixels);
            return IMG_ERR_MEMORY;
        }
        memset(rowbuf[0], 0, line_len);
        memset(rowbuf[1], 0, line_len);
        u8 *cur = rowbuf[1], *prev = rowbuf[0];

        for (u32 py = 0; py < ph; py++) {
            if (rp + 1 + line_len > raw_len) {
                heap_free(rowbuf[0]); heap_free(rowbuf[1]);
                heap_free(raw); heap_free(pixels);
                return IMG_ERR_TRUNCATED;
            }
            int filter = raw[rp++];
            memcpy(cur, raw + rp, line_len);
            rp += line_len;

            int rc = png_unfilter(filter, cur, prev, line_len, bpp);
            if (rc) {
                heap_free(rowbuf[0]); heap_free(rowbuf[1]);
                heap_free(raw); heap_free(pixels);
                return rc;
            }

            u32 dy_ = y0 + py * dy;
            for (u32 px = 0; px < pw; px++) {
                u32 dx_ = x0 + px * dx;
                if (dx_ >= h.width || dy_ >= h.height) continue;
                usize oidx = (usize)dy_ * h.width + dx_;

                if (h.color_type == 3) {
                    u32 pi = get_sample(cur, px, h.bit_depth);
                    if (pi > 255) continue;
                    pixels[oidx * out_ch + 0] = palette[pi * 3 + 0];
                    pixels[oidx * out_ch + 1] = palette[pi * 3 + 1];
                    pixels[oidx * out_ch + 2] = palette[pi * 3 + 2];
                    if (out_ch == 4)
                        pixels[oidx * out_ch + 3] = have_palpha ? palette_alpha[pi] : 0xFF;
                    continue;
                }

                if (h.color_type == 0) {
                    u32 g = get_sample(cur, px, h.bit_depth);
                    u8 v = scale_to8(g, h.bit_depth);
                    int transparent = 0;
                    if (have_trns && trns_key_valid) {
                        u32 g16 = (h.bit_depth == 16) ? g : v;
                        if (g16 == trns_key[0]) transparent = 1;
                    }
                    pixels[oidx * out_ch + 0] = transparent ? 0 : v;
                    pixels[oidx * out_ch + 1] = transparent ? 0 : v;
                    pixels[oidx * out_ch + 2] = transparent ? 0 : v;
                    continue;
                }

                if (h.color_type == 4) {
                    /* grey + alpha, expanded to RGBA */
                    u32 g = get_sample(cur, px * 2 + 0, h.bit_depth);
                    u32 a = get_sample(cur, px * 2 + 1, h.bit_depth);
                    u8 v = scale_to8(g, h.bit_depth);
                    pixels[oidx * out_ch + 0] = v;
                    pixels[oidx * out_ch + 1] = v;
                    pixels[oidx * out_ch + 2] = v;
                    pixels[oidx * out_ch + 3] = scale_to8(a, h.bit_depth);
                    continue;
                }

                if (h.color_type == 6) {
                    /* RGBA straight through; tRNS is not used here. */
                    pixels[oidx * out_ch + 0] = scale_to8(get_sample(cur, px * 4 + 0, h.bit_depth), h.bit_depth);
                    pixels[oidx * out_ch + 1] = scale_to8(get_sample(cur, px * 4 + 1, h.bit_depth), h.bit_depth);
                    pixels[oidx * out_ch + 2] = scale_to8(get_sample(cur, px * 4 + 2, h.bit_depth), h.bit_depth);
                    pixels[oidx * out_ch + 3] = scale_to8(get_sample(cur, px * 4 + 3, h.bit_depth), h.bit_depth);
                    continue;
                }

                /* colour type 2: RGB, possibly with tRNS */
                u32 r = get_sample(cur, px * 3 + 0, h.bit_depth);
                u32 g = get_sample(cur, px * 3 + 1, h.bit_depth);
                u32 b = get_sample(cur, px * 3 + 2, h.bit_depth);
                if (have_trns && trns_key_valid) {
                    u32 r8 = scale_to8(r, h.bit_depth);
                    u32 g8 = scale_to8(g, h.bit_depth);
                    u32 b8 = scale_to8(b, h.bit_depth);
                    u32 kr = (h.bit_depth == 16) ? trns_key[0] : (trns_key[0] >> 8);
                    u32 kg = (h.bit_depth == 16) ? trns_key[1] : (trns_key[1] >> 8);
                    u32 kb = (h.bit_depth == 16) ? trns_key[2] : (trns_key[2] >> 8);
                    if (r8 == kr && g8 == kg && b8 == kb) {
                        pixels[oidx*out_ch]=0; pixels[oidx*out_ch+1]=0; pixels[oidx*out_ch+2]=0;
                        continue;
                    }
                }
                pixels[oidx * out_ch + 0] = scale_to8(r, h.bit_depth);
                pixels[oidx * out_ch + 1] = scale_to8(g, h.bit_depth);
                pixels[oidx * out_ch + 2] = scale_to8(b, h.bit_depth);
            }

            /* Swap for the next row. */
            u8 *t = prev; prev = cur; cur = t;
        }

        heap_free(rowbuf[0]);
        heap_free(rowbuf[1]);
    }

    heap_free(raw);

    out->width = (int)h.width;
    out->height = (int)h.height;
    out->channels = out_ch;
    out->has_alpha = (out_ch == 4);
    out->pixels = pixels;
    out->size = out_len;
    out->stride = (u32)(h.width * out_ch);
    return IMG_OK;
}

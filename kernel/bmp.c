/* ================================================================
 *  Systrix OS — kernel/bmp.c
 *
 *  Windows / OS-2 bitmap decoder.
 *
 *  Covers the uncompressed 1/4/8/16/24/32-bit forms plus the BI_RLE8
 *  and BI_RLE4 compressions, which is everything Windows itself
 *  produces (plus the near-universal 24- and 32-bit exports from
 *  screenshot and paint tools).
 * ================================================================ */
#include "image.h"

static u32 bmp_rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 bmp_rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

i64 bmp_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 14) return IMG_ERR_TRUNCATED;
    if (d[0] != 'B' || d[1] != 'M') return IMG_ERR_FORMAT;

    u32 data_off = bmp_rd32(d + 10);
    u32 hdr_size = bmp_rd32(d + 14);
    if (hdr_size < 12) return IMG_ERR_CORRUPT;

    u32 w, h, bpp = 0, compression = 0, clr_used = 0, pal_entries = 0;
    int  top_down = 0;

    if (hdr_size == 12) {                 /* BITMAPCOREHEADER (OS/2) */
        w = bmp_rd16(d + 18); h = bmp_rd16(d + 20);
        u16 planes = bmp_rd16(d + 22), bits = bmp_rd16(d + 24);
        if (planes != 1) return IMG_ERR_CORRUPT;
        bpp = bits;
    } else {                              /* BITMAPINFOHEADER and later */
        if (hdr_size < 40 || n < 14 + hdr_size) return IMG_ERR_TRUNCATED;
        int sw = (int)bmp_rd32(d + 18);
        int sh = (int)bmp_rd32(d + 22);
        if (sh < 0) { top_down = 1; sh = -sh; }
        w = (u32)sw; h = (u32)sh;
        bpp = bmp_rd16(d + 28);
        compression = bmp_rd32(d + 30);
        clr_used = bmp_rd32(d + 46);
        if (hdr_size >= 52 && (compression == 3 || compression == 6))
            return IMG_ERR_UNSUPPORTED;   /* bitfields: needs a mask table */
    }
    if (hdr_size == 64 || hdr_size == 108 || hdr_size == 124) compression = 3;

    if (w == 0 || h == 0) return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
    if ((u64)w * h > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;

    /* Palette, if any. */
    u8 pal[256 * 4];
    usize pal_off = 14 + hdr_size;
    if (bpp <= 8) {
        pal_entries = clr_used ? clr_used : (1u << bpp);
        if (pal_entries > 256) return IMG_ERR_CORRUPT;
        usize need = pal_off + pal_entries * (hdr_size == 12 ? 3 : 4);
        if (need > n) return IMG_ERR_TRUNCATED;
        for (u32 i = 0; i < pal_entries; i++) {
            const u8 *p = d + pal_off + i * (hdr_size == 12 ? 3 : 4);
            pal[i * 4 + 0] = p[2];       /* stored BGR */
            pal[i * 4 + 1] = p[1];
            pal[i * 4 + 2] = p[0];
            pal[i * 4 + 3] = 0xFF;
        }
    }
    if (data_off == 0) data_off = pal_off + pal_entries * (bpp <= 8 ? 4 : 0);
    if (data_off >= n) return IMG_ERR_TRUNCATED;

    usize out_len = (usize)w * h * 3;
    u8 *px = (u8 *)heap_malloc(out_len);
    if (!px) return IMG_ERR_MEMORY;
    memset(px, 0, out_len);

    u32 row_size = ((u32)w * bpp + 31u) / 32u * 4u;

    if (compression == 1 || compression == 2) {
        /* ── RLE8 / RLE4 ────────────────────────────────────── */
        if (bpp != 8 && bpp != 4) { heap_free(px); return IMG_ERR_UNSUPPORTED; }
        usize p = data_off;
        u32 x = 0, y = 0;
        int  row_pad = 0;   /* RLE rows are byte-aligned to 2 bytes */
        while (p + 1 < n) {
            u8 count = d[p++], val = d[p++];
            if (count > 0) {
                for (u8 k = 0; k < count; k++) {
                    u8 idx = (bpp == 8) ? val
                           : ((k & 1) ? (val & 0x0F) : (val >> 4));
                    if (x < w) {
                        u32 dy = top_down ? y : (h - 1 - y);
                        u8 *o = px + ((usize)dy * w + x) * 3;
                        o[0] = pal[idx*4+0]; o[1] = pal[idx*4+1]; o[2] = pal[idx*4+2];
                    }
                    x++;
                }
            } else if (val == 0) {                 /* end of line  */
                y++; x = 0; row_pad ^= 1;
                if (row_pad && p < n) p++;         /* pad to word  */
            } else if (val == 1) {                 /* end of bitmap*/
                goto rle_done;
            } else if (val == 2) {                 /* delta        */
                if (p + 1 >= n) break;
                x += d[p++];
                y += d[p++];
            } else {                               /* absolute run */
                u8 c = val;
                if (bpp == 8) {
                    if (p + c > n) break;
                    for (u8 k = 0; k < c; k++, x++) {
                        if (x >= w) break;
                        u8 idx = d[p + k];
                        u32 dy = top_down ? y : (h - 1 - y);
                        u8 *o = px + ((usize)dy * w + x) * 3;
                        o[0] = pal[idx*4+0]; o[1] = pal[idx*4+1]; o[2] = pal[idx*4+2];
                    }
                    p += c;
                } else {
                    usize bytes = (c + 1) / 2;
                    if (p + bytes > n) break;
                    for (u8 k = 0; k < c; k++, x++) {
                        if (x >= w) break;
                        u8 byte = d[p + (k >> 1)];
                        u8 idx = (k & 1) ? (byte & 0x0F) : (byte >> 4);
                        u32 dy = top_down ? y : (h - 1 - y);
                        u8 *o = px + ((usize)dy * w + x) * 3;
                        o[0] = pal[idx*4+0]; o[1] = pal[idx*4+1]; o[2] = pal[idx*4+2];
                    }
                    p += bytes;
                }
                if (p & 1) p++;                     /* pad to word  */
            }
            if (y >= h) break;
        }
    rle_done: ;
    } else if (compression == 0) {
        /* ── Uncompressed ───────────────────────────────────── */
        for (u32 row = 0; row < h; row++) {
            usize base = data_off + (usize)row * row_size;
            if (base + row_size > n && base >= n) break;
            u32 dy = top_down ? row : (h - 1 - row);
            u8 *o = px + (usize)dy * w * 3;
            for (u32 x = 0; x < w; x++) {
                const u8 *s = d + base + (usize)x * (bpp / 8);
                u8 r, g, b;
                if (bpp == 1) {
                    u32 idx = (d[base + (x >> 3)] >> (7 - (x & 7))) & 1;
                    r = pal[idx*4+0]; g = pal[idx*4+1]; b = pal[idx*4+2];
                } else if (bpp == 4) {
                    u32 idx = (d[base + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 0xF;
                    r = pal[idx*4+0]; g = pal[idx*4+1]; b = pal[idx*4+2];
                } else if (bpp == 8) {
                    u32 idx = s[0];
                    r = pal[idx*4+0]; g = pal[idx*4+1]; b = pal[idx*4+2];
                } else if (bpp == 16) {
                    u32 v = (u32)s[0] | ((u32)s[1] << 8);
                    r = (u8)(((v >> 10) & 0x1F) * 255 / 31);
                    g = (u8)(((v >> 5)  & 0x1F) * 255 / 31);
                    b = (u8)(( v        & 0x1F) * 255 / 31);
                } else if (bpp == 24) {
                    b = s[0]; g = s[1]; r = s[2];
                } else { /* 32 */
                    b = s[0]; g = s[1]; r = s[2];
                }
                o[x*3+0] = r; o[x*3+1] = g; o[x*3+2] = b;
            }
        }
    } else {
        heap_free(px);
        return IMG_ERR_UNSUPPORTED;
    }

    out->width = (int)w;
    out->height = (int)h;
    out->channels = 3;
    out->has_alpha = 0;
    out->pixels = px;
    out->size = out_len;
    out->stride = w * 3;
    return IMG_OK;
}

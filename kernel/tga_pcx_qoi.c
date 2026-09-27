/* ================================================================
 *  Systrix OS — kernel/tga.c  +  kernel/pcx.c  +  kernel/qoi.c
 *
 *  Three small legacy formats, in one file because each is a couple of
 *  hundred lines and they share the same "read header, expand, write
 *  RGB" shape.  They are the formats Screenshot/Grab still emit on
 *  DOS-era tools, and QOI is the modern tiny-lossless one.
 * ================================================================ */
#include "image.h"

static u32 le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

/* ================================================================
 *  TGA  (Truevision)
 * ================================================================ */

i64 tga_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 18) return IMG_ERR_TRUNCATED;

    u8  idlen    = d[0];
    u8  cmaptype = d[1];
    u8  imgtype  = d[2];
    u32 cmaplen  = le16(d + 5);
    u8  cmapbits = d[7];
    u32 w = le16(d + 12), h = le16(d + 14);
    u8  bpp   = d[16];
    u8  desc  = d[17];

    if (w == 0 || h == 0) return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
    if ((u64)w * h > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;

    /* RLE variants use the top nibble of the image type. */
    int rle = 0;
    switch (imgtype) {
    case 1: case 9: case 3: rle = 0; break;   /* uncompressed      */
    case 2: case 10: case 11: rle = 1; break; /* RLE               */
    default: return IMG_ERR_UNSUPPORTED;      /* colour-mapped not handled */
    }

    usize p = 18 + idlen;

    /* Palette (5 or 6 bytes per entry, BGR(A)). */
    u8 pal[256 * 4];
    if (cmaptype == 1) {
        usize esz = (cmapbits == 6) ? 4 : 3;
        if (p + cmaplen * esz > n) return IMG_ERR_TRUNCATED;
        for (u32 i = 0; i < cmaplen && i < 256; i++) {
            const u8 *c = d + p + i * esz;
            pal[i*4+0] = c[2]; pal[i*4+1] = c[1]; pal[i*4+2] = c[0];
            pal[i*4+3] = (esz == 4) ? c[3] : 0xFF;
        }
        p += cmaplen * esz;
    }

    int out_ch = 4;                        /* keep alpha if the file has it */
    usize out_len = (usize)w * h * out_ch;
    u8 *px = (u8 *)heap_malloc(out_len);
    if (!px) return IMG_ERR_MEMORY;
    memset(px, 0xFF, out_len);

    int src_bytes = bpp / 8;
    if (src_bytes < 1) { heap_free(px); return IMG_ERR_UNSUPPORTED; }

    int top_down = (desc & 0x20) != 0;
    int right_left = (desc & 0x10) != 0;
    u32 total = w * h, done = 0;

    while (done < total) {
        int run = 1;
        if (rle) {
            if (p >= n) break;
            u8 pkt = d[p++];
            run = (pkt & 0x7F) + 1;
            if (pkt & 0x80) {
                if (p + src_bytes > n) break;
                u8 px4[4];
                for (int b = 0; b < src_bytes; b++) px4[b] = d[p + b];
                p += src_bytes;
                for (int k = 0; k < run && done < total; k++, done++) {
                    u32 sx = done % w, sy = done / w;
                    u8 *o = px + ((usize)sy * w + (right_left ? (w - 1 - sx) : sx)) * out_ch;
                    if (bpp == 32) { o[0]=px4[2]; o[1]=px4[1]; o[2]=px4[0]; o[3]=px4[3]; }
                    else { o[0]=px4[2]; o[1]=px4[1]; o[2]=px4[0]; o[3]=0xFF; }
                }
                continue;
            }
        }
        for (int k = 0; k < run && done < total; k++, done++) {
            if (p + src_bytes > n) { done = total; break; }
            u32 sx = done % w, sy = done / w;
            u32 fy = top_down ? sy : (h - 1 - sy);
            u32 fx = right_left ? (w - 1 - sx) : sx;
            u8 *o = px + ((usize)fy * w + fx) * out_ch;
            if (cmaptype == 1) {
                u32 idx = d[p];
                o[0] = pal[idx*4+0]; o[1] = pal[idx*4+1]; o[2] = pal[idx*4+2];
                o[3] = pal[idx*4+3];
            } else if (bpp == 24) {
                o[0]=d[p+2]; o[1]=d[p+1]; o[2]=d[p+0]; o[3]=0xFF;
            } else if (bpp == 32) {
                o[0]=d[p+2]; o[1]=d[p+1]; o[2]=d[p+0]; o[3]=d[p+3];
            } else {                      /* 15/16-bit BGR */
                u32 v = (u32)d[p] | ((u32)d[p+1] << 8);
                o[0] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
                o[1] = (u8)(((v >> 5)  & 0x1F) * 255 / 31);
                o[2] = (u8)(( v        & 0x1F) * 255 / 31);
                o[3] = 0xFF;
            }
            p += src_bytes;
        }
    }

    out->width = (int)w; out->height = (int)h;
    out->channels = out_ch; out->has_alpha = (bpp == 32);
    out->pixels = px; out->size = out_len;
    out->stride = w * out_ch;
    return IMG_OK;
}

/* ================================================================
 *  PCX  (ZSoft)
 * ================================================================ */

i64 pcx_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 128) return IMG_ERR_TRUNCATED;
    if (d[0] != 0x0A) return IMG_ERR_FORMAT;

    u8  version = d[1];
    u8  encoding = d[2];
    u8  bpp = d[3];
    u32 xmin = le16(d + 4), ymin = le16(d + 6);
    u32 xmax = le16(d + 8), ymax = le16(d + 10);
    u32 w = xmax - xmin + 1, h = ymax - ymin + 1;

    if (encoding != 1) return IMG_ERR_UNSUPPORTED;
    if (w == 0 || h == 0) return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
    if ((u64)w * h > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;

    u8  nplanes = d[65];
    if (bpp == 8 && nplanes == 3) {
        /* 24-bit true colour; the palette at the end is not needed. */
        usize row_bytes = (w * 3 + 1) & ~1u;
        usize need = (row_bytes + 1) * h;
        if (need > n) return IMG_ERR_TRUNCATED;

        u8 *raw = (u8 *)heap_malloc(row_bytes * h);
        if (!raw) return IMG_ERR_MEMORY;
        usize p = 128, o = 0;
        while (o < row_bytes * h) {
            if (p >= n) { heap_free(raw); return IMG_ERR_TRUNCATED; }
            u8 c = d[p++];
            if (c & 0xC0) {
                usize cnt = c & 0x3F;
                if (p >= n) { heap_free(raw); return IMG_ERR_TRUNCATED; }
                u8 v = d[p++];
                while (cnt-- && o < row_bytes * h) raw[o++] = v;
            } else {
                raw[o++] = c;
            }
        }

        usize out_len = (usize)w * h * 3;
        u8 *px = (u8 *)heap_malloc(out_len);
        if (!px) { heap_free(raw); return IMG_ERR_MEMORY; }
        for (u32 y = 0; y < h; y++) {
            const u8 *s = raw + (usize)y * row_bytes;
            u8 *o2 = px + (usize)y * w * 3;
            for (u32 x = 0; x < w; x++) { o2[x*3]=s[x*3]; o2[x*3+1]=s[x*3+1]; o2[x*3+2]=s[x*3+2]; }
        }
        heap_free(raw);

        out->width=(int)w; out->height=(int)h; out->channels=3; out->has_alpha=0;
        out->pixels=px; out->size=out_len; out->stride=w*3;
        return IMG_OK;
    }

    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) return IMG_ERR_UNSUPPORTED;
    if (nplanes != 1) return IMG_ERR_UNSUPPORTED;

    /* Palette lives in the last 769 bytes: 768 palette + 0x0C marker. */
    u8 pal[256 * 3];
    for (u32 i = 0; i < 256; i++) { pal[i*3+0]=pal[i*3+1]=pal[i*3+2]=(u8)i; }
    if (n >= 769 && d[n - 769] == 0x0C) {
        const u8 *pp = d + n - 768;
        for (u32 i = 0; i < 256; i++) { pal[i*3]=pp[i*3]; pal[i*3+1]=pp[i*3+1]; pal[i*3+2]=pp[i*3+2]; }
    }

    usize stride = (w + 7) / 8;
    usize need = (stride + 1) * h;
    if (need > n) return IMG_ERR_TRUNCATED;

    u8 *raw = (u8 *)heap_malloc(stride * h);
    if (!raw) return IMG_ERR_MEMORY;
    usize p = 128, o = 0;
    while (o < stride * h) {
        if (p >= n) { heap_free(raw); return IMG_ERR_TRUNCATED; }
        u8 c = d[p++];
        if (c & 0xC0) {
            usize cnt = c & 0x3F;
            if (p >= n) { heap_free(raw); return IMG_ERR_TRUNCATED; }
            u8 v = d[p++];
            while (cnt-- && o < stride * h) raw[o++] = v;
        } else {
            raw[o++] = c;
        }
    }

    usize out_len = (usize)w * h * 3;
    u8 *px = (u8 *)heap_malloc(out_len);
    if (!px) { heap_free(raw); return IMG_ERR_MEMORY; }
    for (u32 y = 0; y < h; y++) {
        const u8 *s = raw + (usize)y * stride;
        u8 *o2 = px + (usize)y * w * 3;
        for (u32 x = 0; x < w; x++) {
            u32 idx;
            switch (bpp) {
            case 1: idx = (s[x >> 3] >> (7 - (x & 7))) & 1; break;
            case 2: idx = (s[x >> 2] >> (6 - ((x & 3) * 2))) & 3; break;
            case 4: idx = (s[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF; break;
            default: idx = s[x]; break;
            }
            o2[x*3+0] = pal[idx*3]; o2[x*3+1] = pal[idx*3+1]; o2[x*3+2] = pal[idx*3+2];
        }
    }
    heap_free(raw);

    out->width=(int)w; out->height=(int)h; out->channels=3; out->has_alpha=0;
    out->pixels=px; out->size=out_len; out->stride=w*3;
    (void)version;
    return IMG_OK;
}

/* ================================================================
 *  QOI  (Quite OK Image format)
 *
 *  Chosen because it is tiny to implement and gives real compression:
 *  a QOI file is 1-4 bytes per pixel on typical photographic content.
 *
 *  Note the header is big-endian, unlike every other format here.
 * ================================================================ */

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF  0x40
#define QOI_OP_LUMA  0x80
#define QOI_OP_RUN   0xC0
#define QOI_OP_RGB   0xFE
#define QOI_OP_RGBA  0xFF
#define QOI_MASK     0x3F

i64 qoi_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 14) return IMG_ERR_TRUNCATED;
    if (d[0] != 'q' || d[1] != 'o' || d[2] != 'i' || d[3] != 'f') return IMG_ERR_FORMAT;

    u32 w = be32(d + 4), h = be32(d + 8);
    u8  channels = d[12];
    if (w == 0 || h == 0) return IMG_ERR_CORRUPT;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
    if ((u64)w * h > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;
    if (channels != 3 && channels != 4) return IMG_ERR_FORMAT;

    usize out_len = (usize)w * h * 4;   /* always expand QOI to RGBA */
    u8 *px = (u8 *)heap_malloc(out_len);
    if (!px) return IMG_ERR_MEMORY;

    u8 index[64][4];
    memset(index, 0, sizeof index);
    u8 px_prev[4] = { 0, 0, 0, 255 };
    u8 cur[4] = { 0, 0, 0, 255 };
    int run = 0;
    usize p = 14;
    u32 count = 0;
    u32 total = w * h;

    while (count < total) {
        if (run > 0) { run--; }
        else {
            if (p >= n) { heap_free(px); return IMG_ERR_TRUNCATED; }
            u8 b1 = d[p++];
            if (b1 == QOI_OP_RGB) {
                if (p + 3 > n) { heap_free(px); return IMG_ERR_TRUNCATED; }
                cur[0] = d[p]; cur[1] = d[p+1]; cur[2] = d[p+2]; p += 3;
            } else if (b1 == QOI_OP_RGBA) {
                if (p + 4 > n) { heap_free(px); return IMG_ERR_TRUNCATED; }
                cur[0] = d[p]; cur[1] = d[p+1]; cur[2] = d[p+2]; cur[3] = d[p+3]; p += 4;
            } else if ((b1 & 0xC0) == QOI_OP_INDEX) {
                memcpy(cur, index[b1 & QOI_MASK], 4);
            } else if ((b1 & 0xC0) == QOI_OP_DIFF) {
                cur[0] = (u8)(cur[0] + ((b1 >> 4) & 0x03) - 2);
                cur[1] = (u8)(cur[1] + ((b1 >> 2) & 0x03) - 2);
                cur[2] = (u8)(cur[2] + ( b1       & 0x03) - 2);
            } else if ((b1 & 0xC0) == QOI_OP_LUMA) {
                if (p >= n) { heap_free(px); return IMG_ERR_TRUNCATED; }
                u8 b2 = d[p++];
                int vg = (b1 & 0x3F) - 32;
                cur[0] = (u8)(cur[0] + vg - 8 + ((b2 >> 4) & 0x0F));
                cur[1] = (u8)(cur[1] + vg);
                cur[2] = (u8)(cur[2] + vg - 8 + ( b2       & 0x0F));
            } else {  /* QOI_OP_RUN */
                run = (b1 & 0x3F);
            }

            /* Update the 64-entry hash table. */
            int hash = (cur[0] * 3 + cur[1] * 5 + cur[2] * 7 + cur[3] * 11) % 64;
            memcpy(index[hash], cur, 4);
        }

        memcpy(px + (usize)count * 4, cur, 4);
        count++;
        if (memcmp(cur, px_prev, 4) == 0) continue;
        memcpy(px_prev, cur, 4);
    }

    out->width=(int)w; out->height=(int)h; out->channels=4; out->has_alpha=1;
    out->pixels=px; out->size=out_len; out->stride=w*4;
    return IMG_OK;
}

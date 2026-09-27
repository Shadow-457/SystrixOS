/* ================================================================
 *  Systrix OS — kernel/jpeg.c
 *
 *  Baseline sequential JPEG decoder (ITU T.81 / ISO/IEC 10918-1).
 *
 *  Scope: baseline DCT, Huffman entropy coding, 1 and 3 components,
 *  arbitrary sampling factors (so 4:4:4, 4:2:2 and 4:2:0 chroma), and
 *  the usual YCbCr -> RGB conversion.  Restart markers (DRI/RSTn) are
 *  handled, which is what MJPEG streams rely on for error resilience.
 *
 *  Progressive JPEG and arithmetic coding are *not* supported and are
 *  reported as such rather than producing garbage.  Progressive files
 *  are a minority of what cameras and web servers emit, and every
 *  encoder has a baseline mode.
 *
 *  The IDCT is the separable 8-point AAN-style integer transform; it
 *  is fast enough for the frame rates the software renderer can push
 *  and, unlike a float AAN implementation, needs no FPU state.
 * ================================================================ */
#include "image.h"

/* ── Bit reader (MSB first, with 0xFF00 byte stuffing) ───────── */

typedef struct {
    const u8 *d;
    usize n, p;
    u32 buf;
    int cnt;
    int marker_hit;      /* set when a marker was reached */
} jbits;

static void jb_init(jbits *b, const u8 *d, usize n, usize off)
{
    b->d = d; b->n = n; b->p = off; b->buf = 0; b->cnt = 0; b->marker_hit = 0;
}

/* Return the next bit, or -1 at end of stream. */
static int jb_bit(jbits *b)
{
    if (b->cnt == 0) {
        if (b->p >= b->n) { b->marker_hit = 1; return -1; }
        u8 c = b->d[b->p++];
        if (c == 0xFF) {
            u8 c2 = (b->p < b->n) ? b->d[b->p] : 0xD9;
            if (c2 == 0x00) b->p++;                 /* stuffed byte    */
            else { b->marker_hit = 1; return -1; }   /* real marker     */
        }
        b->buf = c; b->cnt = 8;
    }
    b->cnt--;
    return (int)((b->buf >> b->cnt) & 1);
}

static int jb_bits(jbits *b, int k)
{
    int v = 0;
    for (int i = 0; i < k; i++) {
        int x = jb_bit(b);
        if (x < 0) return -1;
        v = (v << 1) | x;
    }
    return v;
}

/* ── Huffman tables ──────────────────────────────────────────── */

#define JHUFF_MAX 256

typedef struct {
    u8 bits[17];                 /* number of codes of each length  */
    u8 vals[JHUFF_MAX];
    int mincode[17], maxcode[18];
    int valptr[17];
    int present;
} jhuff;

static void jh_build(jhuff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k  += h->bits[l];
        h->maxcode[l] = code - 1;
        if (h->bits[l] == 0) h->maxcode[l] = -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

static int jh_decode(jbits *b, jhuff *h)
{
    int code = jb_bit(b);
    if (code < 0) return -1;
    int l = 1;
    while (l <= 16) {
        if (h->bits[l] && code <= h->maxcode[l]) {
            int idx = h->valptr[l] + code - h->mincode[l];
            if (idx < 0 || idx >= JHUFF_MAX) return -1;
            return h->vals[idx];
        }
        int nx = jb_bit(b);
        if (nx < 0) return -1;
        code = (code << 1) | nx;
        l++;
    }
    return -1;
}

/* ── Quantisation tables ─────────────────────────────────────── */

static u16 jquant[4][64];
static int jquant_present[4];

/* ── Components ──────────────────────────────────────────────── */

#define JMAX_COMP 4

typedef struct {
    int id, hs, vs, tq;          /* sampling factors, quant table  */
    int td, ta;                  /* DC and AC table selectors       */
    int dcpred;
    /* Per-component plane at its own resolution. */
    u8  *plane;
    int  pw, ph;                 /* padded plane size (MCU blocks)  */
    int  bw, bh;                 /* blocks across / down            */
} jcomp;

/* ── Zigzag order ────────────────────────────────────────────── */

static const u8 jzigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

/* ── IDCT ──────────────────────────────────────────────────────
 *
 * Separable 8-point inverse DCT in fixed point.
 *
 *   Cx[x][u] = R * cos((2x+1) u pi / 16)          R = 4096 = 2^12
 *   Cf(u)    = R * C(u)                           C(0) = 1/sqrt(2)
 *
 * Row pass, for each row v:
 *     T[v][x] = SUM_u Cf(u) * F[v][u] * Cx[x][u]     =  R^2 * G(x,v)
 * where G is the unnormalised 1-D row transform.  The row result is
 * divided by R straight away, because carrying R^2 into the column
 * pass would overflow 64 bits.
 *
 * Column pass:
 *     S[y] = SUM_v Cf(v) * (T/R) * Cx[y][v]          =  R^3 * H(y)
 * where H is the unnormalised 2-D sum; the IDCT is H/4.
 *
 * Hence: shift right by 12 between the passes, then by 3*12 + 2 = 38
 * at the end (three powers of R from the passes, plus the 1/4).
 *
 * Both Cf terms must be applied at *every* frequency.  Omitting the
 * u>0 / v>0 ones scales them 4096x wrong relative to the DC term,
 * which turns a detailed image into flat, over-bright blocks.
 *
 * The passes are written out rather than using a butterfly network:
 * slightly more multiplies, but checkable, and correctness here is
 * worth more than the last few percent of decode speed.
 */
#define IDCT_ONE       4096
#define IDCT_C0        2896      /* round(4096 / sqrt(2)) */
#define IDCT_CN        4096      /* C(u) for u > 0        */
#define IDCT_ROW_SHIFT 12
#define IDCT_FIN_SHIFT 38        /* 3*12 (pass scales) + 2 (the 1/4) */
#define IDCT_ROUND     (1L << (IDCT_FIN_SHIFT - 1))

static const int idct_cos[8][8] = {
    {  4096,  4017,  3784,  3406,  2896,  2276,  1567,   799 },
    {  4096,  3406,  1567,  -799, -2896, -4017, -3784, -2276 },
    {  4096,  2276, -1567, -4017, -2896,   799,  3784,  3406 },
    {  4096,   799, -3784, -2276,  2896,  3406, -1567, -4017 },
    {  4096,  -799, -3784,  2276,  2896, -3406, -1567,  4017 },
    {  4096, -2276, -1567,  4017, -2896,  -799,  3784, -3406 },
    {  4096, -3406,  1567,   799, -2896,  4017, -3784,  2276 },
    {  4096, -4017,  3784, -3406,  2896, -2276,  1567,  -799 },
};

static void jidct8x8(const int *in, u8 *out, int stride)
{
    /* 64-bit: the column accumulator peaks near 9e15. */
    long tmp[64];

    /* Row pass. */
    for (int v = 0; v < 8; v++) {
        const int *row = in + v * 8;
        if (!row[1] && !row[2] && !row[3] && !row[4] &&
            !row[5] && !row[6] && !row[7]) {
            /* DC-only row: G = C(0)*F[0], so (R^2 * G)/R = F[0]*C0. */
            long base = (long)row[0] * IDCT_C0;
            for (int x = 0; x < 8; x++) tmp[v * 8 + x] = base;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            long acc = (long)row[0] * IDCT_C0 * idct_cos[x][0];
            for (int u = 1; u < 8; u++)
                acc += (long)row[u] * IDCT_CN * idct_cos[x][u];
            tmp[v * 8 + x] = acc >> IDCT_ROW_SHIFT;
        }
    }

    /* Column pass, with the 1/4 and the level shift folded in. */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            long acc = tmp[x] * IDCT_C0 * idct_cos[y][0];
            for (int v = 1; v < 8; v++)
                acc += tmp[v * 8 + x] * IDCT_CN * idct_cos[y][v];
            int val = (int)((acc + IDCT_ROUND) >> IDCT_FIN_SHIFT) + 128;
            if (val < 0)   val = 0;
            if (val > 255) val = 255;
            out[y * stride + x] = (u8)val;
        }
    }
}

/* ── Sign-extend a `nbits`-wide Huffman magnitude ────────────── */

static inline int extend(int v, int nbits)
{
    return (v < (1 << (nbits - 1))) ? v - (1 << nbits) + 1 : v;
}

/* ── Decoder ─────────────────────────────────────────────────── */

i64 jpeg_decode(const u8 *d, usize n, image_t *out)
{
    if (n < 4) return IMG_ERR_TRUNCATED;
    if (!(d[0] == 0xFF && d[1] == 0xD8)) return IMG_ERR_FORMAT;

    jhuff hdc[4], hac[4];
    for (int i = 0; i < 4; i++) { hdc[i].present = 0; hac[i].present = 0; }
    for (int i = 0; i < 4; i++) jquant_present[i] = 0;

    jcomp comp[JMAX_COMP];
    int ncomp = 0;
    int W = 0, H = 0;
    int hmax = 1, vmax = 1;
    int restart_interval = 0;
    int progressive = 0;
    int adobe_transform_present = 0;
    int adobe_transform = -1;

    usize p = 2;
    int have_frame = 0;

    while (p + 1 < n) {
        if (d[p] != 0xFF) { p++; continue; }
        while (p < n && d[p] == 0xFF) p++;
        if (p >= n) break;
        u8 m = d[p++];

        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;                       /* EOI */
        if (p + 2 > n) return IMG_ERR_TRUNCATED;

        u16 seglen = (u16)((d[p] << 8) | d[p+1]);
        if (seglen < 2 || p + seglen > n) return IMG_ERR_TRUNCATED;
        const u8 *seg = d + p + 2;
        usize seglen2 = (usize)seglen - 2;

        switch (m) {
        case 0xC0:   /* SOF0  baseline */
        case 0xC1:   /* SOF1  extended sequential */
        {
            if (seglen2 < 6) return IMG_ERR_CORRUPT;
            if (seg[0] != 8) return IMG_ERR_UNSUPPORTED;   /* only 8-bit */
            H = (seg[1] << 8) | seg[2];
            W = (seg[3] << 8) | seg[4];
            ncomp = seg[5];
            if (ncomp < 1 || ncomp > JMAX_COMP) return IMG_ERR_CORRUPT;
            if (W == 0 || H == 0) return IMG_ERR_CORRUPT;
            if (W > IMG_MAX_DIM || H > IMG_MAX_DIM) return IMG_ERR_TOOBIG;
            if ((u64)W * H > IMG_MAX_PIXELS) return IMG_ERR_TOOBIG;
            if (seglen2 < 6 + (usize)ncomp * 3) return IMG_ERR_CORRUPT;
            for (int i = 0; i < ncomp; i++) {
                comp[i].id  = seg[6 + i*3];
                comp[i].hs  = seg[6 + i*3 + 1] >> 4;
                comp[i].vs  = seg[6 + i*3 + 1] & 0xF;
                comp[i].tq  = seg[6 + i*3 + 2];
                if (comp[i].hs < 1 || comp[i].hs > 4 ||
                    comp[i].vs < 1 || comp[i].vs > 4) return IMG_ERR_CORRUPT;
                if (comp[i].tq > 3) return IMG_ERR_CORRUPT;
                if (comp[i].hs > hmax) hmax = comp[i].hs;
                if (comp[i].vs > vmax) vmax = comp[i].vs;
            }
            have_frame = 1;
            break;
        }
        case 0xC2:
            progressive = 1;
            return IMG_ERR_UNSUPPORTED;

        case 0xC4:   /* DHT */
        {
            usize q = 0;
            while (q + 17 <= seglen2) {
                u8 tc = seg[q] >> 4, th = seg[q] & 0xF;
                if (tc > 1 || th > 3) return IMG_ERR_CORRUPT;
                jhuff *h = tc ? &hac[th] : &hdc[th];
                int total = 0;
                h->bits[0] = 0;
                for (int i = 1; i <= 16; i++) {
                    h->bits[i] = seg[q + i];
                    total += h->bits[i];
                }
                if (total > 256 || q + 17 + (usize)total > seglen2) return IMG_ERR_CORRUPT;
                memcpy(h->vals, seg + q + 17, (usize)total);
                jh_build(h);
                h->present = 1;
                q += 17 + total;
            }
            break;
        }
        case 0xDB:   /* DQT */
        {
            usize q = 0;
            while (q < seglen2) {
                u8 pq = seg[q] >> 4, tq = seg[q] & 0xF;
                q++;
                if (tq > 3) return IMG_ERR_CORRUPT;
                if (pq == 0) {
                    if (q + 64 > seglen2) return IMG_ERR_CORRUPT;
                    for (int i = 0; i < 64; i++) jquant[tq][i] = seg[q + i];
                    q += 64;
                } else {
                    if (q + 128 > seglen2) return IMG_ERR_CORRUPT;
                    for (int i = 0; i < 64; i++)
                        jquant[tq][i] = (u16)((seg[q + i*2] << 8) | seg[q + i*2 + 1]);
                    q += 128;
                }
                jquant_present[tq] = 1;
            }
            break;
        }
        case 0xDD:   /* DRI */
            if (seglen2 >= 2) restart_interval = (seg[0] << 8) | seg[1];
            break;

        case 0xEE:   /* APP14 Adobe */
            if (seglen2 >= 11 && memcmp(seg, "Adobe", 5) == 0) {
                adobe_transform_present = 1;
                adobe_transform = seg[seglen2 - 1];
            }
            break;

        case 0xDA:   /* SOS */
        {
            if (!have_frame) return IMG_ERR_CORRUPT;
            if (seglen2 < 1) return IMG_ERR_CORRUPT;
            int ns = seg[0];
            if (ns != ncomp) return IMG_ERR_CORRUPT;   /* interleaved only */
            if (seglen2 < 1 + (usize)ns * 2 + 3) return IMG_ERR_CORRUPT;
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + i*2];
                int t   = seg[2 + i*2];
                for (int c = 0; c < ncomp; c++) {
                    if (comp[c].id == cid) {
                        comp[c].td = t >> 4;
                        comp[c].ta = t & 0xF;
                        if (comp[c].td > 3 || comp[c].ta > 3) return IMG_ERR_CORRUPT;
                    }
                }
            }

            /* --- Set up MCU geometry and component planes --- */
            int mcux = (W  + 8 * hmax - 1) / (8 * hmax);
            int mcuy = (H  + 8 * vmax - 1) / (8 * vmax);
            for (int c = 0; c < ncomp; c++) {
                comp[c].bw = mcux * comp[c].hs;
                comp[c].bh = mcuy * comp[c].vs;
                comp[c].pw = comp[c].bw * 8;
                comp[c].ph = comp[c].bh * 8;
                usize sz = (usize)comp[c].pw * comp[c].ph;
                comp[c].plane = (u8 *)heap_malloc(sz);
                if (!comp[c].plane) {
                    for (int k = 0; k < c; k++) heap_free(comp[k].plane);
                    return IMG_ERR_MEMORY;
                }
                memset(comp[c].plane, 0x80, sz);
                comp[c].dcpred = 0;
            }

            /* --- Entropy-coded data --- */
            jbits b;
            jb_init(&b, d, n, p + seglen);
            int coef[64];
            int mcu_count = 0;
            int since_restart = 0;

            for (int my = 0; my < mcuy; my++) {
                for (int mx = 0; mx < mcux; mx++) {
                    if (restart_interval && since_restart == restart_interval) {
                        /* Skip to the RSTn marker and resync. */
                        b.marker_hit = 1;
                        while (b.p + 1 < n) {
                            if (d[b.p] == 0xFF && d[b.p+1] >= 0xD0 && d[b.p+1] <= 0xD7) {
                                b.p += 2; break;
                            }
                            b.p++;
                        }
                        for (int c = 0; c < ncomp; c++) comp[c].dcpred = 0;
                        b.buf = 0; b.cnt = 0;
                        since_restart = 0;
                    }

                    for (int c = 0; c < ncomp; c++) {
                        jcomp *cp = &comp[c];
                        if (!hdc[cp->td].present || !hac[cp->ta].present) return IMG_ERR_CORRUPT;
                        for (int by = 0; by < cp->vs; by++) {
                            for (int bx = 0; bx < cp->hs; bx++) {
                                for (int k = 0; k < 64; k++) coef[k] = 0;

                                /* DC: difference, category-coded */
                                int t = jh_decode(&b, &hdc[cp->td]);
                                if (t < 0) goto entropy_done;
                                int diff = 0;
                                if (t) {
                                    int v = jb_bits(&b, t);
                                    if (v < 0) goto entropy_done;
                                    diff = extend(v, t);
                                }
                                cp->dcpred += diff;
                                coef[0] = cp->dcpred * jquant[cp->tq][0];

                                /* AC: run-length of zeroes then magnitude */
                                for (int k = 1; k < 64; ) {
                                    int rs = jh_decode(&b, &hac[cp->ta]);
                                    if (rs < 0) goto entropy_done;
                                    int r = rs >> 4, s = rs & 0xF;
                                    if (s == 0) {
                                        if (r != 15) break;   /* EOB */
                                        k += 16;              /* ZRL */
                                        continue;
                                    }
                                    k += r;
                                    if (k > 63) break;
                                    int v = jb_bits(&b, s);
                                    if (v < 0) goto entropy_done;
                                    int zz = jzigzag[k];
                                    /* The quantisation table is stored
                                     * in zigzag order, so index it with
                                     * the zigzag position k, not the
                                     * natural-order index zz. */
                                    coef[zz] = extend(v, s) * jquant[cp->tq][k];
                                    k++;
                                }

                                int ox = (mx * cp->hs + bx) * 8;
                                int oy = (my * cp->vs + by) * 8;
                                jidct8x8(coef, cp->plane + (usize)oy * cp->pw + ox, cp->pw);
                            }
                        }
                    }
                    since_restart++;
                    mcu_count++;
                }
            }
        entropy_done:
            (void)mcu_count;

            /* --- Colour conversion --- */
            int out_ch = (ncomp >= 3 && adobe_transform_present && adobe_transform == 0) ? 1 : 3;
            if (ncomp == 3) out_ch = 3;
            else if (ncomp == 1) out_ch = 1;

            usize out_len = (usize)W * H * (usize)out_ch;
            u8 *px = (u8 *)heap_malloc(out_len);
            if (!px) {
                for (int c = 0; c < ncomp; c++) heap_free(comp[c].plane);
                return IMG_ERR_MEMORY;
            }

            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    u8 *o = px + ((usize)y * W + x) * out_ch;
                    if (ncomp == 1) {
                        const jcomp *c0 = &comp[0];
                        int sx = x * c0->hs / hmax, sy = y * c0->vs / vmax;
                        o[0] = c0->plane[(usize)sy * c0->pw + sx];
                    } else if (ncomp == 3) {
                        const jcomp *cy = &comp[0], *cb = &comp[1], *cr = &comp[2];
                        int yx = x * cy->hs / hmax, yy = y * cy->vs / vmax;
                        int bx = x * cb->hs / hmax, by = y * cb->vs / vmax;
                        int rx = x * cr->hs / hmax, ry = y * cr->vs / vmax;
                        int Y  = cy->plane[(usize)yy * cy->pw + yx];
                        int Cb = cb->plane[(usize)by * cb->pw + bx] - 128;
                        int Cr = cr->plane[(usize)ry * cr->pw + rx] - 128;
                        int r = Y + ((91881 * Cr) >> 16);
                        int g = Y - ((22554 * Cb + 46802 * Cr) >> 16);
                        int b = Y + ((116130 * Cb) >> 16);
                        if (r < 0) r = 0;
                        if (r > 255) r = 255;
                        if (g < 0) g = 0;
                        if (g > 255) g = 255;
                        if (b < 0) b = 0;
                        if (b > 255) b = 255;
                        o[0] = (u8)r; o[1] = (u8)g; o[2] = (u8)b;
                    } else {
                        const jcomp *cy = &comp[0];
                        int yx = x * cy->hs / hmax, yy = y * cy->vs / vmax;
                        o[0] = o[1] = o[2] = cy->plane[(usize)yy * cy->pw + yx];
                    }
                }
            }

            for (int c = 0; c < ncomp; c++) heap_free(comp[c].plane);

            if (out_ch == 1) {
                /* Grey stored as 1 channel: the viewer expands it. */
                out->channels = 1; out->has_alpha = 0;
            } else {
                out->channels = 3; out->has_alpha = 0;
            }
            out->width = W; out->height = H;
            out->pixels = px; out->size = out_len;
            out->stride = (u32)(W * out_ch);
            return IMG_OK;
        }
        default:
            break;   /* APPn, COM, DNL, ... — skip */
        }
        p += seglen;
    }

    (void)progressive;
    return IMG_ERR_TRUNCATED;
}

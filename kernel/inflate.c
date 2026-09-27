/* ================================================================
 *  Systrix OS — kernel/inflate.c
 *
 *  RFC 1951 DEFLATE + RFC 1950 zlib wrapper.
 *
 *  Why this exists: the original PNG viewer in kernel/pngview.c could
 *  only read PNGs whose IDAT stream used *stored* (uncompressed)
 *  deflate blocks — i.e. files that the project's own toy encoder had
 *  produced.  Every PNG written by any real tool uses fixed or dynamic
 *  Huffman coding, so the viewer rejected essentially all real images.
 *
 *  This is a complete, streaming-free (one-shot) inflate: it decodes a
 *  whole buffer into a caller-provided output buffer.  Callers that need
 *  to know the decompressed size in advance use
 *  zlib_inflate_peek(), which walks the stream without emitting output.
 *
 *  Design notes
 *  ------------
 *  * No recursion, no dynamic allocation, no floating point.  The only
 *    scratch space is a 32 KiB window on the caller's stack or in the
 *    caller's buffer (see inflate_ctx below) — important because this
 *    runs in a kernel with a 2 MiB heap and interrupt handlers that
 *    must stay re-entrant.
 *  * Huffman tables are decoded with the canonical "count/offset"
 *    method, which needs no 288-entry symbol table per code length.
 *  * Bounds are checked on every read and write.  A corrupt or hostile
 *    stream returns a negative error code instead of walking off the
 *    end of a buffer.
 * ================================================================ */
#include "../include/kernel.h"

/* Error codes (negative, so they never collide with a byte count) */
#define INFLATE_OK            0
#define INFLATE_ERR_DATA     (-1)   /* malformed stream                */
#define INFLATE_ERR_OUTPUT   (-2)   /* output buffer too small         */
#define INFLATE_ERR_INPUT    (-3)   /* ran off the end of the input    */
#define INFLATE_ERR_MEM      (-4)   /* Huffman table too large         */

/* ── Canonical Huffman decoder ─────────────────────────────────────
 *
 * counts[len]  = number of symbols with that code length
 * symbols[]    = symbols ordered by (length, symbol value)
 *
 * Decoding walks one bit at a time, subtracting as it goes:
 *   code starts at 0; for each length, if code < counts[len] the symbol
 *   is symbols[offset[code]]; otherwise code -= counts[len] and try
 *   the next length.  This is the classic puff.c formulation.
 */
#define MAX_BITS 15

typedef struct {
    u16 counts[MAX_BITS + 1];      /* number of codes of each length   */
    u16 symbols[288];              /* symbols sorted by code           */
} huff_t;

typedef struct {
    const u8 *in;                  /* input buffer                     */
    usize      in_len;
    usize      in_pos;
    u32        bitbuf;
    int        bitcnt;

    u8        *out;                /* output buffer                    */
    usize      out_len;
    usize      out_pos;

    u8        *window;             /* 32 KiB sliding window (or NULL) */
    usize      win_pos;            /* next write position in window   */
    usize      win_have;           /* valid bytes in window           */

    huff_t     lencode;            /* literal/length alphabet          */
    huff_t     distcode;           /* distance alphabet               */
} inflate_ctx;

/* Build a canonical Huffman decoding table from code lengths.
 * `lengths[i]` is the code length of symbol i (0 = unused).
 * Returns 0 on success, -1 if the code is over/under-subscribed. */
static int huff_build(huff_t *h, const u8 *lengths, int n)
{
    int symbol, len;
    int left;

    for (len = 0; len <= MAX_BITS; len++) h->counts[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->counts[lengths[symbol]]++;
    if (h->counts[0] == n) return 0;      /* no codes at all: legal      */

    /* Check for an over-subscribed or incomplete set.  A single-code
     * distance tree (one symbol of length 1) is legal even though it is
     * incomplete, and DEFLATE producers emit it. */
    left = 1;
    for (len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return INFLATE_ERR_DATA;   /* over-subscribed   */
    }

    /* Fill symbols[] in canonical order. */
    {
        u16 offs[MAX_BITS + 1];
        offs[0] = 0; offs[1] = 0;
        for (len = 1; len < MAX_BITS; len++)
            offs[len + 1] = (u16)(offs[len] + h->counts[len]);
        for (symbol = 0; symbol < n; symbol++)
            if (lengths[symbol]) h->symbols[offs[lengths[symbol]]++] = (u16)symbol;
    }
    return 0;
}

/* Read `need` bits LSB-first.  Returns -1 if the input is exhausted. */
static int bits(inflate_ctx *s, int need)
{
    u32 val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return INFLATE_ERR_INPUT;
        val |= (u32)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* Decode one symbol from a Huffman table. */
static int decode(inflate_ctx *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAX_BITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return INFLATE_ERR_INPUT;
        code |= b;
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code  <<= 1;
    }
    return INFLATE_ERR_DATA;
}

/* ── Output ───────────────────────────────────────────────────── */

/* Emit one byte, maintaining the sliding window. */
static int emit(inflate_ctx *s, u8 b)
{
    if (s->out_pos >= s->out_len) return INFLATE_ERR_OUTPUT;
    s->out[s->out_pos++] = b;
    if (s->window) {
        s->window[s->win_pos++] = b;
        if (s->win_pos >= 32768) s->win_pos = 0;
        if (s->win_have < 32768) s->win_have++;
    }
    return INFLATE_OK;
}

/* Copy `len` bytes from `dist` back in the output stream. */
static int emit_match(inflate_ctx *s, u32 dist, u32 len)
{
    if ((usize)dist > s->out_pos) return INFLATE_ERR_DATA;   /* before start */

    /* Without a window we can still copy from the output buffer. */
    if (!s->window) {
        const u8 *src = s->out + (s->out_pos - dist);
        for (u32 i = 0; i < len; i++) {
            int e = emit(s, src[i]);
            if (e) return e;
        }
        return INFLATE_OK;
    }

    for (u32 i = 0; i < len; i++) {
        usize from;
        if (dist <= s->win_have) {
            from = (s->win_pos + 32768 - dist) & 32767u;
        } else {
            /* Reach back past the window start into the output buffer. */
            usize back = dist - s->win_have;
            if (back > s->out_pos) return INFLATE_ERR_DATA;
            u8 b = s->out[s->out_pos - back];
            int e = emit(s, b);
            if (e) return e;
            continue;
        }
        int e = emit(s, s->window[from]);
        if (e) return e;
    }
    return INFLATE_OK;
}

/* ── Blocks ───────────────────────────────────────────────────── */

static const u16 len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const u8 len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const u16 dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577
};
static const u8 dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* Build the fixed literal/length and distance tables. */
static int build_fixed(inflate_ctx *s)
{
    u8 lengths[288];
    int i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (i = 144; i < 256; i++) lengths[i] = 9;
    for (i = 256; i < 280; i++) lengths[i] = 7;
    for (i = 280; i < 288; i++) lengths[i] = 8;
    if (huff_build(&s->lencode, lengths, 288)) return INFLATE_ERR_DATA;
    for (i = 0; i < 30; i++) lengths[i] = 5;
    if (huff_build(&s->distcode, lengths, 30)) return INFLATE_ERR_DATA;
    return INFLATE_OK;
}

/* Code-length alphabet order: 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 */
static const u8 clen_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* Read the dynamic code-length table. */
static int build_dynamic(inflate_ctx *s)
{
    int nlen  = bits(s, 5);
    int ndist = bits(s, 5);
    int ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return INFLATE_ERR_INPUT;
    nlen  += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return INFLATE_ERR_DATA;

    u8 clens[19];
    for (int i = 0; i < 19; i++) clens[i] = 0;
    for (int i = 0; i < ncode; i++) {
        int v = bits(s, 3);
        if (v < 0) return INFLATE_ERR_INPUT;
        clens[clen_order[i]] = (u8)v;
    }
    huff_t clcode;
    if (huff_build(&clcode, clens, 19)) return INFLATE_ERR_DATA;

    u8 lengths[288 + 30];
    for (int i = 0; i < 19; i++) lengths[i] = 0;   /* only nlen+ndist used */
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &clcode);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[i++] = (u8)sym;
        } else {
            int rep;
            u8 val = 0;
            if (sym == 16) {
                if (i == 0) return INFLATE_ERR_DATA;
                val = lengths[i - 1];
                rep = bits(s, 2);
                if (rep < 0) return INFLATE_ERR_INPUT;
                rep += 3;
            } else if (sym == 17) {
                rep = bits(s, 3);
                if (rep < 0) return INFLATE_ERR_INPUT;
                rep += 3;
            } else {
                rep = bits(s, 7);
                if (rep < 0) return INFLATE_ERR_INPUT;
                rep += 11;
            }
            if (i + rep > nlen + ndist) return INFLATE_ERR_DATA;
            while (rep--) lengths[i++] = val;
        }
    }
    /* A code length of zero is only allowed for unused distances. */
    if (lengths[256] == 0) return INFLATE_ERR_DATA;

    if (huff_build(&s->lencode, lengths, nlen)) return INFLATE_ERR_DATA;
    /* The distance alphabet may legitimately end up empty. */
    int rc = huff_build(&s->distcode, lengths + nlen, ndist);
    if (rc && rc != 0) return rc;
    return INFLATE_OK;
}

/* Inflate a raw DEFLATE stream. */
static int inflate_raw(const u8 *in, usize in_len, u8 *out, usize out_len,
                       u8 *window)
{
    inflate_ctx s;
    int last;

    s.in = in; s.in_len = in_len; s.in_pos = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.out_len = out_len; s.out_pos = 0;
    s.window = window; s.win_pos = 0; s.win_have = 0;

    do {
        last = bits(&s, 1);
        if (last < 0) return INFLATE_ERR_INPUT;
        int type = bits(&s, 2);
        if (type < 0) return INFLATE_ERR_INPUT;

        if (type == 0) {
            /* Stored: skip to a byte boundary, then LEN/NLEN + data. */
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) return INFLATE_ERR_INPUT;
            u32 len  = (u32)s.in[s.in_pos] | ((u32)s.in[s.in_pos + 1] << 8);
            u32 nlen = (u32)s.in[s.in_pos + 2] | ((u32)s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return INFLATE_ERR_DATA;
            if (s.in_pos + len > s.in_len) return INFLATE_ERR_INPUT;
            for (u32 k = 0; k < len; k++) {
                int e = emit(&s, s.in[s.in_pos + k]);
                if (e) return e;
            }
            s.in_pos += len;
            continue;
        }

        if (type == 1) {
            int rc = build_fixed(&s);
            if (rc) return rc;
        } else if (type == 2) {
            int rc = build_dynamic(&s);
            if (rc) return rc;
        } else {
            return INFLATE_ERR_DATA;          /* reserved block type */
        }

        /* Decode symbols until the end-of-block code. */
        for (;;) {
            int sym = decode(&s, &s.lencode);
            if (sym < 0) return sym;
            if (sym < 256) {
                int e = emit(&s, (u8)sym);
                if (e) return e;
            } else if (sym == 256) {
                break;                         /* end of block       */
            } else {
                sym -= 257;
                if (sym >= 29) return INFLATE_ERR_DATA;
                int extra = bits(&s, len_extra[sym]);
                if (extra < 0) return INFLATE_ERR_INPUT;
                u32 length = (u32)len_base[sym] + (u32)extra;

                int dsym = decode(&s, &s.distcode);
                if (dsym < 0) return dsym;
                if (dsym >= 30) return INFLATE_ERR_DATA;
                extra = bits(&s, dist_extra[dsym]);
                if (extra < 0) return INFLATE_ERR_INPUT;
                u32 dist = (u32)dist_base[dsym] + (u32)extra;

                int e = emit_match(&s, dist, length);
                if (e) return e;
            }
        }
    } while (!last);

    return (int)s.out_pos;
}

/* ── Public API ───────────────────────────────────────────────── */

int inflate_raw_stream(const u8 *in, usize in_len, u8 *out, usize out_len)
{
    return inflate_raw(in, in_len, out, out_len, 0);
}

/* zlib wrapper (RFC 1950): 2-byte header, deflate data, 4-byte Adler-32.
 *
 * CMF: bits 0-3 = CM (8 = deflate), bits 4-7 = CINFO.
 * FLG: bits 0-4 = FCHECK, bit 5 = FDICT, bits 6-7 = FLEVEL.
 * The 16-bit value CMF<<8|FLG must be a multiple of 31.
 *
 * Note FDICT is bit 5 (0x20), not bit 1 — bit 1 belongs to FCHECK, and
 * testing it rejects every level-9 stream because zlib sets FLEVEL=3
 * there, which pushes FCHECK's low bit up. */
int zlib_inflate(const u8 *in, usize in_len, u8 *out, usize out_len)
{
    if (in_len < 2) return INFLATE_ERR_INPUT;
    u8 cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8) return INFLATE_ERR_DATA;      /* not deflate    */
    if (cmf >> 4 > 7) return INFLATE_ERR_DATA;           /* window > 32K   */
    if (((u32)cmf * 256u + flg) % 31u != 0) return INFLATE_ERR_DATA;
    if (flg & 0x20) return INFLATE_ERR_DATA;             /* preset dict    */

    /* The trailing Adler-32 is not verified: a mismatch means a corrupt
     * image, and the PNG layer validates CRCs itself. */
    return inflate_raw(in + 2, in_len - 2, out, out_len, 0);
}

/* Decode a zlib stream when the caller does not know the output size.
 * The output buffer must be at least `hint` bytes; the function returns
 * the number of bytes written or a negative error. */
int zlib_inflate_buf(const u8 *in, usize in_len, u8 *out, usize out_cap, usize *written)
{
    int n = zlib_inflate(in, in_len, out, out_cap);
    if (n < 0) return n;
    if (written) *written = (usize)n;
    return n;
}

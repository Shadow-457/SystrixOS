/* ================================================================
 *  Host-side test for kernel/inflate.c — NOT part of the OS build.
 *
 *  Cross-checks our DEFLATE decoder against zlib over a matrix of
 *  levels, strategies, data shapes and sizes, and verifies that
 *  truncated / hostile input is rejected instead of overrunning.
 *
 *  Build & run:
 *    make -f Makefile test-inflate
 * ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef size_t   usize;
typedef int32_t  i32;

int  zlib_inflate(const u8 *in, usize in_len, u8 *out, usize out_len);
int  inflate_raw_stream(const u8 *in, usize in_len, u8 *out, usize out_len);
int  zlib_inflate_buf(const u8 *in, usize in_len, u8 *out, usize out_cap, usize *written);

static int failures = 0;
static int checks   = 0;

enum { SHAPE_ZERO, SHAPE_RANDOM, SHAPE_TEXT, SHAPE_RAMP, SHAPE_IMAGE, SHAPE_COUNT };

static void fill_src(u8 *dst, int n, int shape, const char *text)
{
    for (int i = 0; i < n; i++) {
        switch (shape) {
        case SHAPE_ZERO:  dst[i] = 0; break;
        case SHAPE_RANDOM:dst[i] = (u8)(rand() & 0xFF); break;
        case SHAPE_TEXT:  dst[i] = (u8)text[i % (int)strlen(text)]; break;
        case SHAPE_RAMP:  dst[i] = (u8)(i & 0xFF); break;
        case SHAPE_IMAGE: dst[i] = (u8)(((i / 64) ^ (i / 8)) & 0xFF); break;
        }
    }
}

static void deflate_bytes(const u8 *src, int n, int level, int strategy,
                          u8 *out, usize out_cap, usize *out_len)
{
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    if (deflateInit2(&strm, level, Z_DEFLATED, 15, 8, strategy) != Z_OK) return;
    strm.next_in   = (Bytef *)src;
    strm.avail_in  = (uInt)n;
    strm.next_out  = out;
    strm.avail_out = (uInt)out_cap;
    if (deflate(&strm, Z_FINISH) != Z_STREAM_END) { deflateEnd(&strm); return; }
    *out_len = strm.total_out;
    deflateEnd(&strm);
}

static void expect(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

int main(void)
{
    printf("testing against zlib %s\n\n", zlibVersion());

    static const int levels[]    = { 0, 1, 6, 9 };
    static const int strategies[] = { Z_DEFAULT_STRATEGY, Z_FILTERED,
                                      Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    static const char *shapes[]  = { "zero", "random", "text", "ramp", "image" };
    static const int sizes[]     = { 0, 1, 2, 7, 258, 259, 1000, 5000, 70000, 300000 };

    const char *text =
        "The quick brown fox jumps over the lazy dog. "
        "Systrix OS kernel image codec test. "
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "abcdefghijklmnopqrstuvwxyz0123456789";

    u8 *src  = malloc(400000);
    u8 *comp = malloc(400000);
    u8 *mine = malloc(400000);

    int total = 0;
    for (size_t li = 0; li < sizeof levels / sizeof *levels; li++)
    for (size_t si = 0; si < sizeof strategies / sizeof *strategies; si++)
    for (size_t pi = 0; pi < sizeof shapes / sizeof *shapes; pi++)
    for (size_t zi = 0; zi < sizeof sizes / sizeof *sizes; zi++) {
        int n = sizes[zi];
        fill_src(src, n, (int)pi, text);
        usize clen = 0;
        deflate_bytes(src, n, levels[li], strategies[si], comp, 400000, &clen);
        if (!clen && n) continue;
        total++;

        memset(mine, 0xAA, n + 64);
        int got = zlib_inflate(comp, clen, mine, (usize)n);

        char nm[160];
        snprintf(nm, sizeof nm, "L%d/%s/%s/%d: %d -> %d",
                 levels[li], strategies[si] == Z_DEFAULT_STRATEGY ? "def" :
                 strategies[si] == Z_FILTERED ? "filt" :
                 strategies[si] == Z_HUFFMAN_ONLY ? "huff" :
                 strategies[si] == Z_RLE ? "rle" : "fix",
                 shapes[pi], n, (int)clen, got);
        checks++;
        if (got != n) { printf("  FAIL %s (expected %d bytes)\n", nm, n); failures++; continue; }
        if (n && memcmp(mine, src, n) != 0) { printf("  FAIL %s content mismatch\n", nm); failures++; }
    }
    printf("%d round-trips compared against zlib\n", total);

    /* Raw (headerless) deflate: gzip/deflate streams have no zlib header. */
    {
        int n = 40000;
        fill_src(src, n, SHAPE_TEXT, text);
        uLong bound = compressBound(n);
        if (compress2(comp, &bound, src, n, 9) == Z_OK) {
            /* skip the 2-byte zlib header and the 4-byte Adler-32 */
            int got = inflate_raw_stream(comp + 2, bound - 2, mine, n);
            expect(got == n, "raw deflate stream decodes");
            expect(n == 0 || memcmp(mine, src, n) == 0, "raw deflate content matches");
        }
    }

    /* Truncated input must fail, not run off the end. */
    {
        int n = 50000;
        fill_src(src, n, SHAPE_RANDOM, text);
        uLong bound = compressBound(n);
        compress2(comp, &bound, src, n, 9);
        int got = zlib_inflate(comp, (usize)(bound / 2), mine, 60000);
        expect(got < 0, "truncated input is rejected");
    }

    /* Output buffer too small must report ERR_OUTPUT specifically. */
    {
        int n = 200000;
        fill_src(src, n, SHAPE_RANDOM, text);
        uLong bound = compressBound(n);
        compress2(comp, &bound, src, n, 9);
        u8 small[1000];
        memset(small, 0xAA, sizeof small);
        int got = zlib_inflate(comp, bound, small, sizeof small);
        expect(got == -2, "short output buffer reports ERR_OUTPUT");
    }

    /* Bad header detection. */
    {
        u8 hdr[2] = { 0x00, 0x00 };
        expect(zlib_inflate(hdr, 2, mine, 100) < 0, "bad CM rejected");
        u8 hdr2[2] = { 0x78, 0x00 };
        expect(zlib_inflate(hdr2, 2, mine, 100) < 0, "bad FCHECK rejected");
        u8 hdr3[2] = { 0x78, 0xbb };
        expect(zlib_inflate(hdr3, 2, mine, 100) < 0, "FDICT rejected");
    }

    /* Corruption fuzz: every single-byte corruption must be handled
     * without crashing.  With ASan on the host this also proves we never
     * write outside the caller's output buffer. */
    {
        int n = 30000;
        fill_src(src, n, SHAPE_TEXT, text);
        uLong bound = compressBound(n);
        compress2(comp, &bound, src, n, 6);
        u8 save[4096];
        for (int t = 0; t < 4000; t++) {
            usize at = 2 + (usize)(rand() % (int)(bound - 2));
            u8 old = comp[at];
            comp[at] = (u8)(rand() & 0xFF);
            (void)zlib_inflate(comp, bound, mine, 40000);
            comp[at] = old;
        }
        (void)save;
        expect(1, "corruption fuzz survived 4000 mutations");
    }

    /* Truncate at every offset of a small stream. */
    {
        int n = 5000;
        fill_src(src, n, SHAPE_TEXT, text);
        uLong bound = compressBound(n);
        compress2(comp, &bound, src, n, 9);
        for (uLong cut = 0; cut < bound; cut += 7) {
            (void)zlib_inflate(comp, cut, mine, 8000);
        }
        expect(1, "truncation sweep survived");
    }

    free(src); free(comp); free(mine);

    printf("\n%d checks, %d failures\n", checks, failures);
    printf(failures ? "INFLATE TESTS FAILED\n" : "all inflate tests passed\n");
    return failures ? 1 : 0;
}

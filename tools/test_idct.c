/* ================================================================
 *  Unit test for the JPEG IDCT — NOT part of the OS build.
 *
 *  The IDCT is the single most error-prone part of a JPEG decoder and
 *  a scaling mistake produces a plausible-looking but wrong image, so
 *  it is checked directly against a float reference rather than only
 *  through end-to-end image comparison.
 *
 *  Build: tools/run_image_test.sh compiles this alongside the decoders
 *  and the Python side (tools/check_idct.py) diffs the output.
 * ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t   usize;

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
#define IDCT_ONE       4096
#define IDCT_C0        2896
#define IDCT_CN        4096
#define IDCT_ROW_SHIFT 12
#define IDCT_FIN_SHIFT 38
#define IDCT_ROUND     (1L << (IDCT_FIN_SHIFT - 1))

static void jidct8x8(const int *in, u8 *out, int stride)
{
    long tmp[64];
    for (int v = 0; v < 8; v++) {
        const int *row = in + v * 8;
        if (!row[1] && !row[2] && !row[3] && !row[4] &&
            !row[5] && !row[6] && !row[7]) {
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

int main(void)
{
    unsigned seed = 12345u;
    srand(seed);
    for (int t = 0; t < 8; t++) {
        int in[64];
        u8 out[64];
        for (int i = 0; i < 64; i++) in[i] = (rand() % 401) - 200;
        if (t == 0) { memset(in, 0, sizeof in); in[0] = 1024; }   /* DC only  */
        if (t == 1) { memset(in, 0, sizeof in); in[0] = -800; }   /* DC only, neg */
        if (t == 2) { memset(in, 0, sizeof in);                   /* flat grey  */
                      for (int i = 0; i < 64; i++) in[i] = 0;
                      in[0] = 8; }
        if (t == 3) { memset(in, 0, sizeof in); in[0] = 100; in[1] = 60; in[8] = -40; }
        if (t == 4) { for (int i = 0; i < 64; i++) in[i] = 0;     /* full-scale */
                      for (int i = 0; i < 64; i++) in[i] = (i * 37) % 401 - 200; }
        printf("CASE %d\n", t);
        for (int i = 0; i < 64; i++) printf("%d\n", in[i]);
        jidct8x8(in, out, 8);
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) printf("%d%c", out[r * 8 + c], c == 7 ? '\n' : ' ');
        }
    }
    return 0;
}

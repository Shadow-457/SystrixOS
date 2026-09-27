/* ================================================================
 *  Host-side test for the kernel image decoders — NOT part of the OS.
 *
 *  Decodes every image in tools/testdata/ and writes the result as a
 *  binary PPM so a reference decoder (PIL) can compare pixel for pixel.
 *
 *  Build:  tools/run_image_test.sh <outdir>
 * ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

/* ---- kernel shims ---- */
#include "kernel_shim.h"
void *heap_malloc(usize n) { return malloc(n); }
void  heap_free(void *p)   { free(p); }
i64 vfs_open(const char *p)  { (void)p; return -1; }
i64 vfs_read(u64 fd, void *b, usize n) { (void)fd;(void)b;(void)n; return 0; }
i64 vfs_close(u64 fd) { (void)fd; return 0; }

#include "image.h"

/* ---- decoders under test ---- */
#include "inflate.c"
#include "image.c"
#include "png.c"
#include "jpeg.c"
#include "bmp.c"
#include "tga_pcx_qoi.c"

/* ---- helpers ---- */
static u8 *slurp(const char *path, usize *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (usize)n;
    return b;
}

static int dump_ppm(const char *path, const image_t *im)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", im->width, im->height);
    u32 *rgb = malloc((size_t)im->width * im->height * 4);
    image_to_xrgb32(im, rgb);
    for (int i = 0; i < im->width * im->height; i++) {
        u8 o[3] = { (u8)(rgb[i] >> 16), (u8)(rgb[i] >> 8), (u8)rgb[i] };
        fwrite(o, 1, 3, f);
    }
    free(rgb);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s <testdata-dir> <outdir>\n", argv[0]); return 2; }
    const char *indir = argv[1], *outdir = argv[2];

    char path[1024], out[1024];
    snprintf(out, sizeof out, "%s/report.txt", outdir);
    FILE *rep = fopen(out, "w");
    if (!rep) { printf("cannot write %s\n", out); return 2; }

    int ok = 0, bad = 0;
    DIR *d = opendir(indir);
    if (!d) { printf("cannot open %s\n", indir); return 2; }
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        char ext[16];
        snprintf(ext, sizeof ext, "%s", dot);
        for (char *q = ext; *q; q++) *q = (char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
        if (strcmp(ext, ".png") && strcmp(ext, ".jpg") && strcmp(ext, ".jpeg") &&
            strcmp(ext, ".bmp") && strcmp(ext, ".tga") && strcmp(ext, ".pcx") &&
            strcmp(ext, ".qoi"))
            continue;

        snprintf(path, sizeof path, "%s/%s", indir, e->d_name);
        usize n = 0;
        u8 *buf = slurp(path, &n);
        if (!buf) continue;

        char base[512];
        snprintf(base, sizeof base, "%s", e->d_name);
        char *dot2 = strrchr(base, '.');
        if (dot2) *dot2 = 0;

        image_fmt f = image_probe(buf, n);
        image_t im;
        i64 rc = image_decode(buf, n, &im);

        if (rc == IMG_OK) {
            char op[1024];
            snprintf(op, sizeof op, "%s/%s.ppm", outdir, base);
            dump_ppm(op, &im);
            fprintf(rep, "OK   %-28s %-6s %4dx%-4d ch=%d\n", e->d_name,
                    image_fmt_name(f), im.width, im.height, im.channels);
            ok++;
            image_free(&im);
        } else {
            fprintf(rep, "FAIL %-28s %-6s %s (%lld)\n", e->d_name,
                    image_fmt_name(f), image_error(rc), (long long)rc);
            bad++;
        }
        free(buf);
    }
    closedir(d);
    fclose(rep);
    printf("%d decoded, %d not decoded (see report.txt)\n", ok, bad);
    /* Deliberately exit 0: whether a rejection is correct is decided by
     * tools/compare_images.py, which knows which variants are out of
     * scope. */
    return 0;
}

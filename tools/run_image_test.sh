#!/bin/sh
# Host-side test for the kernel image decoders.
#
#   tools/run_image_test.sh
#
# Generates a corpus of images with PIL (PNG colour types, bit depths,
# interlace, tRNS; JPEG grayscale/4:2:0/4:2:2/4:4:4/progressive; BMP
# 8/24/32-bit and RLE; QOI), runs the kernel decoders over it, then
# compares every decoded frame against PIL pixel for pixel.
set -e
cd "$(dirname "$0")/.."
WORK=${TMPDIR:-/tmp}/systrix-image-test
rm -rf "$WORK"
mkdir -p "$WORK/in" "$WORK/out"

python3 tools/make_test_images.py "$WORK/in"

# The decoders include "image.h", which includes "kernel.h".  Provide a
# minimal stand-in so they can be compiled for the host.
cat > "$WORK/kernel_shim.h" <<'EOF'
/* host stand-in for include/kernel.h (only what the decoders touch) */
#ifndef SYSTRIX_HOST_SHIM_H
#define SYSTRIX_HOST_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t   usize;
void *heap_malloc(usize n);
void  heap_free(void *p);
i64 vfs_open(const char *path);
i64 vfs_read(u64 fd, void *buf, usize n);
i64 vfs_close(u64 fd);
#endif
EOF

mkdir -p "$WORK/kernel"
for f in image.c png.c jpeg.c bmp.c tga_pcx_qoi.c inflate.c; do
    sed "s|#include \"../include/kernel.h\"|#include \"kernel_shim.h\"|" \
        kernel/$f > "$WORK/kernel/$f"
done
sed 's|#include "kernel.h"|#include "kernel_shim.h"|' kernel/image.h > "$WORK/kernel/image.h"

cc -O2 -g -Wall -Wextra -Wno-unused-function -Wno-sign-compare \
   -fsanitize=address,undefined -I"$WORK" -I"$WORK/kernel" \
   -o "$WORK/test" tools/test_image.c

"$WORK/test" "$WORK/in" "$WORK/out"

# IDCT checked directly against numpy: a scaling mistake still yields a
# plausible-looking image, so it deserves its own test.
cc -O2 -Wall -Wextra -o "$WORK/test_idct" tools/test_idct.c
python3 tools/check_idct.py "$WORK/test_idct"

python3 tools/compare_images.py "$WORK/in" "$WORK/out"

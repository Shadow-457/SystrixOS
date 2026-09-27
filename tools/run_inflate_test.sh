#!/bin/sh
# Host-side test for the kernel DEFLATE decoder.
#
# Builds tools/test_inflate.c together with kernel/inflate.c (with the
# kernel.h include stubbed out) and links against the system zlib so the
# two implementations can be compared directly.
set -e
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/systrix-inflate-test
mkdir -p "$OUT"

sed -e 's|#include "../include/kernel.h"|#include <stddef.h>\ntypedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64; typedef unsigned long usize;|' \
    kernel/inflate.c > "$OUT/inflate.c"

cc -O2 -g -Wall -Wextra -fsanitize=address,undefined -o "$OUT/test" \
   "$OUT/inflate.c" tools/test_inflate.c -lz

"$OUT/test"

#ifndef PNG_H
#define PNG_H

#include "../user/libc.h"

/* Simple PNG encoder for saving screenshots.
 * Encodes a 32-bit (ARGB/XRGB) framebuffer to a PNG file. */
int png_encode(const char *filename, uint32_t *fb, uint32_t w, uint32_t h);

#endif

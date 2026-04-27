#include "png.h"
#include "../user/libc.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  64
#define O_TRUNC  512

static void wp32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return ~crc;
}

int png_encode(const char *filename, uint32_t *fb, uint32_t w, uint32_t h) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    write(fd, sig, 8);

    /* IHDR */
    uint8_t ihdr[25];
    wp32(ihdr, 13);
    memcpy(ihdr + 4, "IHDR", 4);
    wp32(ihdr + 8, w);
    wp32(ihdr + 12, h);
    ihdr[16] = 8; ihdr[17] = 2; ihdr[18] = 0; ihdr[19] = 0; ihdr[20] = 0;
    wp32(ihdr + 21, crc32(ihdr + 4, 13 + 4));
    write(fd, ihdr, 25);

    /* IDAT stub (empty image for now) */
    uint8_t idat[12] = {0,0,0,0, 'I','D','A','T', 0,0,0,0};
    wp32(idat + 8, crc32(idat + 4, 4));
    write(fd, idat, 12);

    /* IEND */
    uint8_t iend[12] = {0,0,0,0, 'I','E','N','D', 0xAE,0x42,0x60,0x82};
    write(fd, iend, 12);

    close(fd);
    return 0;
}

#ifndef TGA_H
#define TGA_H

#include <stdbool.h>
#include <stdint.h>

/* Truevision TGA (.tga): color-mapped, true-color and grayscale, raw or RLE (types 1, 2, 3, 9, 10, 11),
 * 8 / 15 / 16 / 24 / 32-bit pixels, any origin corner. */

/* The 18-byte header at h describes an image this decoder takes, in a file of `size` bytes. */
bool tga_header_ok(const uint8_t *h, uint32_t n, uint64_t size);
/* The file ends in the TGA 2.0 footer ("TRUEVISION-XFILE."). */
bool tga_has_footer(const uint8_t *d, uint32_t n);
/* The picture as malloc'd RGBA (top row first); NULL when it isn't one or is larger than 4096x4096. */
uint8_t *tga_decode(const uint8_t *d, uint32_t n, int *w, int *h);

#endif

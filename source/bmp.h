#ifndef BMP_H
#define BMP_H

#include <stdbool.h>
#include <stdint.h>

/* Windows / OS/2 bitmaps (.bmp, .dib): 1 / 2 / 4 / 8-bit palette, RLE4 / RLE8, 16 / 24 / 32-bit (with
 * bit field masks, alpha included), BITMAPCOREHEADER to BITMAPV5HEADER, bottom-up or top-down. */

/* "BM" and a header this decoder takes, at h (n bytes of a `size` byte file). */
bool bmp_header_ok(const uint8_t *h, uint32_t n, uint64_t size);
/* The picture as malloc'd RGBA (top row first); NULL when it isn't one or is larger than 4096x4096. */
uint8_t *bmp_decode(const uint8_t *d, uint32_t n, int *w, int *h);

#endif

#ifndef PSD_H
#define PSD_H

#include <stdbool.h>
#include <stdint.h>

/* Adobe Photoshop .psd / .psb: the flattened image Photoshop stores after the layers (saved with
 * "Maximize Compatibility", which is the default). 1, 8 and 16 bits per channel; bitmap, grayscale,
 * duotone, indexed, RGB, CMYK and Lab; raw or RLE compressed. ZIP-compressed composites and 32-bit
 * float files are not read. */

bool psd_header_ok(const uint8_t *h, uint32_t n);
/* The picture as malloc'd RGBA (top row first). An image larger than max_dim either way is
 * decimated by a whole factor to fit. NULL when it isn't a PSD this decoder reads. */
uint8_t *psd_decode(const uint8_t *d, uint32_t n, int max_dim, int *w, int *h);

#endif

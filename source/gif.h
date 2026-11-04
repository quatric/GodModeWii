#ifndef GIF_H
#define GIF_H

#include <stdbool.h>
#include <stdint.h>

/* A GIF's first frame as malloc'd RGBA (the logical screen's size); NULL when it isn't one. */
uint8_t *gif_decode(const uint8_t *d, uint32_t n, int *w, int *h);

#endif

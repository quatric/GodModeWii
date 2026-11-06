#ifndef ASH_H
#define ASH_H

#include <stdbool.h>
#include <stdint.h>

/* ASH0 compression (Wii Menu layouts, Animal Crossing, My Pokemon Ranch): two Huffman trees, one for
 * literals / lengths and one for distances (11 bits wide in the Wii Menu, 15 in My Pokemon Ranch: both
 * are tried). Portable, no libogc. */

/* The size of the data inside, 0 if this is not ASH0. */
uint32_t ash_size(const uint8_t *in, uint32_t len);
/* Decompresses `in` into `out` (osz = ash_size()). */
bool ash_decompress(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t osz);

#endif

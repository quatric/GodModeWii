#ifndef QLZ_H
#define QLZ_H

#include <stdbool.h>
#include <stdint.h>

/* A QuickLZ 1.4.0 (level 3) or 1.20 stream of exactly n bytes: its decompressed size, header length
 * and version (140 / 120). */
bool qlz_header(const uint8_t *src, uint32_t n, uint32_t *out_size, uint32_t *hdr, int *version);
/* The whole stream decompressed into a new buffer; NULL when it isn't one, is bigger than max_out
 * or doesn't decode. */
uint8_t *qlz_unpack(const uint8_t *src, uint32_t n, uint32_t max_out, uint32_t *out_size);

#endif

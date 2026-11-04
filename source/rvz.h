#ifndef RVZ_H
#define RVZ_H

/* RVZ and WIA disc images (Dolphin / wit), read-only, with random access.
 * Written from Dolphin's format description (docs/WiaAndRvz.md); the design (the disc outside
 * partition data read by disc offset, Wii partition data read decrypted and without hashes, the
 * last group kept decoded) follows KakarottoCake's RiftWii.
 * Compression: none, purge (WIA), bzip2, LZMA, LZMA2 and Zstandard (RVZ), with RVZ packing. */

#include <stdint.h>
#include <stdbool.h>
#include "archive.h"

typedef struct rvz rvz_t;

bool rvz_probe(const uint8_t head[4]);              /* "RVZ\1" or "WIA\1" */
rvz_t *rvz_open(const src_t *file, const char **err);
void rvz_close(rvz_t *r);

uint64_t rvz_iso_size(const rvz_t *r);
int rvz_disc_type(const rvz_t *r);                  /* 1 GameCube, 2 Wii */
const char *rvz_describe(const rvz_t *r);            /* "RVZ, Zstandard, 128 KiB chunks" */

/* The disc outside Wii partition data, by disc offset (a GameCube disc is all of it). Partition
 * data is not stored in its encrypted form and reads as zeros here. */
bool rvz_read_raw(rvz_t *r, uint64_t off, uint8_t *buf, uint32_t len);

/* Wii partitions: their data as a game reads it, decrypted and without hashes (0x7C00 bytes for
 * each 0x8000 on the disc). */
int rvz_partitions(const rvz_t *r);
/* where its encrypted data starts on the disc */
uint64_t rvz_part_disc_offset(const rvz_t *r, int i);
uint64_t rvz_part_size(const rvz_t *r, int i);
bool rvz_read_part(rvz_t *r, int i, uint64_t off, uint8_t *buf, uint32_t len);

#endif

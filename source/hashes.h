#ifndef HASHES_H
#define HASHES_H

#include <stdint.h>
#include <stddef.h>

/* Tiny SHA-1 and XXH32 implementations, used to recognise a key by its hash
 * (the same way xyzzy-mod does) while scanning memory. */
uint32_t xxh32(const void *data, size_t len, uint32_t seed);
void sha1_digest(const void *data, size_t len, uint8_t out[20]);

#endif

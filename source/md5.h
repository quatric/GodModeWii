#ifndef MD5_H
#define MD5_H

#include <stddef.h>
#include <stdint.h>

/* MD5 (RFC 1321): content.bin headers are checked with it. */
void md5(const void *data, size_t len, uint8_t out[16]);

#endif

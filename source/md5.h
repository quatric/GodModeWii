#ifndef MD5_H
#define MD5_H

#include <stddef.h>
#include <stdint.h>

/* MD5 (RFC 1321): content.bin headers are checked with it. */
void md5(const void *data, size_t len, uint8_t out[16]);

typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; } md5_t;
void md5_init(md5_t *c);
void md5_update(md5_t *c, const void *data, size_t len);
void md5_final(md5_t *c, uint8_t out[16]);

#endif

#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    uint32_t used;
} sha1_t;

void sha1_init(sha1_t *c);
void sha1_update(sha1_t *c, const void *data, size_t len);
void sha1_final(sha1_t *c, uint8_t out[20]);

#endif

#include <string.h>
#include "hashes.h"

/* ---------------------------------------------------------------- XXH32 */

#define P1 2654435761U
#define P2 2246822519U
#define P3 3266489917U
#define P4 668265263U
#define P5 374761393U

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t xxh_round(uint32_t acc, uint32_t input)
{
    return rotl32(acc + input * P2, 13) * P1;
}

uint32_t xxh32(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = data, *end = p + len;
    uint32_t h;

    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        do {
            v1 = xxh_round(v1, read_le32(p)); p += 4;
            v2 = xxh_round(v2, read_le32(p)); p += 4;
            v3 = xxh_round(v3, read_le32(p)); p += 4;
            v4 = xxh_round(v4, read_le32(p)); p += 4;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P5;
    }

    h += (uint32_t)len;
    while (p + 4 <= end) {
        h += read_le32(p) * P3;
        h = rotl32(h, 17) * P4;
        p += 4;
    }
    while (p < end) {
        h += (*p++) * P5;
        h = rotl32(h, 11) * P1;
    }
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

/* ---------------------------------------------------------------- SHA-1 */

static void sha1_block(uint32_t st[5], const uint8_t *b)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);            k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d;                      k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = bb ^ c ^ d;                      k = 0xCA62C1D6; }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(bb, 30); bb = a; a = t;
    }
    st[0] += a; st[1] += bb; st[2] += c; st[3] += d; st[4] += e;
}

void sha1_digest(const void *data, size_t len, uint8_t out[20])
{
    uint32_t st[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const uint8_t *p = data;
    size_t n = len;
    while (n >= 64) {
        sha1_block(st, p);
        p += 64;
        n -= 64;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t total = (n < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(st, tail);
    if (total == 128)
        sha1_block(st, tail + 64);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = st[i] >> 24;
        out[i * 4 + 1] = st[i] >> 16;
        out[i * 4 + 2] = st[i] >> 8;
        out[i * 4 + 3] = st[i];
    }
}

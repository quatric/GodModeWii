#ifndef FSUTIL_H
#define FSUTIL_H

/* Helpers shared by the read-only file system readers (ext, HFS+, XFS, APFS, ZFS, partition tables):
 * little endian reads, UTF-16 names, and a small cache of 4 KiB chunks in front of a src_t so that
 * walking directories and B-trees does not hit a USB drive for every few bytes. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

#define FSC_CHUNK 4096u
#define FSC_N 48

typedef struct {
    const src_t *src;
    uint64_t key[FSC_N];        /* chunk number + 1, 0 = empty */
    uint32_t stamp[FSC_N];
    uint8_t *data;              /* FSC_N * FSC_CHUNK */
    uint32_t tick;
} fsc_t;

static inline bool fsc_init(fsc_t *c, const src_t *src)
{
    memset(c, 0, sizeof(*c));
    c->src = src;
    c->data = malloc((size_t)FSC_N * FSC_CHUNK);
    return c->data != NULL;
}

static inline void fsc_free(fsc_t *c) { free(c->data); c->data = NULL; }

/* Reads len bytes at off (zero past the end of the source is an error). */
static inline bool fsc_read(fsc_t *c, uint64_t off, void *buf, uint32_t len)
{
    uint8_t *out = buf;
    if (off + len > c->src->size || off + len < off) return false;
    if (len >= 4 * FSC_CHUNK)                           /* big reads go straight through */
        return src_read_full(c->src, off, out, len);
    while (len) {
        uint64_t ch = off / FSC_CHUNK;
        uint32_t skew = (uint32_t)(off % FSC_CHUNK), n = FSC_CHUNK - skew;
        if (n > len) n = len;
        int slot = -1, oldest = 0;
        for (int i = 0; i < FSC_N; i++) {
            if (c->key[i] == ch + 1) { slot = i; break; }
            if (c->stamp[i] < c->stamp[oldest]) oldest = i;
        }
        if (slot < 0) {
            slot = oldest;
            uint64_t base = ch * FSC_CHUNK;
            uint32_t want = FSC_CHUNK;
            if (base + want > c->src->size) want = (uint32_t)(c->src->size - base);
            c->key[slot] = 0;
            memset(c->data + (size_t)slot * FSC_CHUNK, 0, FSC_CHUNK);
            if (!src_read_full(c->src, base, c->data + (size_t)slot * FSC_CHUNK, want)) return false;
            c->key[slot] = ch + 1;
        }
        c->stamp[slot] = ++c->tick;
        memcpy(out, c->data + (size_t)slot * FSC_CHUNK + skew, n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

/* UTF-16 (big or little endian code units already decoded into u16) -> UTF-8. Returns the length. */
static inline size_t fs_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < outsz; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 0x3F)); out[o++] = (char)(0x80 | (c >> 6 & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
    return o;
}

/* a name from a file system: bytes copied, made safe for the tree, never empty */
static inline void fs_name(char *out, size_t outsz, const uint8_t *name, size_t len, const char *fallback)
{
    if (len >= outsz) len = outsz - 1;
    memcpy(out, name, len);
    out[len] = 0;
    tree_clean_name(out);
    if (!out[0]) snprintf(out, outsz, "%s", fallback);
}

#define FS_MAX_DIR_ENTRIES 200000

#endif

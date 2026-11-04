/*
 * RVZ and WIA disc images. See rvz.h.
 *
 * File layout (big endian): a 0x48 byte file header, then the 0xDC byte disc struct (method,
 * chunk size, the disc's first 0x80 bytes, where the partition, raw data and group tables are).
 * Raw data entries cover the disc outside Wii partition data by disc offset; partition entries
 * cover each partition's data, stored decrypted and without hashes, in two segments. Both point
 * at groups: one chunk of data each, compressed on its own, so any part of the disc can be read
 * by decoding one group. Wii partition groups start with hash exception lists, which a reader
 * that doesn't rebuild hashes just skips. RVZ adds Zstandard, groups stored uncompressed when
 * that is smaller, chunks under 2 MiB, and "packing": runs of the disc's pseudorandom padding
 * stored as the seed of the generator that made them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bzlib.h>
#include "zstd.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"

#include "rvz.h"

enum { M_NONE, M_PURGE, M_BZIP2, M_LZMA, M_LZMA2, M_ZSTD };

#define SECTOR      0x8000u
#define SECTOR_DATA 0x7C00u
#define LIST_MAX    3328u               /* exceptions per list: Dolphin's limit, covers all hashes and padding */
#define CHUNK_MAX   (32u << 20)

typedef struct {
    uint64_t start, end;
    uint32_t group, ngroups;
} raw_t;
typedef struct {
    uint32_t first, count, group, ngroups;
} pd_t;
typedef struct {
    pd_t pd[2];
} part_t;
typedef struct {
    uint32_t off4, size, packed;
} group_t;

struct rvz {
    src_t f;
    bool is_rvz;
    int disc_type, method;
    uint32_t chunk;
    uint8_t dhead[0x80], props[7];
    uint64_t iso_size;
    raw_t *raw;
    uint32_t nraw;
    part_t *part;
    uint32_t npart;
    group_t *grp;
    uint32_t ngrp;
    ZSTD_DCtx *zd;
    uint8_t *stored, *tmp, *cache;      /* a group as stored, decompressed, decoded */
    uint32_t stored_cap, tmp_cap;
    uint32_t cached;                    /* group in cache, or ~0 */
    uint32_t cached_len;
    char desc[96];
};

/* ---------------------------------------------------------------- the padding generator */

typedef struct {
    uint32_t buf[521];
    uint32_t pos;
} junk_t;

static void junk_advance(junk_t *j)
{
    for (int i = 0; i < 32; i++) j->buf[i] ^= j->buf[i + 521 - 32];
    for (int i = 32; i < 521; i++) j->buf[i] ^= j->buf[i - 32];
}

static void junk_seed(junk_t *j, const uint8_t *seed)
{
    for (int i = 0; i < 17; i++) j->buf[i] = be32(seed + i * 4);
    for (int i = 17; i < 521; i++)
        j->buf[i] = (j->buf[i - 17] << 23) ^ (j->buf[i - 16] >> 9) ^ j->buf[i - 1];
    for (int i = 0; i < 4; i++) junk_advance(j);
    j->pos = 0;
}

static void junk_out(junk_t *j, uint8_t *dst, uint32_t n, bool write)
{
    static const int shift[4] = { 24, 18, 8, 0 };
    while (n) {
        if (j->pos == 521 * 4) {
            junk_advance(j);
            j->pos = 0;
        }
        uint32_t k = 521 * 4 - j->pos;
        if (k > n) k = n;
        if (write)
            for (uint32_t i = 0; i < k; i++, j->pos++)
                *dst++ = (uint8_t)(j->buf[j->pos >> 2] >> shift[j->pos & 3]);
        else
            j->pos += k;
        n -= k;
    }
}

/* RVZ packing: {u32 size; data} runs, or {u32 size | 0x80000000; 68 byte seed} runs of padding.
 * data_offset is where the first output byte sits, for the generator. */
static bool unpack(const uint8_t *in, uint32_t n, uint64_t data_offset, uint8_t *out, uint32_t len)
{
    uint32_t p = 0, o = 0;
    junk_t *j = NULL;

    while (o < len) {
        if (n - p < 4) {
            free(j);
            return false;
        }

        uint32_t sz = be32(in + p);
        p += 4;
        bool gen = sz & 0x80000000u;
        sz &= 0x7FFFFFFFu;
        if (sz > len - o) {
            free(j);
            return false;
        }

        if (gen) {
            if (n - p < 68) {
                free(j);
                return false;
            }
            if (!j && !(j = malloc(sizeof(*j)))) return false;
            junk_seed(j, in + p);
            p += 68;
            junk_out(j, NULL, (uint32_t)((data_offset + o) % SECTOR), false);
            junk_out(j, out + o, sz, true);
        } else {
            if (sz > n - p) {
                free(j);
                return false;
            }
            memcpy(out + o, in + p, sz);
            p += sz;
        }

        o += sz;
    }

    free(j);
    return true;
}

/* ---------------------------------------------------------------- decompression */

static void *lz_alloc(ISzAllocPtr p, size_t n)
{
    (void)p;
    return malloc(n);
}
static void lz_free(ISzAllocPtr p, void *a)
{
    (void)p;
    free(a);
}
static const ISzAlloc LZ_ALLOC = { lz_alloc, lz_free };

/* in -> out (cap bytes); *got = bytes produced. PURGE expects exactly cap bytes of output. */
static bool decompress(rvz_t *r, const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap,
                       uint32_t *got)
{
    switch (r->method) {
    case M_NONE:
        if (n > cap) n = cap;
        memcpy(out, in, n);
        *got = n;
        return true;
    case M_PURGE: {                     /* {u32 offset, u32 size, data} segments, then a SHA-1 */
        if (n < 20) return false;
        memset(out, 0, cap);
        uint32_t p = 0, end = n - 20;
        while (p < end) {
            if (end - p < 8) return false;
            uint32_t o = be32(in + p), s = be32(in + p + 4);
            p += 8;
            if (s > end - p || o > cap || s > cap - o) return false;
            memcpy(out + o, in + p, s);
            p += s;
        }
        *got = cap;
        return true;
    }
    case M_BZIP2: {
        unsigned int dl = cap;
        if (BZ2_bzBuffToBuffDecompress((char *)out, &dl, (char *)in, n, 0, 0) != BZ_OK)
            return false;
        *got = dl;
        return true;
    }
    case M_LZMA: case M_LZMA2: {
        SizeT dl = cap, sl = n;
        ELzmaStatus st;
        SRes res = r->method == M_LZMA
            ? LzmaDecode(out, &dl, in, &sl, r->props, 5, LZMA_FINISH_ANY, &st, &LZ_ALLOC)
            : Lzma2Decode(out, &dl, in, &sl, r->props[0], LZMA_FINISH_ANY, &st, &LZ_ALLOC);
        if (res != SZ_OK && !(res == SZ_ERROR_INPUT_EOF && dl)) return false;
        *got = (uint32_t)dl;
        return true;
    }
    case M_ZSTD: {
        size_t z = ZSTD_decompressDCtx(r->zd, out, cap, in, n);
        if (ZSTD_isError(z)) return false;
        *got = (uint32_t)z;
        return true;
    }
    }

    return false;
}

static bool grow(uint8_t **b, uint32_t *cap, uint32_t need)
{
    if (need <= *cap) return true;
    uint8_t *nb = realloc(*b, need);
    if (!nb) return false;
    *b = nb;
    *cap = need;
    return true;
}

/* Bytes taken by `lists` exception lists at the start of p (and the padding to 4 when pad). */
static bool skip_lists(const uint8_t *p, uint32_t n, uint32_t lists, bool pad, uint32_t *used)
{
    uint32_t q = 0;
    for (uint32_t i = 0; i < lists; i++) {
        if (n - q < 2) return false;
        uint32_t c = be16(p + q);
        q += 2;
        if ((uint64_t)c * 22 > n - q) return false;
        q += c * 22;
    }
    if (pad && lists) q = (q + 3) & ~3u;
    if (q > n) return false;
    *used = q;
    return true;
}

/* Decodes group g (payload bytes once decoded) into r->cache. */
static bool load_group(rvz_t *r, uint32_t g, uint32_t payload, uint32_t lists, uint64_t data_offset)
{
    if (r->cached == g) return true;
    r->cached = ~0u;
    if (g >= r->ngrp || payload > r->chunk) return false;
    const group_t *e = &r->grp[g];
    uint32_t stored = r->is_rvz ? e->size & 0x7FFFFFFFu : e->size;
    bool compressed = r->is_rvz ? (e->size >> 31) != 0 : r->method != M_NONE;
    if (!stored) {                      /* all zeros, lists with no exceptions */
        memset(r->cache, 0, payload);
        r->cached = g;
        r->cached_len = payload;
        return true;
    }

    if (stored > CHUNK_MAX + 0x100000 || !grow(&r->stored, &r->stored_cap, stored)) return false;
    if (!src_read_full(&r->f, (uint64_t)e->off4 * 4, r->stored, stored)) return false;
    uint32_t body_len = e->packed ? e->packed : payload;
    const uint8_t *body;
    uint32_t have, used;
    if (compressed && r->method != M_PURGE) {
        /* exception lists are compressed along with the data */
        uint32_t cap = body_len + lists * (2 + LIST_MAX * 22) + 64;
        if (!grow(&r->tmp, &r->tmp_cap, cap) ||
            !decompress(r, r->stored, stored, r->tmp, cap, &have))
            return false;
        if (!skip_lists(r->tmp, have, lists, false, &used)) return false;
        body = r->tmp + used;
        have -= used;
    } else {
        /* lists are stored as they are, padded to 4, before the data (or the PURGE segments) */
        if (!skip_lists(r->stored, stored, lists, true, &used)) return false;
        if (compressed) {
            if (!grow(&r->tmp, &r->tmp_cap, body_len) ||
                !decompress(r, r->stored + used, stored - used, r->tmp, body_len, &have))
                return false;
            body = r->tmp;
        } else {
            body = r->stored + used;
            have = stored - used;
        }
    }

    if (e->packed) {
        if (!unpack(body, have, data_offset, r->cache, payload)) return false;
    } else {
        if (have < payload) return false;
        memcpy(r->cache, body, payload);
    }

    r->cached = g;
    r->cached_len = payload;
    return true;
}

/* ---------------------------------------------------------------- reading */

bool rvz_read_raw(rvz_t *r, uint64_t off, uint8_t *buf, uint32_t len)
{
    while (len) {
        if (off < 0x80) {               /* the disc header lives in the disc struct */
            uint32_t n = 0x80 - (uint32_t)off;
            if (n > len) n = len;
            memcpy(buf, r->dhead + off, n);
            buf += n;
            off += n;
            len -= n;
            continue;
        }

        const raw_t *e = NULL;
        uint64_t next = r->iso_size > off ? r->iso_size : off + len;
        for (uint32_t i = 0; i < r->nraw; i++) {
            if (off >= r->raw[i].start && off < r->raw[i].end) {
                e = &r->raw[i];
                break;
            }
            if (r->raw[i].start > off && r->raw[i].start < next) next = r->raw[i].start;
        }
        if (!e) {                       /* not raw data: partition data, or past the end */
            uint64_t n = next - off;
            if (n > len) n = len;
            memset(buf, 0, (size_t)n);
            buf += n;
            off += n;
            len -= (uint32_t)n;
            continue;
        }

        uint64_t rel = off - e->start;
        uint32_t gi = (uint32_t)(rel / r->chunk), in = (uint32_t)(rel % r->chunk);
        uint64_t gstart = e->start + (uint64_t)gi * r->chunk, left = e->end - gstart;
        uint32_t payload = left < r->chunk ? (uint32_t)left : r->chunk;
        if (gi >= e->ngroups || !load_group(r, e->group + gi, payload, 0, gstart)) return false;

        uint32_t n = payload - in;
        if (n > len) n = len;
        memcpy(buf, r->cache + in, n);
        buf += n;
        off += n;
        len -= n;
    }

    return true;
}

int rvz_partitions(const rvz_t *r) { return (int)r->npart; }

uint64_t rvz_part_disc_offset(const rvz_t *r, int i)
{
    return i >= 0 && (uint32_t)i < r->npart ? (uint64_t)r->part[i].pd[0].first * SECTOR : 0;
}

uint64_t rvz_part_size(const rvz_t *r, int i)
{
    if (i < 0 || (uint32_t)i >= r->npart) return 0;
    const part_t *p = &r->part[i];
    uint32_t last =
        p->pd[1].count ? p->pd[1].first + p->pd[1].count : p->pd[0].first + p->pd[0].count;
    return (uint64_t)(last - p->pd[0].first) * SECTOR_DATA;
}

bool rvz_read_part(rvz_t *r, int i, uint64_t off, uint8_t *buf, uint32_t len)
{
    if (i < 0 || (uint32_t)i >= r->npart) return false;
    const part_t *p = &r->part[i];
    uint32_t gsz = r->chunk / SECTOR * SECTOR_DATA;
    uint32_t lists = r->chunk >= 0x200000 ? r->chunk / 0x200000 : 1;

    while (len) {
        const pd_t *d = NULL;
        uint64_t seg = 0;
        for (int k = 0; k < 2; k++) {
            const pd_t *c = &p->pd[k];
            uint64_t s = (uint64_t)(c->first - p->pd[0].first) * SECTOR_DATA,
                     e = s + (uint64_t)c->count * SECTOR_DATA;
            if (c->count && off >= s && off < e) {
                d = c;
                seg = s;
                break;
            }
        }
        if (!d) {
            memset(buf, 0, len);
            return true;
        }

        uint64_t rel = off - seg, size = (uint64_t)d->count * SECTOR_DATA;
        uint32_t gi = (uint32_t)(rel / gsz), in = (uint32_t)(rel % gsz);
        uint64_t left = size - (uint64_t)gi * gsz;
        uint32_t payload = left < gsz ? (uint32_t)left : gsz;
        if (gi >= d->ngroups ||
            !load_group(r, d->group + gi, payload, lists, seg + (uint64_t)gi * gsz))
            return false;

        uint32_t n = payload - in;
        if (n > len) n = len;
        memcpy(buf, r->cache + in, n);
        buf += n;
        off += n;
        len -= n;
    }

    return true;
}

/* ---------------------------------------------------------------- opening */

bool rvz_probe(const uint8_t h[4])
{
    return (!memcmp(h, "RVZ", 3) || !memcmp(h, "WIA", 3)) && h[3] == 1;
}

uint64_t rvz_iso_size(const rvz_t *r) { return r->iso_size; }

void rvz_close(rvz_t *r)
{
    if (!r) return;
    if (r->zd) ZSTD_freeDCtx(r->zd);
    free(r->raw);
    free(r->part);
    free(r->grp);
    free(r->stored);
    free(r->tmp);
    free(r->cache);
    free(r);
}

/* A table stored compressed at `off` (stored bytes) that decompresses to exactly `need` bytes. */
static uint8_t *read_table(rvz_t *r, uint64_t off, uint32_t stored, uint32_t need)
{
    if (stored > 64u << 20 || need > 64u << 20) return NULL;
    uint8_t *in = malloc(stored ? stored : 1), *out = malloc(need ? need : 1);
    uint32_t got = 0;
    bool ok = in && out && src_read_full(&r->f, off, in, stored) &&
              decompress(r, in, stored, out, need, &got) && got >= need;
    free(in);
    if (!ok) {
        free(out);
        return NULL;
    }
    return out;
}

rvz_t *rvz_open(const src_t *f, const char **err)
{
    static const char *const names[] = { "none", "purge", "bzip2", "LZMA", "LZMA2", "Zstandard" };
    uint8_t h[0x48 + 0xDC];
    *err = "Not an RVZ or WIA image";
    if (f->size < sizeof(h) || !src_read_full(f, 0, h, sizeof(h)) || !rvz_probe(h)) return NULL;
    rvz_t *r = calloc(1, sizeof(*r));
    if (!r) {
        *err = "Out of memory";
        return NULL;
    }

    r->f = *f;
    r->cached = ~0u;
    r->is_rvz = h[0] == 'R';
    uint32_t compat = be32(h + 8);
    const uint8_t *d = h + 0x48;
    r->iso_size = be64(h + 0x24);
    r->disc_type = (int)be32(d);
    r->method = (int)be32(d + 4);
    r->chunk = be32(d + 0xC);
    memcpy(r->dhead, d + 0x10, 0x80);
    memcpy(r->props, d + 0xD5, 7);
    uint32_t npart = be32(d + 0x90), psize = be32(d + 0x94);
    uint64_t poff = be64(d + 0x98);
    r->nraw = be32(d + 0xB4);
    uint64_t roff = be64(d + 0xB8);
    uint32_t rsize = be32(d + 0xC0);
    r->ngrp = be32(d + 0xC4);
    uint64_t goff = be64(d + 0xC8);
    uint32_t gsize = be32(d + 0xD0), gent = r->is_rvz ? 12 : 8;

    if (compat > 0x01000000) {
        *err = "This image needs a newer RVZ / WIA reader";
        goto fail;
    }

    if (r->method > M_ZSTD || (r->is_rvz && r->method == M_PURGE) ||
        (!r->is_rvz && r->method == M_ZSTD)) {
        *err = "This image uses an unknown compression method";
        goto fail;
    }

    bool chunk_ok = r->chunk >= 0x200000 ?
                    r->chunk % 0x200000 == 0 :
                    r->is_rvz && r->chunk >= 0x8000 && !(r->chunk & (r->chunk - 1));
    if (!chunk_ok) {
        *err = "This image's chunk size isn't valid";
        goto fail;
    }

    if (r->chunk > CHUNK_MAX) {
        *err = "This image's chunks are over 32 MiB, too large for the Wii's memory";
        goto fail;
    }

    if (r->disc_type != 1 && r->disc_type != 2) {
        *err = "This image isn't a GameCube or Wii disc";
        goto fail;
    }

    if (npart > 16 || r->nraw > 100000 || r->ngrp > 10000000 || psize < 0x30 - 16 ||
        psize > 0x100) {
        *err = "This image's tables are damaged";
        goto fail;
    }

    if (r->method == M_ZSTD && !(r->zd = ZSTD_createDCtx())) {
        *err = "Out of memory";
        goto fail;
    }

    /* partitions: stored uncompressed */
    if (npart) {
        uint8_t *pt = malloc(npart * psize);
        r->part = calloc(npart, sizeof(part_t));
        if (!pt || !r->part || !src_read_full(f, poff, pt, npart * psize)) {
            free(pt);
            *err = "This image's tables are damaged";
            goto fail;
        }
        for (uint32_t i = 0; i < npart; i++) {
            uint8_t e[0x30] = { 0 };
            memcpy(e, pt + i * psize, psize < 0x30 ? psize : 0x30);
            for (int k = 0; k < 2; k++) {
                const uint8_t *q = e + 16 + k * 16;
                r->part[i].pd[k] = (pd_t){ be32(q), be32(q + 4), be32(q + 8), be32(q + 12) };
            }
        }
        free(pt);
        r->npart = npart;
    }

    uint8_t *rt = read_table(r, roff, rsize, r->nraw * 24);
    uint8_t *gt = rt ? read_table(r, goff, gsize, r->ngrp * gent) : NULL;
    if (!rt || !gt) {
        free(rt);
        free(gt);
        *err = "This image's tables couldn't be read";
        goto fail;
    }

    r->raw = calloc(r->nraw ? r->nraw : 1, sizeof(raw_t));
    r->grp = calloc(r->ngrp ? r->ngrp : 1, sizeof(group_t));
    if (!r->raw || !r->grp) {
        free(rt);
        free(gt);
        *err = "Out of memory";
        goto fail;
    }

    for (uint32_t i = 0; i < r->nraw; i++) {
        const uint8_t *q = rt + i * 24;
        uint64_t o = be64(q), s = be64(q + 8);
        /* the first one starts at 0x80: round down */
        r->raw[i].start = o & ~(uint64_t)(SECTOR - 1);
        r->raw[i].end = o + s;
        r->raw[i].group = be32(q + 16);
        r->raw[i].ngroups = be32(q + 20);
    }

    for (uint32_t i = 0; i < r->ngrp; i++) {
        const uint8_t *q = gt + i * gent;
        r->grp[i].off4 = be32(q);
        r->grp[i].size = be32(q + 4);
        r->grp[i].packed = r->is_rvz ? be32(q + 8) : 0;
    }

    free(rt);
    free(gt);
    r->cache = malloc(r->chunk);
    if (!r->cache) {
        *err = "Out of memory";
        goto fail;
    }

    unsigned kib = r->chunk / 1024;
    snprintf(r->desc, sizeof(r->desc), "%s, %s, %u %s chunks", r->is_rvz ? "RVZ" : "WIA",
             names[r->method], kib >= 1024 ? kib / 1024 : kib, kib >= 1024 ? "MiB" : "KiB");
    *err = NULL;
    return r;
fail:
    rvz_close(r);
    return NULL;
}

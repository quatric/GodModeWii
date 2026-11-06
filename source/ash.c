/*
 * ASH0 decompression (Wii Menu, Animal Crossing, My Pokemon Ranch). See ash.h.
 */
#include <stdlib.h>
#include <string.h>

#include "ash.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

typedef struct {
    const uint8_t *src;
    uint32_t size, pos, word, used;
} ashbits;

static bool ash_feed(ashbits *b)
{
    if (b->size < 4 || b->pos > b->size - 4) return false;
    b->word = be32(b->src + b->pos);
    b->pos += 4;
    b->used = 32;
    return true;
}

static bool ash_read(ashbits *b, uint32_t n, uint32_t *v)
{
    uint32_t val = 0;
    while (n--) {
        if (!b->used && !ash_feed(b)) return false;
        val = val << 1 | b->word >> 31;
        b->word <<= 1;
        b->used--;
    }
    *v = val;
    return true;
}

/* Huffman tree, stored as left / right child tables above the leaf range. */
static bool ash_tree(ashbits *b, uint32_t width, uint32_t *left, uint32_t *right, uint32_t *root,
                     uint32_t *work)
{
    const uint32_t max = 1u << width, cap = 2 * max - 1, wcap = 2 * max;
    uint32_t used = 0, nodes = 0, next = max;
    for (;;) {
        uint32_t bit, value;
        if (!ash_read(b, 1, &bit)) return false;
        if (bit) {
            if (used + 2 > wcap || next >= cap) return false;
            work[used++] = next | 0x80000000u;
            work[used++] = next | 0x40000000u;
            nodes += 2;
            next++;
            continue;
        }
        if (!ash_read(b, width, &value) || value >= max) return false;
        *root = value;
        while (nodes) {
            uint32_t node = work[--used], idx = node & 0x3fffffffu;
            if (idx >= cap) return false;
            nodes--;
            if (node & 0x80000000u) {
                right[idx] = *root;
                *root = idx;
            } else {
                left[idx] = *root;
                break;
            }
        }
        if (!nodes) return true;
    }
}

static bool ash_sym(ashbits *b, uint32_t root, uint32_t max, const uint32_t *l, const uint32_t *r,
                    uint32_t *v)
{
    uint32_t s = root;
    while (s >= max) {
        uint32_t bit;
        if (s >= 2 * max - 1 || !ash_read(b, 1, &bit)) return false;
        s = bit ? r[s] : l[s];
    }
    *v = s;
    return true;
}

/* The distance tree is 11 bits wide in the Wii Menu and Animal Crossing,
 * 15 in My Pokémon Ranch; nothing in the header says which, so try both. */
static bool ash_try(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t osz, uint32_t dbits)
{
    const uint32_t smax = 1u << 9, dmax = 1u << dbits;
    uint32_t *mem = calloc((2 * smax - 1) * 2 + (2 * dmax - 1) * 2 + 2 * dmax, sizeof(uint32_t));
    if (!mem) return false;
    uint32_t *sl = mem, *sr = sl + 2 * smax - 1, *dl = sr + 2 * smax - 1, *dr = dl + 2 * dmax - 1,
             *work = dr + 2 * dmax - 1;

    ashbits sb = { in, len, 0x0C, 0, 0 }, db = { in, len, be32(in + 8), 0, 0 };
    uint32_t sroot = 0, droot = 0;
    bool ok = ash_feed(&sb) && ash_feed(&db) && ash_tree(&sb, 9, sl, sr, &sroot, work) &&
              ash_tree(&db, dbits, dl, dr, &droot, work);

    for (uint32_t pos = 0; ok && pos < osz;) {
        uint32_t sym, dist;
        if (!ash_sym(&sb, sroot, smax, sl, sr, &sym)) {
            ok = false;
            break;
        }
        if (sym < 0x100) {
            out[pos++] = (uint8_t)sym;
            continue;
        }
        uint32_t n = sym - 0x100 + 3;
        if (!ash_sym(&db, droot, dmax, dl, dr, &dist) || dist >= pos || n > osz - pos) {
            ok = false;
            break;
        }
        for (uint32_t k = 0; k < n; k++, pos++) out[pos] = out[pos - dist - 1];
    }

    free(mem);
    return ok;
}

bool ash_decompress(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t osz)
{
    if (len < 12 || memcmp(in, "ASH0", 4)) return false;
    return ash_try(in, len, out, osz, 11) || ash_try(in, len, out, osz, 15);
}

uint32_t ash_size(const uint8_t *in, uint32_t len)
{
    return len >= 12 && !memcmp(in, "ASH0", 4) ? be32(in + 4) & 0x00FFFFFF : 0;
}

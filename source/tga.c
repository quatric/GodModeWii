/*
 * Truevision TGA decoding. See tga.h.
 *
 * Header (little-endian): id length, color map type, image type, color map first entry (u16), length
 * (u16) and entry bits, x / y origin, width, height (u16 each), pixel bits, descriptor (bits 0-3 the
 * alpha bits, bit 4 right-to-left, bit 5 top-to-bottom). Then the id, the color map and the pixels; RLE
 * packets are a byte (bit 7 a run, the low 7 bits the count - 1) and one pixel for a run or count pixels.
 */
#include <stdlib.h>
#include <string.h>

#include "tga.h"

#define TGA_MAX 4096

static uint32_t u16le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

static bool type_ok(int type) { return type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11; }

bool tga_header_ok(const uint8_t *h, uint32_t n, uint64_t size)
{
    if (n < 18 || size < 18) return false;
    int cmtype = h[1], type = h[2], bits = h[16];
    uint32_t cmlen = u16le(h + 5), cmbits = h[7], w = u16le(h + 12), ht = u16le(h + 14);
    if (!type_ok(type) || cmtype > 1 || !w || !ht || w > TGA_MAX || ht > TGA_MAX || (h[17] & 0xC0)) return false;
    bool mapped = (type & 7) == 1;
    if (mapped) {
        if (cmtype != 1 || !cmlen || (bits != 8 && bits != 16)) return false;
        if (cmbits != 15 && cmbits != 16 && cmbits != 24 && cmbits != 32) return false;
    } else if ((type & 7) == 3) {
        if (bits != 8 && bits != 16) return false;
    } else if (bits != 15 && bits != 16 && bits != 24 && bits != 32)
        return false;
    if (cmtype == 1 && cmbits != 15 && cmbits != 16 && cmbits != 24 && cmbits != 32) return false;
    uint64_t need = 18 + h[0] + (cmtype ? (uint64_t)cmlen * ((cmbits + 7) / 8) : 0);
    if (type < 8) need += (uint64_t)w * ht * ((bits + 7) / 8);   /* raw: all the pixels are there */
    else need += 2;                                             /* RLE: at least one packet */
    return need <= size;
}

bool tga_has_footer(const uint8_t *d, uint32_t n)
{
    return n >= 18 + 26 && !memcmp(d + n - 18, "TRUEVISION-XFILE.", 18);
}

/* One stored pixel / color map entry of `bits` at p to RGBA. alpha: the 16 / 32-bit alpha is used. */
static void color(const uint8_t *p, int bits, bool gray, bool alpha, uint8_t *o)
{
    if (gray) {
        o[0] = o[1] = o[2] = p[0];
        o[3] = bits == 16 ? p[1] : 255;
        return;
    }
    switch (bits) {
    case 15:
    case 16: {
        uint32_t v = u16le(p);
        uint32_t r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
        o[0] = r << 3 | r >> 2;
        o[1] = g << 3 | g >> 2;
        o[2] = b << 3 | b >> 2;
        o[3] = bits == 16 && alpha ? (v & 0x8000 ? 255 : 0) : 255;
        break;
    }
    case 24:
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255;
        break;
    default:
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = alpha ? p[3] : 255;
        break;
    }
}

uint8_t *tga_decode(const uint8_t *d, uint32_t n, int *wp, int *hp)
{
    if (!tga_header_ok(d, n, n)) return NULL;
    int cmtype = d[1], type = d[2], bits = d[16], desc = d[17];
    uint32_t cmfirst = u16le(d + 3), cmlen = u16le(d + 5), cmbits = d[7];
    int w = (int)u16le(d + 12), h = (int)u16le(d + 14);
    bool mapped = (type & 7) == 1, gray = (type & 7) == 3, rle = type >= 8;
    int abits = desc & 15;
    /* 32-bit pixels with no alpha bits declared still often carry alpha; it is checked below */
    bool alpha = abits > 0 || (mapped ? cmbits == 32 : bits == 32);

    uint32_t at = 18 + d[0];
    const uint8_t *map = NULL;
    uint32_t mapsz = (cmbits + 7) / 8;
    if (cmtype == 1) {
        map = d + at;
        at += cmlen * mapsz;
    }
    uint32_t psz = (bits + 7) / 8, total = (uint32_t)w * h;
    uint8_t *px = malloc((size_t)total * 4);
    if (!px) return NULL;

    uint8_t one[4];
    const uint8_t *src = NULL;
    uint32_t run = 0, lit = 0;                  /* RLE: pixels left in the current run / literal packet */
    bool ok = true;
    for (uint32_t i = 0; i < total && ok; i++) {
        const uint8_t *p;
        if (!rle) {
            p = d + at;
            at += psz;
        } else {
            if (!run && !lit) {
                if (at >= n) { ok = false; break; }
                uint8_t c = d[at++];
                if (c & 0x80) {
                    run = (c & 0x7F) + 1;
                    if ((uint64_t)at + psz > n) { ok = false; break; }
                    src = d + at;
                    at += psz;
                } else
                    lit = (c & 0x7F) + 1;
            }
            if (run) {
                p = src;
                run--;
            } else {
                if ((uint64_t)at + psz > n) { ok = false; break; }
                p = d + at;
                at += psz;
                lit--;
            }
        }
        uint8_t *o = px + (size_t)i * 4;
        if (mapped) {
            uint32_t idx = bits == 16 ? u16le(p) : p[0];
            if (idx < cmfirst || idx - cmfirst >= cmlen) {
                o[0] = o[1] = o[2] = 0;
                o[3] = 255;
                continue;
            }
            color(map + (idx - cmfirst) * mapsz, (int)cmbits, false, alpha, o);
        } else {
            memcpy(one, p, psz);
            color(one, bits, gray, alpha, o);
        }
    }
    if (!ok) {
        free(px);
        return NULL;
    }

    /* an alpha channel that is 0 everywhere is unused (common in 32-bit files with no alpha bits) */
    if (alpha && !abits) {
        bool any = false;
        for (uint32_t i = 0; i < total && !any; i++) any = px[i * 4 + 3] != 0;
        if (!any)
            for (uint32_t i = 0; i < total; i++) px[i * 4 + 3] = 255;
    }

    /* to top-to-bottom, left-to-right */
    if (desc & 0x10)
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * w * 4);
            for (int a = 0, b = w - 1; a < b; a++, b--) {
                uint32_t t = row[a];
                row[a] = row[b];
                row[b] = t;
            }
        }
    if (!(desc & 0x20)) {
        uint8_t *tmp = malloc((size_t)w * 4);
        if (tmp)
            for (int a = 0, b = h - 1; a < b; a++, b--) {
                memcpy(tmp, px + (size_t)a * w * 4, (size_t)w * 4);
                memcpy(px + (size_t)a * w * 4, px + (size_t)b * w * 4, (size_t)w * 4);
                memcpy(px + (size_t)b * w * 4, tmp, (size_t)w * 4);
            }
        free(tmp);
    }
    *wp = w;
    *hp = h;
    return px;
}

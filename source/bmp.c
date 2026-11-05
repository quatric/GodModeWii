/*
 * Windows bitmap decoding. See bmp.h.
 *
 * File header (little-endian): "BM", file size, two reserved u16, pixel data offset. Then the info
 * header: its size (12 for OS/2 BITMAPCOREHEADER: u16 width, height, planes, bits; 40 and up: s32
 * width, height (negative: top-down), u16 planes, bits, u32 compression, image size, resolution x / y,
 * colors used, important; 52+ adds the R / G / B masks, 56+ the alpha mask). BI_BITFIELDS keeps its
 * three masks after a 40-byte header. The palette follows the header: 3-byte entries for OS/2, else 4
 * (B, G, R, unused). Rows are padded to 4 bytes.
 */
#include <stdlib.h>
#include <string.h>

#include "bmp.h"

#define BMP_MAX 4096

enum { BI_RGB = 0, BI_RLE8 = 1, BI_RLE4 = 2, BI_BITFIELDS = 3, BI_ALPHABITFIELDS = 6 };

static uint32_t u16le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32le(const uint8_t *p) { return u16le(p) | u16le(p + 2) << 16; }

typedef struct {
    int w, h;
    bool topdown, core;
    int bits, comp;
    uint32_t hsize, data, ncolors;
} hdr_t;

static bool parse(const uint8_t *d, uint32_t n, uint64_t size, hdr_t *o)
{
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return false;
    memset(o, 0, sizeof(*o));
    o->data = u32le(d + 10);
    o->hsize = u32le(d + 14);
    if (o->hsize == 12) {
        o->core = true;
        o->w = (int)u16le(d + 18);
        o->h = (int)u16le(d + 20);
        o->bits = (int)u16le(d + 24);
    } else if (o->hsize >= 40 && o->hsize <= 124 && n >= 14 + 40) {
        int32_t hh = (int32_t)u32le(d + 22);
        o->w = (int32_t)u32le(d + 18);
        o->topdown = hh < 0;
        o->h = hh < 0 ? -hh : hh;
        o->bits = (int)u16le(d + 28);
        o->comp = (int)u32le(d + 30);
        o->ncolors = u32le(d + 46);
    } else
        return false;
    if (o->w <= 0 || o->h <= 0 || o->w > BMP_MAX || o->h > BMP_MAX) return false;
    switch (o->bits) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: break;
    default: return false;
    }
    switch (o->comp) {
    case BI_RGB: break;
    case BI_RLE8: if (o->bits != 8 || o->topdown) return false; break;
    case BI_RLE4: if (o->bits != 4 || o->topdown) return false; break;
    case BI_BITFIELDS: case BI_ALPHABITFIELDS: if (o->bits != 16 && o->bits != 32) return false; break;
    default: return false;                      /* JPEG / PNG inside a BMP are not taken */
    }
    if (o->data < 14 + o->hsize || o->data >= size) return false;
    if (o->comp != BI_RLE8 && o->comp != BI_RLE4 && (uint64_t)o->data + (uint64_t)((o->w * o->bits + 31) / 32 * 4) * o->h > size)
        return false;
    return true;
}

bool bmp_header_ok(const uint8_t *h, uint32_t n, uint64_t size)
{
    hdr_t hd;
    return parse(h, n, size, &hd);
}

/* A mask's shift and width, to scale its field to 8 bits. */
static void mask_info(uint32_t m, int *shift, int *bits)
{
    *shift = *bits = 0;
    if (!m) return;
    while (!(m & 1)) { m >>= 1; (*shift)++; }
    while (m & 1) { m >>= 1; (*bits)++; }
}

static uint8_t field(uint32_t v, uint32_t m, int shift, int bits)
{
    if (!bits) return 255;
    uint32_t x = (v & m) >> shift, max = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    return (uint8_t)((uint64_t)x * 255 / max);
}

uint8_t *bmp_decode(const uint8_t *d, uint32_t n, int *wp, int *hp)
{
    hdr_t hd;
    if (!parse(d, n, n, &hd)) return NULL;
    int w = hd.w, h = hd.h, bits = hd.bits;

    /* the palette */
    uint8_t pal[256][4];
    memset(pal, 0, sizeof(pal));
    for (int i = 0; i < 256; i++) pal[i][3] = 255;
    if (bits <= 8) {
        uint32_t esz = hd.core ? 3 : 4, at = 14 + hd.hsize, max = 1u << bits;
        if (hd.comp == BI_BITFIELDS && hd.hsize == 40) at += 12;
        uint32_t cnt = hd.ncolors && hd.ncolors < max ? hd.ncolors : max;
        if (at + cnt * esz > hd.data) cnt = at < hd.data ? (hd.data - at) / esz : 0;
        for (uint32_t i = 0; i < cnt; i++) {
            pal[i][0] = d[at + i * esz + 2];
            pal[i][1] = d[at + i * esz + 1];
            pal[i][2] = d[at + i * esz];
        }
    }

    /* the masks */
    uint32_t mr = 0, mg = 0, mb = 0, ma = 0;
    if (hd.comp == BI_BITFIELDS || hd.comp == BI_ALPHABITFIELDS) {
        uint32_t at = 14 + 40;                  /* in the V2+ header, or right after the 40-byte one */
        if ((uint64_t)at + 12 > n) return NULL;
        mr = u32le(d + at);
        mg = u32le(d + at + 4);
        mb = u32le(d + at + 8);
        if ((hd.hsize >= 56 || hd.comp == BI_ALPHABITFIELDS) && (uint64_t)at + 16 <= n) ma = u32le(d + at + 12);
    } else if (bits == 16) {
        mr = 0x7C00; mg = 0x03E0; mb = 0x001F;
    } else if (bits == 32) {
        mr = 0x00FF0000; mg = 0x0000FF00; mb = 0x000000FF;
        if (hd.hsize >= 56) ma = u32le(d + 14 + 52);
    }
    int sr, br, sg, bg, sb, bb, sa, ba;
    mask_info(mr, &sr, &br);
    mask_info(mg, &sg, &bg);
    mask_info(mb, &sb, &bb);
    mask_info(ma, &sa, &ba);

    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    /* row r of the file is picture row y */
#define ROW(r) (hd.topdown ? (r) : h - 1 - (r))

    if (hd.comp == BI_RLE8 || hd.comp == BI_RLE4) {
        for (int i = 0; i < w * h; i++) memcpy(px + i * 4, pal[0], 4);   /* skipped pixels: color 0 */
        uint32_t at = hd.data;
        int x = 0, r = 0;
        bool rle4 = hd.comp == BI_RLE4;
        while (at + 1 < n && r < h) {
            int c = d[at], v = d[at + 1];
            at += 2;
            if (c) {                            /* a run of c pixels */
                for (int k = 0; k < c && x < w; k++, x++) {
                    int idx = rle4 ? (k & 1 ? v & 15 : v >> 4) : v;
                    memcpy(px + ((size_t)ROW(r) * w + x) * 4, pal[idx], 4);
                }
            } else if (v == 0) {                /* end of line */
                x = 0;
                r++;
            } else if (v == 1)                  /* end of bitmap */
                break;
            else if (v == 2) {                  /* move right / up */
                if (at + 1 >= n) break;
                x += d[at];
                r += d[at + 1];
                at += 2;
            } else {                            /* v pixels as they are, padded to 2 bytes */
                uint32_t len = rle4 ? (v + 1) / 2 : v;
                if ((uint64_t)at + len > n) break;
                for (int k = 0; k < v && x < w; k++, x++) {
                    int idx = rle4 ? (k & 1 ? d[at + k / 2] & 15 : d[at + k / 2] >> 4) : d[at + k];
                    memcpy(px + ((size_t)ROW(r) * w + x) * 4, pal[idx], 4);
                }
                at += (len + 1) & ~1u;
            }
        }
    } else {
        uint32_t stride = (uint32_t)(w * bits + 31) / 32 * 4;
        bool any_alpha = false;
        for (int r = 0; r < h; r++) {
            const uint8_t *row = d + hd.data + (size_t)r * stride;
            uint8_t *o = px + (size_t)ROW(r) * w * 4;
            for (int x = 0; x < w; x++, o += 4) {
                if (bits <= 8) {
                    int bitpos = x * bits;
                    int idx = row[bitpos >> 3] >> (8 - bits - (bitpos & 7)) & ((1 << bits) - 1);
                    memcpy(o, pal[idx], 4);
                } else if (bits == 24) {
                    o[0] = row[x * 3 + 2];
                    o[1] = row[x * 3 + 1];
                    o[2] = row[x * 3];
                    o[3] = 255;
                } else {
                    uint32_t v = bits == 16 ? u16le(row + x * 2) : u32le(row + x * 4);
                    o[0] = field(v, mr, sr, br);
                    o[1] = field(v, mg, sg, bg);
                    o[2] = field(v, mb, sb, bb);
                    if (ma) o[3] = field(v, ma, sa, ba);
                    else if (bits == 32 && hd.comp == BI_RGB) {
                        o[3] = row[x * 4 + 3];  /* often alpha, often junk: used when not all 0 */
                        any_alpha |= o[3] != 0;
                    } else
                        o[3] = 255;
                }
            }
        }
        if (bits == 32 && !ma && hd.comp == BI_RGB && !any_alpha)
            for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    }
#undef ROW
    *wp = w;
    *hp = h;
    return px;
}

/*
 * Photoshop .psd / .psb merged-image reader. See psd.h.
 *
 * The file is: header, color mode data, image resources, layer and mask info, then the composite as
 * planar channels (all of channel 0, then channel 1 ...), raw or PackBits-compressed per scanline.
 * Rows are decoded one at a time into a byte-per-channel RGBA buffer that is converted in place, so
 * the only big allocation is the picture itself.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "psd.h"

static inline uint16_t b16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t b32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

typedef struct {
    int version, channels, depth, mode;
    uint32_t w, h;
    uint32_t pal_at, pal_len;           /* indexed: the 768-byte palette */
    uint64_t layer_at;                  /* layer and mask info section (its length field) */
    uint64_t data_at;                   /* the compression field of the composite */
    bool alpha;                         /* the first extra channel is the composite's transparency */
} psd_t;

bool psd_header_ok(const uint8_t *h, uint32_t n)
{
    if (n < 26 || memcmp(h, "8BPS", 4)) return false;
    int ver = b16(h + 4);
    return ver == 1 || ver == 2;
}

static bool parse(const uint8_t *d, uint32_t n, psd_t *p)
{
    memset(p, 0, sizeof(*p));
    if (!psd_header_ok(d, n)) return false;
    p->version = b16(d + 4);
    p->channels = b16(d + 12);
    p->h = b32(d + 14);
    p->w = b32(d + 18);
    p->depth = b16(d + 22);
    p->mode = b16(d + 24);
    uint32_t maxdim = p->version == 2 ? 300000 : 30000;
    if (p->channels < 1 || p->channels > 56 || !p->w || !p->h || p->w > maxdim || p->h > maxdim) return false;
    if (p->depth != 1 && p->depth != 8 && p->depth != 16) return false;

    uint64_t pos = 26;
    if (pos + 4 > n) return false;
    uint32_t cm = b32(d + pos);
    pos += 4;
    if (p->mode == 2 && cm == 768 && pos + 768 <= n) { p->pal_at = (uint32_t)pos; p->pal_len = 768; }
    pos += cm;
    if (pos + 4 > n) return false;
    pos += 4 + (uint64_t)b32(d + pos);                      /* image resources */
    if (pos + (p->version == 2 ? 8 : 4) > n) return false;
    uint64_t li = p->version == 2 ? ((uint64_t)b32(d + pos) << 32 | b32(d + pos + 4)) : b32(d + pos);
    pos += p->version == 2 ? 8 : 4;
    p->layer_at = pos;
    /* a negative layer count means the first alpha channel holds the composite's transparency:
     * the layer info inside the section starts with its own length, then the signed count */
    {
        uint64_t lsz = p->version == 2 ? 8 : 4;
        if (li > lsz + 2 && pos + lsz + 2 <= n) {
            uint64_t sub = p->version == 2 ? ((uint64_t)b32(d + pos) << 32 | b32(d + pos + 4)) : b32(d + pos);
            if (sub >= 2) p->alpha = (int16_t)b16(d + pos + lsz) < 0;
        }
    }
    pos += li;
    if (pos + 2 > n) return false;
    p->data_at = pos;
    return true;
}

/* PackBits: one row of `want` bytes from `src` (len bytes); returns false on a damaged row. */
static bool unpack(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t want)
{
    uint32_t o = 0, i = 0;
    while (o < want && i < len) {
        int8_t c = (int8_t)src[i++];
        if (c >= 0) {
            uint32_t run = (uint32_t)c + 1;
            if (i + run > len) run = len - i;
            if (o + run > want) run = want - o;
            memcpy(dst + o, src + i, run);
            i += run;
            o += run;
        } else if (c != -128) {
            uint32_t run = (uint32_t)(1 - c);
            if (i >= len) break;
            uint8_t v = src[i++];
            if (o + run > want) run = want - o;
            memset(dst + o, v, run);
            o += run;
        }
    }
    if (o < want) memset(dst + o, 0, want - o);
    return o == want;
}

static uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

static void lab_to_rgb(int L8, int a8, int b8, uint8_t *o)
{
    float L = L8 * 100.0f / 255.0f, a = (float)a8 - 128.0f, b = (float)b8 - 128.0f;
    float fy = (L + 16.0f) / 116.0f, fx = fy + a / 500.0f, fz = fy - b / 200.0f;
    float x3 = fx * fx * fx, y3 = fy * fy * fy, z3 = fz * fz * fz;
    float X = (x3 > 0.008856f ? x3 : (fx - 16.0f / 116.0f) / 7.787f) * 0.9642f;
    float Y = y3 > 0.008856f ? y3 : (fy - 16.0f / 116.0f) / 7.787f;
    float Z = (z3 > 0.008856f ? z3 : (fz - 16.0f / 116.0f) / 7.787f) * 0.8249f;   /* D50 */
    float r = 3.1339f * X - 1.6169f * Y - 0.4906f * Z;
    float g = -0.9788f * X + 1.9161f * Y + 0.0335f * Z;
    float bl = 0.0719f * X - 0.2290f * Y + 1.4052f * Z;
    float c[3] = { r, g, bl };
    for (int i = 0; i < 3; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
        o[i] = clamp8(v * 255.0f);
    }
}

uint8_t *psd_decode(const uint8_t *d, uint32_t n, int max_dim, int *ow, int *oh)
{
    psd_t p;
    if (!parse(d, n, &p)) return NULL;
    int mode = p.mode;
    /* the channels each mode needs for its picture */
    int need;
    switch (mode) {
    case 0: case 1: case 2: case 8: need = 1; break;
    case 3: case 9: need = 3; break;
    case 4: need = 4; break;
    default: return NULL;                                   /* multichannel and the rest */
    }
    if (p.channels < need) return NULL;
    if (mode == 2 && !p.pal_len) return NULL;
    if (p.depth == 1 && mode != 0 && mode != 1) return NULL;
    bool has_alpha = p.alpha && p.channels > need && (mode == 1 || mode == 3 || mode == 8 || mode == 9) && p.depth >= 8;
    int use = need + (has_alpha ? 1 : 0);

    uint32_t step = 1;
    while ((p.w + step - 1) / step > (uint32_t)max_dim || (p.h + step - 1) / step > (uint32_t)max_dim) step++;
    uint32_t w = (p.w + step - 1) / step, h = (p.h + step - 1) / step;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return NULL;
    memset(rgba, 255, (size_t)w * h * 4);

    uint32_t comp = b16(d + p.data_at);
    if (comp > 1) { free(rgba); return NULL; }              /* ZIP: not read */
    uint64_t pos = p.data_at + 2;
    size_t bpc = p.depth == 16 ? 2 : 1;
    uint32_t rowbytes = p.depth == 1 ? (p.w + 7) / 8 : (uint32_t)(p.w * bpc);
    size_t cnt_sz = p.version == 2 ? 4 : 2;
    uint64_t counts_at = pos;
    uint64_t data = pos + (comp == 1 ? (uint64_t)p.channels * p.h * cnt_sz : 0);
    if (data > n) { free(rgba); return NULL; }
    uint8_t *row = malloc(rowbytes + 16);
    if (!row) { free(rgba); return NULL; }

    /* where each channel starts: raw channels are rowbytes * h apart, RLE ones follow the previous row sums */
    uint64_t chan_at = data;
    for (int c = 0; c < p.channels; c++) {
        bool wanted = c < use;
        uint64_t at = chan_at;
        for (uint32_t y = 0; y < p.h; y++) {
            const uint8_t *rp = NULL;
            uint32_t rl = 0;
            if (comp == 0) {
                if (at + rowbytes > n) { memset(row, 0, rowbytes); }
                else { rp = d + at; }
                at += rowbytes;
            } else {
                uint64_t ci = counts_at + ((uint64_t)c * p.h + y) * cnt_sz;
                if (ci + cnt_sz > n) { free(row); free(rgba); return NULL; }
                rl = cnt_sz == 4 ? b32(d + ci) : b16(d + ci);
                if (at + rl <= n && wanted && y % step == 0) {
                    if (!unpack(d + at, rl, row, rowbytes)) { /* keep what came out */ }
                    rp = row;
                } else if (wanted && y % step == 0) {
                    memset(row, 0, rowbytes);
                    rp = row;
                }
                at += rl;
            }
            if (!wanted || y % step) continue;
            if (!rp) rp = row;
            uint8_t *out = rgba + (size_t)(y / step) * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t sx = x * step;
                int v;
                if (p.depth == 1) v = (rp[sx >> 3] >> (7 - (sx & 7))) & 1 ? 0 : 255;    /* 1 is black */
                else if (p.depth == 16) v = rp[sx * 2];
                else v = rp[sx];
                out[x * 4 + c] = (uint8_t)v;
            }
        }
        chan_at = at;
    }
    free(row);

    /* the channels sit in rgba[0..use) of every pixel: turn them into colors */
    size_t px = (size_t)w * h;
    const uint8_t *pal = p.pal_len ? d + p.pal_at : NULL;
    for (size_t i = 0; i < px; i++) {
        uint8_t *o = rgba + i * 4;
        uint8_t a = has_alpha ? o[need] : 255;
        switch (mode) {
        case 0: case 1: case 8: {
            uint8_t g = o[0];
            o[0] = o[1] = o[2] = g;
            break;
        }
        case 2: {
            uint8_t k = o[0];
            o[0] = pal[k]; o[1] = pal[256 + k]; o[2] = pal[512 + k];
            break;
        }
        case 3: break;
        case 4: {
            /* Photoshop stores CMYK inverted: 255 is no ink */
            int k = o[3];
            o[0] = (uint8_t)(o[0] * k / 255);
            o[1] = (uint8_t)(o[1] * k / 255);
            o[2] = (uint8_t)(o[2] * k / 255);
            break;
        }
        case 9: {
            uint8_t t[3];
            lab_to_rgb(o[0], o[1], o[2], t);
            o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
            break;
        }
        }
        o[3] = a;
    }
    *ow = (int)w;
    *oh = (int)h;
    return rgba;
}

/*
 * Monster Games textures (Excite Truck, Excitebots: Trick Racing), after quatric's nintoolbox
 * (lib-excite.c, itself ported from the ExciteExtract research tool). See excite.h.
 *
 *   .tex / .art / .img with a header (Excitebots): u16 width, u16 height (little endian), u8 levels,
 *        u8 renderer code 0x40 - 0x4F, pixels at 0x80. 0x40 / 0x41 are I4 / IA4 chains plus a
 *        1024-byte tail, 0x44 CMPR (optionally followed by a second chain); for the others the format
 *        is the one whose chain fills the file exactly, best by mip_score / seam_score.
 *   .tex without one (Excite Truck): raw GX mips and a 128-byte footer that may hold the size; the
 *        format is never stored, so every format / size whose chain fills the file is decoded and the
 *        one whose level 1 matches level 0 shrunk by half wins (mip_score), else the one whose GX
 *        tiles join up best (seam_score), else the smoothest. Best effort: ExciteExtract misses ~2 %.
 *   .art / .img without one: 2^k bytes of one level and 128 zero bytes; seam_score picks; an image
 *        twice as tall as wide whose bottom half is a stencil is folded into one with alpha.
 *   .tm0: the Excitebots header at 0x80, a CMPR chain at 0x100, then (code 0x44) an I4 stencil chain
 *        128 bytes short of full, which is the alpha.
 *
 * Pixels come out of image_decode_gx (big endian GX data, read byte by byte).
 */
#include <stdlib.h>
#include <string.h>

#include "excite.h"
#include "media.h"

enum { XF_I4 = 0, XF_I8 = 1, XF_IA4 = 2, XF_IA8 = 3, XF_RGB565 = 4, XF_RGB5A3 = 5, XF_RGBA8 = 6, XF_CMPR = 14 };

static const u8 cands_fmt[] = { XF_CMPR, XF_I8, XF_IA4, XF_IA8, XF_RGB5A3, XF_RGB565, XF_I4, XF_RGBA8 };
#define NFMT (sizeof(cands_fmt) / sizeof(cands_fmt[0]))
static const u32 dims[] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
#define NDIM (sizeof(dims) / sizeof(dims[0]))

static u16 le16p(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static bool is_dim(u32 v)
{
    for (u32 i = 0; i < NDIM; i++)
        if (dims[i] == v) return true;
    return false;
}

static void tile(u8 f, u32 *bw, u32 *bh)
{
    if (f == XF_I4 || f == XF_CMPR) *bw = 8, *bh = 8;
    else if (f == XF_I8 || f == XF_IA4) *bw = 8, *bh = 4;
    else *bw = 4, *bh = 4;
}

static u32 lvl(u8 f, u32 w, u32 h) { return image_gx_size(f, (int)w, (int)h); }

static u64 chain(u8 f, u32 w, u32 h, u32 levels)
{
    u64 t = 0;
    for (u32 i = 0; i < levels; i++) t += lvl(f, w >> i ? w >> i : 1, h >> i ? h >> i : 1);
    return t;
}

/* image_decode_gx keeps a transparent CMPR texel's averaged colour; the scores here (and nintoolbox's)
 * count it as black, which is what tells a stencil half apart */
static u8 *dec(u8 f, u32 w, u32 h, const u8 *d, u32 n)
{
    u8 *px = image_decode_gx(d, n, f, (int)w, (int)h, NULL, 0, 0);
    if (px && f == XF_CMPR)
        for (u32 i = 0; i < w * h; i++)
            if (!px[i * 4 + 3]) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
    return px;
}

/* up to 3: how many different pixels a coarse sample sees */
static u32 distinct(const u8 *px, u32 npx)
{
    u32 seen = 0, step = npx / 4096 > 1 ? npx / 4096 : 1;
    const u8 *first = NULL;
    for (u32 i = 0; i < npx && seen < 3; i += step) {
        const u8 *p = px + (size_t)i * 4;
        if (!first) {
            first = p;
            seen = 1;
        } else if (memcmp(p, first, 4))
            seen++;
    }
    return seen;
}

static double mip_score(const u8 *d, u32 n, u8 f, u32 w, u32 h)
{
    if (w < 16 || h < 16) return -1;
    u32 l0 = lvl(f, w, h), l1 = lvl(f, w / 2, h / 2);
    if (!l0 || !l1 || (u64)l0 + l1 > n) return -1;
    u8 *a = dec(f, w, h, d, l0), *c = dec(f, w / 2, h / 2, d + l0, l1);
    double r = -1;
    if (a && c && distinct(a, w * h) >= 3 && distinct(c, (w / 2) * (h / 2)) >= 2) {
        u32 hw = w / 2, hh = h / 2, sy = hh / 32 > 1 ? hh / 32 : 1, sx = hw / 32 > 1 ? hw / 32 : 1, cnt = 0;
        u64 tot = 0;
        for (u32 y = 0; y < hh; y += sy)
            for (u32 x = 0; x < hw; x += sx) {
                u32 acc[4] = { 0, 0, 0, 0 };
                for (u32 dy = 0; dy < 2; dy++)
                    for (u32 dx = 0; dx < 2; dx++) {
                        const u8 *p = a + ((size_t)(y * 2 + dy) * w + (x * 2 + dx)) * 4;
                        for (int k = 0; k < 4; k++) acc[k] += p[k];
                    }
                const u8 *q = c + ((size_t)y * hw + x) * 4;
                for (int k = 0; k < 4; k++) tot += acc[k] / 4 > q[k] ? acc[k] / 4 - q[k] : q[k] - acc[k] / 4;
                cnt += 4;
            }
        r = cnt ? (double)tot / cnt : -1;
    }
    free(a);
    free(c);
    return r;
}

static u32 delta(const u8 *px, u32 w, u32 x0, u32 y0, u32 x1, u32 y1)
{
    const u8 *a = px + ((size_t)y0 * w + x0) * 4, *b = px + ((size_t)y1 * w + x1) * 4;
    u32 s = 0;
    for (int k = 0; k < 4; k++) s += a[k] > b[k] ? a[k] - b[k] : b[k] - a[k];
    return s;
}

static double seam_score(const u8 *px, u32 w, u32 h, u32 bw, u32 bh)
{
    u64 es = 0, is = 0;
    u32 en = 0, in = 0;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = bw - 1; x + 1 < w; x += bw) es += delta(px, w, x, y, x + 1, y), en++;
        for (u32 x = 0; x + 1 < w; x += bw)
            if ((x + 1) % bw) is += delta(px, w, x, y, x + 1, y), in++;
    }
    for (u32 x = 0; x < w; x++) {
        for (u32 y = bh - 1; y + 1 < h; y += bh) es += delta(px, w, x, y, x, y + 1), en++;
        for (u32 y = 0; y + 1 < h; y += bh)
            if ((y + 1) % bh) is += delta(px, w, x, y, x, y + 1), in++;
    }
    if (!en || !in) return -1;
    double e = (double)es / en, i = (double)is / in;
    return ((e + 1.0) / (i + 1.0)) * (1.0 + i / 24.0);
}

static double smoothness(const u8 *px, u32 w, u32 h)
{
    u64 tot = 0;
    u32 n = 0, sy = h / 64 > 1 ? h / 64 : 1, sx = w / 64 > 1 ? w / 64 : 1;
    for (u32 y = 0; y + 1 < h; y += sy)
        for (u32 x = 0; x + 1 < w; x += sx) {
            tot += delta(px, w, x, y, x + 1, y) + delta(px, w, x, y, x, y + 1);
            n += 8;
        }
    return n ? (double)tot / n : 1e18;
}

typedef struct {
    u8 fmt;
    u32 w, h, dl;
} cand_t;

static bool find_footer(const u8 *d, u32 n, u32 *hoff, u32 *w, u32 *h)
{
    static const int offs[] = { -64, -32, -128, -96 };
    for (u32 i = 0; i < 4; i++) {
        if ((int)n + offs[i] < 0) continue;
        u32 ho = n + offs[i], fw = le16p(d + ho), fh = le16p(d + ho + 2);
        if (!is_dim(fw) || !is_dim(fh)) continue;
        u32 bl = 0;
        for (u32 t = fw; t; t >>= 1) bl++;
        if (d[ho + 4] == bl - 3) {
            *hoff = ho;
            *w = fw;
            *h = fh;
            return true;
        }
    }
    return false;
}

static u8 *done(u8 f, u32 w, u32 h, const u8 *d, u32 n, int *ow, int *oh)
{
    u8 *px = dec(f, w, h, d, n);
    if (px) *ow = (int)w, *oh = (int)h;
    return px;
}

static u8 *classify(const u8 *d, u32 n, bool want_footer, int *ow, int *oh)
{
    u32 hoff = 0, fw = 0, fh = 0;
    bool foot = want_footer && find_footer(d, n, &hoff, &fw, &fh);
    u32 dl = foot ? hoff : n >= 128 ? n - 128 : 0;
    if (!dl) return NULL;

    static cand_t c[256];
    u32 nc = 0;
#define ADD(f_, w_, h_, d_)                                                                       \
    do {                                                                                          \
        bool dup = false;                                                                         \
        for (u32 k = 0; k < nc; k++)                                                              \
            if (c[k].fmt == (f_) && c[k].w == (w_) && c[k].h == (h_) && c[k].dl == (d_)) dup = true; \
        if (!dup && nc < 256) c[nc++] = (cand_t){ (f_), (w_), (h_), (d_) };                       \
    } while (0)
    if (foot)
        for (u32 i = 0; i < NFMT; i++)
            if (lvl(cands_fmt[i], fw, fh) <= hoff) ADD(cands_fmt[i], fw, fh, hoff);
    for (u32 i = 0; i < NFMT; i++)
        for (u32 wi = 0; wi < NDIM; wi++)
            for (u32 hi = 0; hi < NDIM; hi++)
                for (u32 lv = 1; lv <= 10; lv++)
                    if (chain(cands_fmt[i], dims[wi], dims[hi], lv) == dl) {
                        ADD(cands_fmt[i], dims[wi], dims[hi], dl);
                        break;
                    }
#undef ADD

    double best = 1e18;
    int bi = -1;
    for (u32 i = 0; i < nc; i++) {                                   /* 1: mip chain agrees */
        double m = mip_score(d, c[i].dl, c[i].fmt, c[i].w, c[i].h);
        if (m >= 0 && (bi < 0 || m < best)) best = m, bi = (int)i;
    }
    if (bi >= 0 && best < 12.0) return done(c[bi].fmt, c[bi].w, c[bi].h, d, c[bi].dl, ow, oh);

    bi = -1;
    best = 1e18;
    for (u32 i = 0; i < nc; i++) {                                   /* 2: GX tiles join up */
        u8 *px = dec(c[i].fmt, c[i].w, c[i].h, d, c[i].dl);
        if (!px) continue;
        if (distinct(px, c[i].w * c[i].h) < 3) {
            free(px);
            continue;
        }
        u32 bw, bh;
        tile(c[i].fmt, &bw, &bh);
        double s = seam_score(px, c[i].w, c[i].h, bw, bh);
        if (s < 0) s = smoothness(px, c[i].w, c[i].h);
        double ar = (double)(c[i].w > c[i].h ? c[i].w : c[i].h) / (c[i].w < c[i].h ? c[i].w : c[i].h);
        if (ar >= 8) s *= 1.0 + 0.35 * (ar / 8.0);
        free(px);
        if (bi < 0 || s < best) best = s, bi = (int)i;
    }
    if (bi >= 0) return done(c[bi].fmt, c[bi].w, c[bi].h, d, c[bi].dl, ow, oh);

    bi = -1;
    best = 1e18;
    for (u32 i = 0; i < nc; i++) {                                   /* 3: smoothest */
        u8 *px = dec(c[i].fmt, c[i].w, c[i].h, d, c[i].dl);
        if (!px) continue;
        double s = smoothness(px, c[i].w, c[i].h);
        free(px);
        if (bi < 0 || s < best) best = s, bi = (int)i;
    }
    return bi >= 0 ? done(c[bi].fmt, c[bi].w, c[bi].h, d, c[bi].dl, ow, oh) : NULL;
}

/* Excitebots' 128-byte header (shared by .tex, .art and .img) */
static u8 *with_header(const u8 *d, u32 n, int *ow, int *oh)
{
    if (n <= 128) return NULL;
    u32 w = le16p(d), h = le16p(d + 2), levels = d[5] == 0x44 && d[4] == 2 ? 1 : d[4];
    if (!is_dim(w) || !is_dim(h) || levels < 1 || levels > 10 || d[5] < 0x40 || d[5] > 0x4F) return NULL;
    const u8 *px = d + 128;
    u32 ps = n - 128;
    if (d[5] == 0x40 || d[5] == 0x41) {
        u8 f = d[5] == 0x40 ? XF_I4 : XF_IA4;
        return chain(f, w, h, levels) + 1024 == ps ? done(f, w, h, px, ps, ow, oh) : NULL;
    }
    if (d[5] == 0x44) {
        u64 one = chain(XF_CMPR, w, h, levels);
        if (one * 2 == ps || one == ps) return done(XF_CMPR, w, h, px, ps, ow, oh);
    }
    double best = 1e18;
    int bf = -1;
    for (u32 i = 0; i < NFMT; i++) {
        u8 f = cands_fmt[i];
        if (chain(f, w, h, levels) != ps) continue;
        double s = levels > 1 ? mip_score(px, ps, f, w, h) : -1;
        if (s < 0) {
            u8 *p = dec(f, w, h, px, ps);
            if (!p) continue;
            u32 bw, bh;
            tile(f, &bw, &bh);
            s = seam_score(p, w, h, bw, bh);
            if (s < 0) s = smoothness(p, w, h);
            free(p);
        }
        if (bf < 0 || s < best) best = s, bf = f;
    }
    return bf >= 0 ? done((u8)bf, w, h, px, ps, ow, oh) : NULL;
}

static bool looks_like_mask(const u8 *px, u32 w, u32 h)
{
    u32 half = w * (h / 2), total = w * h, step = (total - half) / 4096 > 1 ? (total - half) / 4096 : 1;
    u32 sampled = 0, dark = 0, lo = 0, hi = 0;
    for (u32 i = half; i < total; i += step) {
        const u8 *p = px + (size_t)i * 4;
        if (((u32)p[0] + p[1] + p[2]) / 3 < 24) dark++;
        if (p[3] < 128) lo++;
        else hi++;
        sampled++;
    }
    if (!sampled) return false;
    u32 amin = lo < hi ? lo : hi;
    return (double)dark / sampled > 0.90 && (double)amin / sampled > 0.02;
}

u8 *excite_tex(const u8 *d, u32 n, int *w, int *h)
{
    if (n < 64) return NULL;
    u8 *px = with_header(d, n, w, h);
    return px ? px : classify(d, n, true, w, h);
}

u8 *excite_art(const u8 *d, u32 n, int *w, int *h)
{
    if (n <= 128) return NULL;
    u8 *px = with_header(d, n, w, h);
    if (px) return px;
    u32 dl = n - 128;
    if (!dl || (dl & (dl - 1))) return NULL;
    for (u32 i = n - 128; i < n; i++)
        if (d[i]) return NULL;
    px = classify(d, n, false, w, h);
    if (px && *h == 2 * *w && looks_like_mask(px, (u32)*w, (u32)*h)) {       /* colour over stencil */
        u32 W = (u32)*w, hh = (u32)*h / 2;
        for (u32 y = 0; y < hh; y++)
            for (u32 x = 0; x < W; x++) {
                u8 *c = px + ((size_t)y * W + x) * 4;
                c[3] = 255 - px[((size_t)(y + hh) * W + x) * 4 + 3];
            }
        *h = (int)hh;
    }
    return px;
}

u8 *excite_tm0(const u8 *d, u32 n, int *w, int *h)
{
    if (n <= 0x100) return NULL;
    const u8 *hd = d + 0x80;
    u32 W = le16p(hd), H = le16p(hd + 2), levels = hd[4], code = hd[5];
    if (!is_dim(W) || !is_dim(H) || levels < 1 || levels > 10 || code < 0x40 || code > 0x4F) return NULL;
    u32 ps = n - 0x100;
    u64 full = chain(XF_CMPR, W, H, levels);
    if (full <= 128) return NULL;
    u64 tail = full - 128;
    int chains = ps == full + tail ? 2 : ps == tail ? 1 : 0;
    if (!chains) return NULL;
    u8 *px = dec(XF_CMPR, W, H, d + 0x100, ps);
    if (!px) return NULL;
    if (chains == 2) {
        u8 *m = dec(XF_I4, W, H, d + 0x100 + full, (u32)tail);
        if (!m) {
            free(px);
            return NULL;
        }
        for (u32 i = 0; i < W * H; i++) px[4 * i + 3] = m[4 * i];
        free(m);
    }
    *w = (int)W;
    *h = (int)H;
    return px;
}

bool excite_head_ok(const u8 *d, u32 n)
{
    if (n < 6) return false;
    u32 w = le16p(d), h = le16p(d + 2);
    return is_dim(w) && is_dim(h) && d[4] >= 1 && d[4] <= 10 && d[5] >= 0x40 && d[5] <= 0x4F;
}

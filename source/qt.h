/*
 * QuickTime / MP4 atom helpers shared by the Motion-JPEG player and the M4A reader.
 */
#ifndef QT_H
#define QT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"                          /* be16 / be32 */

typedef struct { long off; uint32_t sz; } span;
typedef struct { span *v; int n, cap; } spans;

static inline int push(spans *s, long off, uint32_t sz)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        span *nv = realloc(s->v, nc * sizeof(span));
        if (!nv) return 0;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n].off = off;
    s->v[s->n++].sz = sz;
    return 1;
}

typedef struct { const uint8_t *p; uint32_t n; } blob;

/* the first child atom of type t inside [p, p + n) */
static inline blob atom(const uint8_t *p, uint32_t n, const char *t)
{
    blob r = { NULL, 0 };
    for (uint32_t o = 0; o + 8 <= n;) {
        uint32_t sz = be32(p + o);
        if (sz < 8 || sz > n - o) break;
        if (!memcmp(p + o + 4, t, 4)) {
            r.p = p + o + 8;
            r.n = sz - 8;
            return r;
        }
        o += sz;
    }
    return r;
}

/* the samples of one track: chunk offsets (stco / co64), samples per chunk (stsc), sizes (stsz) */
static inline int track_spans(blob stbl, spans *out, uint32_t *total_samples, uint32_t *const_sz)
{
    blob stsz = atom(stbl.p, stbl.n, "stsz"), stsc = atom(stbl.p, stbl.n, "stsc");
    blob co = atom(stbl.p, stbl.n, "stco");
    int wide = 0;
    if (!co.p) {
        co = atom(stbl.p, stbl.n, "co64");
        wide = 1;
    }
    if (!stsz.p || !stsc.p || !co.p || stsz.n < 12 || stsc.n < 8 || co.n < 8) return 0;
    uint32_t fixed = be32(stsz.p + 4), ns = be32(stsz.p + 8), nsc = be32(stsc.p + 4), nco = be32(co.p + 4);
    if ((!fixed && stsz.n < 12 + (uint64_t)ns * 4) || stsc.n < 8 + (uint64_t)nsc * 12 ||
        co.n < 8 + (uint64_t)nco * (wide ? 8 : 4))
        return 0;
    *total_samples = ns;
    *const_sz = fixed;
    uint32_t s = 0, e = 0;                   /* sample index; stsc entry */
    for (uint32_t c = 0; c < nco; c++) {
        while (e + 1 < nsc && be32(stsc.p + 8 + (e + 1) * 12) <= c + 1) e++;
        uint32_t per = nsc ? be32(stsc.p + 8 + e * 12 + 4) : 0;
        long off = wide ? (long)be32(co.p + 8 + c * 8 + 4) : (long)be32(co.p + 8 + c * 4);
        if (fixed) {                          /* PCM: one span for the whole chunk */
            if (!push(out, off, per * fixed)) return 0;
            s += per;
            continue;
        }
        for (uint32_t i = 0; i < per && s < ns; i++, s++) {
            uint32_t sz = be32(stsz.p + 12 + s * 4);
            if (!push(out, off, sz)) return 0;
            off += sz;
        }
    }
    return 1;
}

#endif

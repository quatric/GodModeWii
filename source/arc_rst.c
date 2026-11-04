/*
 * Monster Games RST archives (Excite Truck, Excitebots: Trick Racing, Excitebike: World Rally), read-only,
 * after quatric's nintoolbox (lib-rst.c, extract/rst.inc).
 *
 *   header (0x80)  "RST0" (big endian fields) or "0TSR" (little endian); data offset at 0x18 (0 = 0x80),
 *                  file count at 0x20
 *   payload        at the data offset: either the files themselves, or "PMCr" / "rMCP" with the stream
 *                  length at 8 and a QuickLZ 1.4.0 stream at 16. Offsets below the decompressed size
 *                  point into it; the rest point into the raw bytes after the stream (big, mostly
 *                  incompressible files: streamed textures, music, sound).
 *   names          in the sibling "<name>.toc": "0SERCOTE" (little endian) / "ETOCRES0" (big endian) or
 *                  the compact version 3 form, 0x28-byte records {name offset, ?, ?, size, offset} and a
 *                  name pool; or, on WiiWare, that .toc is a member of a "tocres.res" RST next to it.
 *                  Excite Truck's older layout keeps 0x44-byte records {name[32], ..., size at 0x28,
 *                  absolute offset at 0x2C} in its .toc (after 0x28 bytes) or right after the header,
 *                  and a compressed tocres.res holds a compact directory with no .toc at all.
 *
 * The decompressed block stays in memory; files in the raw part are read from the archive on demand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archive.h"
#include "qlz.h"

#define RST_MAX_BLOCK (32u * 1024 * 1024)
#define RST_MAX_TABLE (8u * 1024 * 1024)

typedef struct {
    src_t f;
    bool be;
    uint32_t count, doff;
    uint8_t *pay;                   /* the QuickLZ block, decompressed (NULL when not compressed) */
    uint32_t plen;
    uint64_t tail_off, tail_len;    /* raw bytes: after the stream, or the whole payload */
} rst_t;

static uint32_t r32(bool be, const uint8_t *p) { return be ? be32(p) : le32(p); }

static void rst_free(rst_t *r) { free(r->pay); r->pay = NULL; }

static bool rst_head(const src_t *f, uint8_t h[0x80])
{
    return f->size >= 0x80 && src_read_full(f, 0, h, 0x80) &&
           (!memcmp(h, "RST0", 4) || !memcmp(h, "0TSR", 4));
}

static bool rst_load(rst_t *r, const src_t *f)
{
    uint8_t h[0x80];
    memset(r, 0, sizeof(*r));
    if (!rst_head(f, h)) return false;
    r->f = *f;
    r->be = !memcmp(h, "RST0", 4);
    r->count = r32(r->be, h + 0x20);
    if (!r->count || r->count > 20000) return false;
    r->doff = r32(r->be, h + 0x18);
    if (!r->doff || r->doff >= f->size) r->doff = 0x80;
    r->tail_off = r->doff;
    r->tail_len = f->size - r->doff;

    uint8_t m[16];
    if (r->tail_len >= 16 && src_read_full(f, r->doff, m, 16) &&
        (!memcmp(m, "PMCr", 4) || !memcmp(m, "rMCP", 4))) {
        uint64_t ql = r32(r->be, m + 8);
        if (!ql || ql > r->tail_len - 16) ql = r->tail_len - 16;
        if (ql > RST_MAX_BLOCK) {
            g_archive_err = "This RST's compressed block is too big for memory";
            return false;
        }
        uint8_t *z = malloc(ql ? (size_t)ql : 1);
        if (z && src_read_full(f, r->doff + 16, z, (uint32_t)ql))
            r->pay = qlz_unpack(z, (uint32_t)ql, RST_MAX_BLOCK, &r->plen);
        free(z);
        if (!r->pay) {
            g_archive_err = "Corrupt QuickLZ block";
            return false;
        }
        r->tail_off = r->doff + 16 + ql;
        r->tail_len = f->size - r->tail_off;
    }
    return true;
}

/* Where a TOC offset lands: region 0 = the decompressed block, 1 = the file (absolute). */
static bool rst_where(const rst_t *r, uint32_t foff, uint32_t fsize, uint32_t *region, uint64_t *at)
{
    uint64_t o = foff;
    if (r->pay && foff < r->plen) {
        if (fsize > r->plen - foff) return false;
        *region = 0;
        *at = foff;
        return true;
    }
    if (r->pay) o -= r->plen;
    if (fsize > r->tail_len || o > r->tail_len - fsize) return false;
    *region = 1;
    *at = r->tail_off + o;
    return true;
}

typedef void (*rst_add_fn)(void *ctx, const char *name, uint32_t size, uint32_t region, uint64_t at);

static void clean(char *out, size_t n, const char *src, size_t max)
{
    size_t k = 0;
    for (; k + 1 < n && k < max && src[k]; k++) {
        char c = src[k];
        out[k] = c == '\\' ? '/' : ((unsigned char)c < 0x20 || c == '|') ? '_' : c;
    }
    out[k] = 0;
}

/* A name field: printable up to its NUL, and not empty. */
static bool good_name(const uint8_t *s, size_t max)
{
    size_t k = 0;
    for (; k < max && s[k]; k++)
        if (s[k] < 0x20 || s[k] > 0x7E) return false;
    return k > 0;
}

/* Every file of r, named from toc (NULL when there is none). Excite Truck keeps its 0x44-byte records
 * right after the header only in tocres.res (`inline_ok`). Returns how many were found. */
static uint32_t rst_list(rst_t *r, const uint8_t *toc, uint32_t tsz, bool inline_ok, rst_add_fn add, void *ctx)
{
    uint32_t found = 0, region;
    uint64_t at;
    char nm[256];

    /* Excite Truck: 0x44-byte records with inline names and absolute offsets */
    bool old_toc = toc && tsz == 0x28 + (uint64_t)r->count * 0x44;
    bool old_inline = inline_ok && !toc && !r->pay && r->f.size >= 0x80 + (uint64_t)r->count * 0x44;
    if (old_toc || old_inline) {
        uint32_t tl = r->count * 0x44;
        const uint8_t *rec = old_toc ? toc + 0x28 : NULL;
        uint8_t *buf = NULL;
        if (!rec) {
            if (tl > RST_MAX_TABLE || !(buf = malloc(tl)) || !src_read_full(&r->f, 0x80, buf, tl)) {
                free(buf);
                return 0;
            }
            rec = buf;
        }
        for (uint32_t i = 0; i < r->count; i++) {
            const uint8_t *e = rec + i * 0x44;
            uint32_t sz = le32(e + 0x28), off = le32(e + 0x2C);
            if (!sz || !good_name(e, 32) || (uint64_t)off + sz > r->f.size) continue;
            clean(nm, sizeof(nm), (const char *)e, 32);
            add(ctx, nm, sz, 1, off);
            found++;
        }
        free(buf);
        if (found || old_toc) return found;
    }

    if (toc && tsz >= 0x0C + (r->count + 1ull) * 0x28) {
        bool compact = le32(toc) == 3 && tsz >= 0x20;
        bool tbe = !compact && !memcmp(toc, "ETOCRES0", 8);
        uint32_t n = compact ? r32(tbe, toc + 0x0C) : r->count;
        uint32_t eoff = compact ? 0x20 : 0x0C;
        uint64_t names = eoff + (uint64_t)(compact ? n : n + 1) * 0x28;
        if (names >= tsz) return 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t e = eoff + (uint64_t)(compact ? i : i + 1) * 0x28;
            if (e + 0x14 > tsz) break;
            uint32_t nrel = r32(tbe, toc + e), sz = r32(tbe, toc + e + 0x0C), off = r32(tbe, toc + e + 0x10);
            if (!sz || names + nrel >= tsz || !rst_where(r, off, sz, &region, &at)) continue;
            clean(nm, sizeof(nm), (const char *)toc + names + nrel, tsz - names - nrel);
            if (!nm[0]) continue;
            add(ctx, nm, sz, region, at);
            found++;
        }
        return found;
    }

    /* WiiWare tocres.res: a compact directory at the start of its (compressed) payload */
    if (!toc && (r->pay || inline_ok)) {
        const uint8_t *p = r->pay;
        uint32_t pl = r->plen;
        uint8_t *buf = NULL;
        if (!p) {
            if (r->tail_len > RST_MAX_TABLE) return 0;
            pl = (uint32_t)r->tail_len;
            if (!(buf = malloc(pl ? pl : 1)) || !src_read_full(&r->f, r->tail_off, buf, pl)) {
                free(buf);
                return 0;
            }
            p = buf;
        }
        uint64_t names = (uint64_t)r->count * 0x28;
        if (pl >= names)
            for (uint32_t i = 0; i < r->count; i++) {
                const uint8_t *e = p + i * 0x28;
                uint32_t nrel = r32(r->be, e), sz = r32(r->be, e + 0x0C), off = r32(r->be, e + 0x10);
                if (!sz || off > pl || sz > pl - off || nrel >= pl - names) continue;
                const char *s = (const char *)p + names + nrel;
                if (!memchr(s, 0, pl - names - nrel) || !good_name((const uint8_t *)s, pl - names - nrel)) continue;
                clean(nm, sizeof(nm), s, pl - names - nrel);
                /* these offsets are within the payload itself */
                if (r->pay) add(ctx, nm, sz, 0, off);
                else add(ctx, nm, sz, 1, r->tail_off + off);
                found++;
            }
        free(buf);
    }
    return found;
}

/* ---- the .toc inside a WiiWare tocres.res */

typedef struct {
    const char *want;
    rst_t *r;
    uint8_t *data;
    uint32_t size;
} toc_find_t;

static void toc_find_add(void *ctx, const char *name, uint32_t size, uint32_t region, uint64_t at)
{
    toc_find_t *t = ctx;
    if (t->data || strcasecmp(name, t->want) || size > RST_MAX_TABLE) return;
    uint8_t *d = malloc(size ? size : 1);
    if (!d) return;
    if (region == 0) memcpy(d, t->r->pay + at, size);
    else if (!src_read_full(&t->r->f, at, d, size)) {
        free(d);
        return;
    }
    t->data = d;
    t->size = size;
}

static uint8_t *toc_from_tocres(const char *tocname, uint32_t *size)
{
    src_t s;
    if (!archive_open_sibling("tocres.res", &s)) return NULL;
    rst_t r;
    toc_find_t t = { tocname, &r, NULL, 0 };
    if (rst_load(&r, &s)) rst_list(&r, NULL, 0, true, toc_find_add, &t);
    rst_free(&r);
    if (s.close) s.close(s.ctx);
    *size = t.size;
    return t.data;
}

/* ---- the archive */

typedef struct {
    rst_t r;
} rst_arc_t;

static void arc_add(void *ctx, const char *name, uint32_t size, uint32_t region, uint64_t at)
{
    tree_add_path(&((archive_t *)ctx)->tree, name, false, size, at, region, 0);
}

static int32_t rst_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    rst_arc_t *p = a->priv;
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    if (nd->b == 0) {
        memcpy(buf, p->r.pay + nd->a + off, len);
        return (int32_t)len;
    }
    return a->base.read(a->base.ctx, nd->a + off, buf, len);
}

static void rst_destroy(archive_t *a)
{
    rst_arc_t *p = a->priv;
    if (!p) return;
    rst_free(&p->r);
    free(p);
}

bool arc_rst_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x80];
    if (!rst_head(f, h)) return false;
    uint32_t n = r32(!memcmp(h, "RST0", 4), h + 0x20);
    return n && n <= 20000;
}

archive_t *arc_rst_open(const src_t *f, const char *name)
{
    rst_arc_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (!rst_load(&p->r, f)) {
        if (!g_archive_err) g_archive_err = "Not a Monster Games RST archive";
        free(p);
        return NULL;
    }

    /* "x.res" -> "x.toc" beside it, else inside tocres.res */
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    char tocname[256];
    snprintf(tocname, sizeof(tocname), "%s", base);
    char *dot = strrchr(tocname, '.');
    if (dot) *dot = 0;
    size_t l = strlen(tocname);
    if (l + 5 < sizeof(tocname)) memcpy(tocname + l, ".toc", 5);

    uint8_t *toc = NULL;
    uint32_t tsz = 0;
    src_t ts;
    if (archive_open_sibling(tocname, &ts)) {
        if (ts.size >= 0x0C && ts.size <= RST_MAX_TABLE && (toc = malloc((size_t)ts.size)) &&
            src_read_full(&ts, 0, toc, (uint32_t)ts.size))
            tsz = (uint32_t)ts.size;
        else {
            free(toc);
            toc = NULL;
        }
        if (ts.close) ts.close(ts.ctx);
    }
    if (!toc && strcasecmp(base, "tocres.res")) toc = toc_from_tocres(tocname, &tsz);

    archive_t *a = archive_alloc("Monster Games RST");
    if (!a) {
        free(toc);
        rst_free(&p->r);
        free(p);
        return NULL;
    }
    a->priv = p;
    a->read = rst_read;
    a->destroy = rst_destroy;
    a->base = *f;
    p->r.f = *f;
    uint32_t n = rst_list(&p->r, toc, tsz, !strcasecmp(base, "tocres.res"), arc_add, a);
    free(toc);
    if (!n) {
        g_archive_err = "No files found (is its .toc next to it?)";
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        return NULL;
    }
    return a;
}

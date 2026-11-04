/*
 * Retro Studios PAK archives, read-only, for browsing. Layouts and the LZO1X decoder follow
 * quatric's nintoolbox (lib-rpak.c, lib-lzo.c, extract/retropak.inc).
 *
 *   PAK 3.5 (Metroid Prime, Metroid Prime 2 on GameCube)
 *     u16 3, u16 5, u32 0; u32 named count, {fourcc, u32 id, u32 name length, name};
 *     u32 resource count, {u32 compressed, fourcc, u32 id, u32 size, u32 offset}.
 *     A compressed resource is u32 size, then a zlib stream (Prime) or {s16 length, LZO1X data}
 *     pieces (Prime 2; a negative length is a stored piece of that many bytes).
 *   PAK 2 (Metroid Prime 3, Metroid Prime Trilogy, Donkey Kong Country Returns)
 *     u32 2, u32 0x40, hash; u32 section count, {tag, u32 size}; 0x40-aligned STRG / RSHD / DATA.
 *     STRG {u32 n, {cstr name, fourcc, u64 id}}, RSHD {u32 n, {u32 compressed, fourcc, u64 id,
 *     u32 size, u32 offset}}. A compressed resource is "CMPD", u32 blocks, {u32 (flags << 29 |
 *     stored size), u32 size}, then the blocks: stored when both sizes match, zlib (DKC Returns)
 *     or {u16 length, LZO1X data} pieces.
 *
 * Resources are listed as name.type (or the id when unnamed). Nothing is unpacked up front:
 * a compressed resource is decompressed when it is first read, one at a time.
 *
 * Also lzop's .lzo files (LZO1X blocks behind a small header), opened as the file inside.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "archive.h"

#define MAX_RES (48u * 1024 * 1024)

/* ---------------------------------------------------------------- LZO1X */

/* One LZO1X stream (the original format, as described in the Linux kernel's lzo.rst) into
 * out[0..cap). Returns the bytes written, or -1 if the stream is damaged or doesn't fit. */
long lzo1x_decode(const uint8_t *src, size_t n, uint8_t *out, size_t cap)
{
    size_t ip = 0, op = 0, state = 0;
    if (!n) return -1;
#define NEED_IN(k)  do { if ((size_t)(k) > n - ip) return -1; } while (0)
#define NEED_OUT(k) do { if ((size_t)(k) > cap - op) return -1; } while (0)
#define LITERALS(k) do { size_t k_ = (k); NEED_IN(k_); NEED_OUT(k_); memcpy(out + op, src + ip, k_); ip += k_; op += k_; } while (0)
#define LENGTH(bits, base, value, res) do {                         \
        size_t v_ = (value);                                        \
        if (!v_) {                                                  \
            v_ = (1u << (bits)) - 1;                                \
            while (ip < n && !src[ip]) { v_ += 255; ip++; if (v_ > (1u << 28)) return -1; } \
            if (ip >= n) return -1;                                 \
            v_ += src[ip++];                                        \
        }                                                           \
        (res) = v_ + (base);                                        \
    } while (0)

    size_t token = src[ip++];
    bool have = true;
    if (token > 17) {
        size_t k = token - 17;
        LITERALS(k);
        state = k < 4 ? k : 4;
        have = false;
    }
    for (;;) {
        if (!have) { NEED_IN(1); token = src[ip++]; }
        have = false;
        size_t len, dist;
        if (token < 16) {
            if (!state) {
                LENGTH(4, 3, token, len);
                LITERALS(len);
                state = 4;
                continue;
            }
            NEED_IN(1);
            len = state < 4 ? 2 : 3;
            dist = ((size_t)src[ip++] << 2) + (token >> 2) + (state < 4 ? 1 : 2049);
            state = token & 3;
        } else if (token < 32) {
            LENGTH(3, 2, token & 7, len);
            NEED_IN(2);
            size_t d = src[ip] | (size_t)src[ip + 1] << 8;
            ip += 2;
            dist = 16384 + ((token & 8) << 11) + (d >> 2);
            state = d & 3;
            if (dist == 16384) return (long)op;     /* end of stream */
        } else if (token < 64) {
            LENGTH(5, 2, token & 31, len);
            NEED_IN(2);
            size_t d = src[ip] | (size_t)src[ip + 1] << 8;
            ip += 2;
            dist = (d >> 2) + 1;
            state = d & 3;
        } else {
            NEED_IN(1);
            len = token < 128 ? 3 + ((token >> 5) & 1) : 5 + ((token >> 5) & 3);
            dist = ((size_t)src[ip++] << 3) + ((token >> 2) & 7) + 1;
            state = token & 3;
        }
        if (!dist || dist > op) return -1;
        NEED_OUT(len);
        for (size_t i = 0; i < len; i++) out[op + i] = out[op - dist + i];
        op += len;
        LITERALS(state);
    }
#undef NEED_IN
#undef NEED_OUT
#undef LITERALS
#undef LENGTH
}

/* {u16 length, LZO1X} pieces (a length with the top bit set is a stored piece of 0x10000 - length). */
static bool lzo_pieces(const uint8_t *p, size_t n, uint8_t *out, size_t size)
{
    size_t q = 0, got = 0;
    while (got < size) {
        if (n - q < 2) return false;
        unsigned w = p[q] << 8 | p[q + 1];
        q += 2;
        bool stored = w & 0x8000;
        size_t l = stored ? 0x10000 - w : w;
        if (!l || l > n - q) return false;
        if (stored) {
            if (l > size - got) return false;
            memcpy(out + got, p + q, l);
            got += l;
        } else {
            long r = lzo1x_decode(p + q, l, out + got, size - got);
            if (r < 0) return false;
            got += (size_t)r;
        }
        q += l;
    }
    return true;
}

static bool zlib_exact(const uint8_t *p, size_t n, uint8_t *out, size_t size)
{
    uLongf got = size;
    return uncompress(out, &got, p, n) == Z_OK && got == size;
}

/* ---------------------------------------------------------------- the archive */

typedef struct {
    int node;                   /* tree node whose bytes are in buf, or -1 */
    uint8_t *buf;
} retro_priv;

enum { PAK_V35 = 1, PAK_V2 = 2 };

/* node: a = offset in the file, b = stored size, c = format (bit 0 compressed, bits 1.. PAK version) */
static uint8_t *unpack(archive_t *a, const tnode *nd)
{
    uint32_t stored = (uint32_t)nd->b;
    if (stored > MAX_RES) return NULL;
    uint8_t *in = malloc(stored ? stored : 1), *out = NULL;
    if (!in || !src_read_full(&a->src, nd->a, in, stored)) goto fail;
    out = malloc(nd->size ? nd->size : 1);
    if (!out) goto fail;
    if ((nd->c >> 1) == PAK_V35) {
        if (stored < 4) goto fail;
        bool z = stored >= 6 && in[4] == 0x78 && ((in[4] << 8 | in[5]) % 31) == 0;
        if (!(z ? zlib_exact(in + 4, stored - 4, out, nd->size) : lzo_pieces(in + 4, stored - 4, out, nd->size)))
            goto fail;
    } else {
        uint32_t nb = be32(in + 4);
        size_t ip = 8 + 8 * (size_t)nb, op = 0;
        if (stored < 8 || memcmp(in, "CMPD", 4) || !nb || ip > stored) goto fail;
        for (uint32_t i = 0; i < nb; i++) {
            uint32_t cs = be32(in + 8 + i * 8) & 0x1FFFFFFF, ds = be32(in + 12 + i * 8);
            if (cs > stored - ip || ds > nd->size - op) goto fail;
            const uint8_t *p = in + ip;
            bool ok;
            if (cs == ds) { memcpy(out + op, p, ds); ok = true; }
            else if (cs >= 2 && p[0] == 0x78 && ((p[0] << 8 | p[1]) % 31) == 0) ok = zlib_exact(p, cs, out + op, ds);
            else ok = lzo_pieces(p, cs, out + op, ds);
            if (!ok) goto fail;
            ip += cs;
            op += ds;
        }
        if (op != nd->size) goto fail;
    }
    free(in);
    return out;
fail:
    free(in);
    free(out);
    return NULL;
}

static int32_t retro_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    if (!(nd->c & 1))
        return a->src.read(a->src.ctx, nd->a + off, buf, len);
    retro_priv *p = a->priv;
    int idx = (int)(nd - a->tree.n);
    if (p->node != idx) {
        free(p->buf);
        p->buf = unpack(a, nd);
        p->node = p->buf ? idx : -1;
        if (!p->buf) return -1;
    }
    memcpy(buf, p->buf + off, len);
    return (int32_t)len;
}

static void retro_destroy(archive_t *a)
{
    retro_priv *p = a->priv;
    if (p) free(p->buf);
    free(p);
}

static void type_ext(const uint8_t *fourcc, char out[5])
{
    for (int k = 0; k < 4; k++) {
        uint8_t c = fourcc[k];
        out[k] = c >= 'A' && c <= 'Z' ? c + 32 : (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ? c : '_';
    }
    out[4] = 0;
}

static int add_res(archive_t *a, const char *name, const char *ext, uint64_t off, uint32_t stored, uint32_t size,
                   bool compressed, int version)
{
    char nm[256];
    snprintf(nm, sizeof(nm), "%s.%s", name, ext);
    tree_clean_name(nm);
    if (tree_find_child(&a->tree, 0, nm) >= 0) {       /* the same resource can be listed twice */
        size_t l = strlen(nm);
        snprintf(nm + l, sizeof(nm) - l, "_%x", (unsigned)off);
    }
    return tree_add(&a->tree, 0, nm, false, size, off, stored, (uint32_t)(compressed | version << 1));
}

static bool v35_header(const uint8_t *h) { return be32(h) == 0x00030005 && be32(h + 4) == 0; }
static bool v2_header(const uint8_t *h) { return be32(h) == 2 && be32(h + 4) == 0x40; }

bool arc_retro_probe(const src_t *f, const char *name)
{
    uint8_t h[0x48];
    if (!arc_name_has_ext(name, ".pak") || f->size < 0x48 || !src_read_full(f, 0, h, 0x48)) return false;
    return v35_header(h) || (v2_header(h) && be32(h + 0x40) && be32(h + 0x40) <= 8);
}

/* The size a compressed resource unpacks to. */
static bool packed_size(const src_t *f, uint64_t off, uint32_t stored, int version, uint32_t *size)
{
    uint8_t h[8];
    if (stored < 8 || !src_read_full(f, off, h, 8)) return false;
    if (version == PAK_V35) { *size = be32(h); return *size <= MAX_RES; }
    uint32_t nb = be32(h + 4);
    if (memcmp(h, "CMPD", 4) || !nb || nb > 4096 || 8 + 8ull * nb > stored) return false;
    uint8_t *t = malloc(8 * nb);
    if (!t || !src_read_full(f, off + 8, t, 8 * nb)) { free(t); return false; }
    uint64_t total = 0;
    for (uint32_t i = 0; i < nb; i++) total += be32(t + i * 8 + 4);
    free(t);
    *size = (uint32_t)total;
    return total <= MAX_RES;
}

static uint8_t *read_at(const src_t *f, uint64_t off, uint32_t n)
{
    if (n > 64u * 1024 * 1024 || off + n > f->size) return NULL;
    uint8_t *b = malloc(n ? n : 1);
    if (b && !src_read_full(f, off, b, n)) { free(b); b = NULL; }
    return b;
}

static bool open_v35(archive_t *a, const src_t *f)
{
    uint8_t c[4];
    if (!src_read_full(f, 8, c, 4)) return false;
    uint32_t named = be32(c);
    /* the name table is small; read up to 1 MiB of it and walk it */
    uint32_t span = f->size - 12 < 0x100000 ? (uint32_t)(f->size - 12) : 0x100000;
    uint8_t *nt = read_at(f, 12, span);
    if (!nt) return false;
    size_t q = 0;
    typedef struct { uint32_t id; char type[4]; char name[96]; } named_t;
    named_t *names = named && named < 100000 ? calloc(named, sizeof(named_t)) : NULL;
    uint32_t got = 0;
    for (uint32_t i = 0; i < named && names; i++) {
        if (q + 12 > span) break;
        uint32_t l = be32(nt + q + 8);
        if (l > span - q - 12) break;
        memcpy(names[got].type, nt + q, 4);
        names[got].id = be32(nt + q + 4);
        snprintf(names[got].name, sizeof(names[got].name), "%.*s", (int)(l < 95 ? l : 95), (const char *)nt + q + 12);
        got++;
        q += 12 + l;
    }
    bool ok = false;
    uint64_t rt = 12 + q;
    uint8_t *rc = q + 4 <= span ? nt + q : NULL;
    uint32_t nres = rc ? be32(rc) : 0;
    uint8_t *tab = nres && nres < 1000000 ? read_at(f, rt + 4, nres * 20) : NULL;
    for (uint32_t i = 0; tab && i < nres; i++) {
        const uint8_t *e = tab + i * 20;
        uint32_t comp = be32(e), id = be32(e + 8), stored = be32(e + 12), off = be32(e + 16), size = stored;
        if ((uint64_t)off + stored > f->size) continue;
        if (comp && !packed_size(f, off, stored, PAK_V35, &size)) continue;
        char ext[5], nm[100];
        type_ext(e + 4, ext);
        snprintf(nm, sizeof(nm), "%08x", (unsigned)id);
        for (uint32_t k = 0; k < got; k++)
            if (names[k].id == id && !memcmp(names[k].type, e + 4, 4)) { snprintf(nm, sizeof(nm), "%s", names[k].name); break; }
        if (add_res(a, nm, ext, off, stored, size, comp != 0, PAK_V35) >= 0) ok = true;
    }
    free(tab);
    free(names);
    free(nt);
    return ok;
}

static bool open_v2(archive_t *a, const src_t *f)
{
    uint8_t h[0x44 + 64];
    if (!src_read_full(f, 0, h, 0x44)) return false;
    uint32_t ns = be32(h + 0x40);
    if (!ns || ns > 8 || !src_read_full(f, 0x44, h + 0x44, ns * 8)) return false;
    uint64_t pos = (0x44 + 8ull * ns + 0x3F) & ~0x3Full, strg = 0, rshd = 0, data = 0;
    uint32_t strg_n = 0, rshd_n = 0;
    for (uint32_t i = 0; i < ns; i++) {
        const uint8_t *t = h + 0x44 + i * 8;
        uint32_t sz = be32(t + 4);
        if (pos + sz > f->size) return false;
        if (!memcmp(t, "STRG", 4)) { strg = pos; strg_n = sz; }
        else if (!memcmp(t, "RSHD", 4)) { rshd = pos; rshd_n = sz; }
        else if (!memcmp(t, "DATA", 4)) data = pos;
        pos = (pos + sz + 0x3F) & ~0x3Full;
    }
    if (!rshd || !data || rshd_n < 4) return false;
    uint8_t *rs = read_at(f, rshd, rshd_n), *st = strg_n ? read_at(f, strg, strg_n) : NULL;
    bool ok = false;
    uint32_t nres = rs ? be32(rs) : 0;
    if (nres && 4 + 24ull * nres <= rshd_n) {
        for (uint32_t i = 0; i < nres; i++) {
            const uint8_t *e = rs + 4 + i * 24;
            uint32_t comp = be32(e), stored = be32(e + 16), size = stored;
            uint64_t off = data + be32(e + 20);
            if (off + stored > f->size) continue;
            if (comp && !packed_size(f, off, stored, PAK_V2, &size)) continue;
            char ext[5], nm[208];
            type_ext(e + 4, ext);
            snprintf(nm, sizeof(nm), "%02x%02x%02x%02x%02x%02x%02x%02x", e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15]);
            if (st && strg_n >= 4) {                    /* names: cstring, fourcc, u64 id */
                size_t q = 4;
                for (uint32_t k = 0, cnt = be32(st); k < cnt && q < strg_n; k++) {
                    const char *s = (const char *)st + q;
                    size_t l = strnlen(s, strg_n - q);
                    if (q + l + 1 + 12 > strg_n) break;
                    if (!memcmp(st + q + l + 5, e + 8, 8)) { snprintf(nm, sizeof(nm), "%.200s", s); break; }
                    q += l + 1 + 12;
                }
            }
            if (add_res(a, nm, ext, off, stored, size, comp != 0, PAK_V2) >= 0) ok = true;
        }
    }
    free(rs);
    free(st);
    return ok;
}

archive_t *arc_retro_open(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[8];
    if (!src_read_full(f, 0, h, 8)) return NULL;
    archive_t *a = archive_alloc("Retro PAK");
    retro_priv *p = calloc(1, sizeof(*p));
    if (!a || !p) { free(p); if (a) archive_close(a); return NULL; }
    p->node = -1;
    a->priv = p;
    a->read = retro_read;
    a->destroy = retro_destroy;
    a->src = *f;                                /* read only with the file's own reader until we keep it */
    bool ok = v35_header(h) ? open_v35(a, f) : open_v2(a, f);
    if (!ok) {
        memset(&a->src, 0, sizeof(a->src));
        archive_close(a);
        return NULL;
    }
    a->base = *f;
    return a;
}

/* ---------------------------------------------------------------- lzop (.lzo) */

static const uint8_t LZOP_MAGIC[9] = { 0x89, 'L', 'Z', 'O', 0x00, 0x0D, 0x0A, 0x1A, 0x0A };

bool arc_lzop_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[9];
    return f->size > 40 && src_read_full(f, 0, h, 9) && !memcmp(h, LZOP_MAGIC, 9);
}

archive_t *arc_lzop_open(const src_t *f, const char *name)
{
    if (f->size > MAX_RES * 2) { g_archive_err = "This .lzo file is too large to open here"; return NULL; }
    uint32_t len = (uint32_t)f->size;
    uint8_t *in = read_at(f, 0, len), *out = NULL;
    if (!in) return NULL;
    size_t p = 9;
    char inner[256] = "";
#define NEED(k) do { if ((size_t)(k) > len - p) goto bad; } while (0)
    NEED(4);
    unsigned version = in[p] << 8 | in[p + 1];
    p += 4;                                             /* version, library version */
    if (version >= 0x0940) { NEED(2); p += 2; }         /* version needed to extract */
    NEED(1);
    unsigned method = in[p++];
    if (method < 1 || method > 3) goto bad;             /* LZO1X-1, LZO1X-1(15), LZO1X-999 */
    if (version >= 0x0940) { NEED(1); p++; }            /* level */
    NEED(4);
    uint32_t flags = be32(in + p);
    p += 4;
    if (flags & 0x800) { NEED(4); p += 4; }             /* filter */
    NEED(8);
    p += 8;                                             /* mode, mtime */
    if (version >= 0x0940) { NEED(4); p += 4; }
    NEED(1);
    unsigned nl = in[p++];
    NEED(nl + 4);
    snprintf(inner, sizeof(inner), "%.*s", (int)nl, (const char *)in + p);
    p += nl + 4;                                        /* name, header checksum */
    if (flags & 0x40) { NEED(4); uint32_t xl = be32(in + p); p += 4; NEED((uint64_t)xl + 4); p += xl + 4; }

    size_t cap = 0, op = 0;
    for (;;) {
        NEED(4);
        uint32_t dl = be32(in + p);
        p += 4;
        if (!dl) break;
        NEED(4);
        uint32_t sl = be32(in + p);
        p += 4;
        if (dl > 64u * 1024 * 1024 || sl > dl || op + dl > MAX_RES * 2) goto bad;
        if (flags & 1) p += 4;                          /* Adler-32 / CRC-32 of the data */
        if (flags & 0x100) p += 4;
        if (sl < dl) { if (flags & 2) p += 4; if (flags & 0x200) p += 4; }
        NEED(sl);
        if (op + dl > cap) {
            size_t nc = cap ? cap * 2 : 1u << 20;
            while (nc < op + dl) nc *= 2;
            uint8_t *nb = realloc(out, nc);
            if (!nb) goto bad;
            out = nb;
            cap = nc;
        }
        if (sl == dl) memcpy(out + op, in + p, dl);
        else if (lzo1x_decode(in + p, sl, out + op, dl) != (long)dl) goto bad;
        op += dl;
        p += sl;
    }
#undef NEED
    free(in);
    char *slash = strrchr(inner, '/');                 /* lzop keeps the path it was given */
    if (slash) memmove(inner, slash + 1, strlen(slash));
    tree_clean_name(inner);
    if (!inner[0] || !strcmp(inner, ".") || !strcmp(inner, "..")) {
        const char *base = strrchr(name ? name : "", '/');
        base = base ? base + 1 : (name ? name : "");
        snprintf(inner, sizeof(inner), "%s", base);
        if (arc_name_has_ext(inner, ".lzo")) inner[strlen(inner) - 4] = 0;
        if (!inner[0]) snprintf(inner, sizeof(inner), "data.bin");
    }
    if (!out) out = malloc(1);
    return out ? archive_open_memory(out, op, inner) : NULL;
bad:
    free(in);
    free(out);
    g_archive_err = "This .lzo file is damaged or uses a method other than LZO1X";
    return NULL;
}

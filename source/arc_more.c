/*
 * More archives, read-only, for browsing. Layouts match quatric's nintoolbox (lib-vcra.c,
 * lib-sszl.c, lib-rflres.c, lib-gpak.c, lib-gpkg.c, lib-blpkg.c and the matching extract readers).
 *
 *   VCRA       Namco Museum Remix: "VCRA", u32 count, size, crc flag (LE); entries of 44 bytes
 *              (offset, size, crc, name[32]) or, when the flag is 0, 64 bytes from 0x40 (offset,
 *              size, name[56]).
 *   SSZL       Namco Museum: LZSS in a 4 KiB ring, "SSZL", u32 0, packed size, unpacked size (LE).
 *   RFL_Res    The Mii parts library (RFL_Res.dat, no magic): u16 count, then offsets of up to 18
 *              part archives, each u16 count and count + 1 offsets; textures are BTIs.
 *   GPAK       World of Goo master.pak: u32 count, 0, 0, 0; 16-byte {offset, size} records.
 *   GPKG       Bonsai Barber .pkg: one zlib stream; 0x14-byte header (u32 data offset at 4,
 *              count at 16), 0x28-byte entries {name[32], offset, size}, offsets from the end.
 *   BLPKG      Barking Lizards "pkg\0": count at 8, 32 bytes of stats, 52-byte entries
 *              {name[32], type, size, packed size, offset, ?}: stored or gzip.
 *
 * Everything is unpacked into memory when the archive opens.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "archive.h"

#define MAX_OUT (48u * 1024 * 1024)

static bool head(const src_t *f, uint8_t *h, uint32_t n)
{
    return f->size >= n && src_read_full(f, 0, h, n);
}

static uint8_t *read_all(const src_t *f, uint32_t *len)
{
    if (f->size > MAX_OUT) return NULL;
    uint8_t *b = malloc(f->size ? f->size : 1);
    if (b && !src_read_full(f, 0, b, (uint32_t)f->size)) {
        free(b);
        return NULL;
    }
    *len = (uint32_t)f->size;
    return b;
}

/* ---------------------------------------------------------------- building an archive in memory */

typedef struct {
    archive_t *a;
    uint8_t *blob;
    uint64_t n, cap;
    bool bad;
} mb_t;

static int32_t mb_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    return a->src.read(a->src.ctx, nd->a + off, buf, len);
}

static bool mb_init(mb_t *m, const char *kind)
{
    memset(m, 0, sizeof(*m));
    m->a = archive_alloc(kind);
    if (!m->a) return false;
    m->a->read = mb_read;
    return true;
}

/* Adds a file at `path` ("dir/name"; folders are made as needed) with a copy of its bytes. */
static void mb_file(mb_t *m, const char *path, const void *d, uint32_t n)
{
    if (m->bad) return;
    if (m->n + n > MAX_OUT) {
        m->bad = true;
        return;
    }
    if (m->n + n > m->cap) {
        uint64_t nc = (m->cap + n) * 2;
        if (nc > MAX_OUT) nc = MAX_OUT;
        uint8_t *nb = realloc(m->blob, nc ? nc : 1);
        if (!nb) {
            m->bad = true;
            return;
        }
        m->blob = nb;
        m->cap = nc;
    }
    if (n) memcpy(m->blob + m->n, d, n);
    char p[256];
    snprintf(p, sizeof(p), "%s", path);
    for (char *q = p; *q; q++)
        if (*q == '\\') *q = '/';
        else if (*q == '|' || (unsigned char)*q < 0x20) *q = '_';
    tree_add_path(&m->a->tree, p, false, n, m->n, 0, 0);
    m->n += n;
}

static archive_t *mb_done(mb_t *m, const src_t *f)
{
    if (m->bad || m->a->tree.count <= 1) {
        archive_close(m->a);
        free(m->blob);
        return NULL;
    }
    m->a->base = *f;
    m->a->src = src_from_memory(m->blob, m->n);
    if (!m->a->src.ctx) {
        memset(&m->a->base, 0, sizeof(m->a->base));
        archive_close(m->a);
        return NULL;
    }
    return m->a;
}

static void mb_drop(mb_t *m)
{
    archive_close(m->a);
    free(m->blob);
}

/* A name from a fixed-size field: printable and terminated, else "fileNNNN.bin". */
static void field_name(char *out, size_t n, const uint8_t *src, size_t len, unsigned idx)
{
    size_t k = 0;
    while (k < len && k + 1 < n && src[k]) {
        out[k] = (char)src[k];
        k++;
    }
    out[k] = 0;
    bool ok = k > 0 && strcmp(out, ".") && strcmp(out, "..");
    for (size_t i = 0; i < k; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7E) ok = false;
    if (!ok) snprintf(out, n, "file%04u.bin", idx);
}

/* ---------------------------------------------------------------- VCRA */

static bool vcra_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    return head(f, h, 16) && !memcmp(h, "VCRA", 4) && le32(h + 8) == f->size && le32(h + 4);
}

static archive_t *vcra_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint32_t cnt = le32(d + 4);
    bool crc = le32(d + 12) != 0;
    uint32_t tab = crc ? 12 : 64, es = crc ? 44 : 64, nl = crc ? 32 : 56, no = crc ? 12 : 8;
    mb_t m;
    archive_t *a = NULL;
    if (cnt && cnt <= (len - tab) / es && mb_init(&m, "VCRA")) {
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = d + tab + i * es;
            uint32_t off = le32(e), sz = le32(e + 4);
            if (off > len || sz > len - off) continue;
            char nm[64];
            field_name(nm, sizeof(nm), e + no, nl, i);
            mb_file(&m, nm, d + off, sz);
        }
        a = mb_done(&m, f);
    }
    free(d);
    return a;
}

/* ---------------------------------------------------------------- SSZL */

static bool sszl_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    return head(f, h, 16) && !memcmp(h, "SSZL", 4) && le32(h + 12) && le32(h + 12) <= MAX_OUT &&
           le32(h + 8) <= f->size - 16;
}

static archive_t *sszl_open(const src_t *f, const char *name)
{
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    uint32_t zs = le32(d + 8), us = le32(d + 12);
    uint8_t *out = us <= MAX_OUT && zs <= len - 16 ? malloc(us) : NULL;
    if (!out) {
        free(d);
        return NULL;
    }

    uint8_t ring[4096] = { 0 };
    uint32_t r = 4096 - 18, ip = 0, op = 0, flags = 0;
    const uint8_t *in = d + 16;
    bool ok = true;

    while (op < us && ok) {
        if (!(flags & 0x100)) {
            if (ip >= zs) {
                ok = false;
                break;
            }
            flags = in[ip++] | 0xFF00;
        }

        if (flags & 1) {
            if (ip >= zs) {
                ok = false;
                break;
            }
            out[op++] = ring[r] = in[ip++];
            r = (r + 1) & 0xFFF;
        } else {
            if (ip + 1 >= zs) {
                ok = false;
                break;
            }
            uint32_t p = in[ip++], b = in[ip++];
            p |= (b & 0xF0) << 4;
            uint32_t n = (b & 15) + 3;
            if (n > us - op) {
                ok = false;
                break;
            }
            while (n--) {
                out[op++] = ring[r] = ring[p++ & 0xFFF];
                r = (r + 1) & 0xFFF;
            }
        }

        flags >>= 1;
    }

    free(d);
    if (!ok) {
        free(out);
        return NULL;
    }

    char inner[256];
    const char *base = strrchr(name ? name : "", '/');
    snprintf(inner, sizeof(inner), "%s", base ? base + 1 : (name ? name : "data.bin"));
    char *dot = strrchr(inner, '.');
    if (dot && (!strcasecmp(dot, ".sszl") || !strcasecmp(dot, ".szl"))) *dot = 0;
    else if (!dot) snprintf(inner + strlen(inner), sizeof(inner) - strlen(inner), ".bin");
    return archive_open_memory(out, us, inner);
}

/* ---------------------------------------------------------------- RFL_Res (Mii parts) */

static const char *const rfl_names[18] = { "beard",    "eye",       "eyebrow", "faceline",
                                           "face_tex", "fore_head", "glass",   "glass_tex",
                                           "hair",     "mask",      "mole",    "mouth",
                                           "mustache", "nose",      "nline",   "nline_tex",
                                           "cap",      "cap_tex" };

static bool rfl_scan(const uint8_t *d, uint32_t len, mb_t *m)
{
    if (len < 8) return false;
    uint32_t cnt = be16(d);
    if (cnt < 1 || cnt > 128 || 4 + cnt * 4 > len) return false;

    uint32_t prev = 0, files = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t off = be32(d + 4 + i * 4);
        if (off < 4 + cnt * 4 || (uint64_t)off + 4 > len || off < prev) return false;
        prev = off;
        uint32_t num = be16(d + off);
        if ((uint64_t)off + 4 + (num + 1ull) * 4 > len) return false;
        files += num;
    }

    if (!files || files > 0x10000) return false;
    if (!m) return true;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t off = be32(d + 4 + i * 4), num = be16(d + off);
        uint64_t base = (uint64_t)off + 4 + (num + 1ull) * 4;
        char cat[16];
        if (i < 18) snprintf(cat, sizeof(cat), "%s", rfl_names[i]);
        else snprintf(cat, sizeof(cat), "arc_%02u", (unsigned)i);
        bool tex = strstr(cat, "tex") || !strcmp(cat, "eye") || !strcmp(cat, "eyebrow") ||
                   !strcmp(cat, "mole") || !strcmp(cat, "mouth") || !strcmp(cat, "mustache");

        for (uint32_t j = 0; j < num; j++) {
            uint32_t a = be32(d + off + 4 + j * 4), b = be32(d + off + 8 + j * 4);
            if (b < a || base + b > len) return false;
            char nm[48];
            snprintf(nm, sizeof(nm), "%s/%03u%s", cat, (unsigned)j, tex ? ".bti" : ".bin");
            mb_file(m, nm, d + base + a, b - a);
        }
    }
    return true;
}

static bool rfl_probe(const src_t *f, const char *name)
{
    /* no magic: only by name, then the whole table has to make sense */
    if (!arc_name_has_ext(name, ".dat") && !arc_name_has_ext(name, ".rfl") &&
        !arc_name_has_ext(name, ".rflres"))
        return false;
    uint32_t len;
    if (f->size > MAX_OUT) return false;
    uint8_t *d = read_all(f, &len);
    bool ok = d && rfl_scan(d, len, NULL);
    free(d);
    return ok;
}

static archive_t *rfl_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    mb_t m;
    archive_t *a = NULL;
    if (mb_init(&m, "Mii parts")) {
        if (rfl_scan(d, len, &m)) a = mb_done(&m, f);
        else mb_drop(&m);
    }
    free(d);
    return a;
}

/* ---------------------------------------------------------------- GPAK (World of Goo) */

static bool gpak_probe(const src_t *f, const char *name)
{
    if (!arc_name_has_ext(name, ".pak") && !arc_name_has_ext(name, ".gpak")) return false;
    uint8_t h[32];
    if (!head(f, h, 32)) return false;
    uint32_t n = be32(h);
    if (be32(h + 8) || !n || n > 0x100000 || (n + 1ull) * 16 > f->size) return false;
    uint32_t off = be32(h + 16), sz = be32(h + 20);
    return off >= (n + 1ull) * 16 && (uint64_t)off + sz <= f->size;
}

static archive_t *gpak_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint32_t n = be32(d);
    mb_t m;
    archive_t *a = NULL;
    if (n && (n + 1ull) * 16 <= len && mb_init(&m, "GPAK")) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = d + (i + 1) * 16;
            uint32_t off = be32(e), sz = be32(e + 4);
            if ((uint64_t)off + sz > len) continue;
            char nm[24];
            snprintf(nm, sizeof(nm), "file_%04u.bin", (unsigned)i);
            mb_file(&m, nm, d + off, sz);
        }
        a = mb_done(&m, f);
    }
    free(d);
    return a;
}

/* ---------------------------------------------------------------- GPKG (Bonsai Barber) */

static uint8_t *inflate_all(const uint8_t *src, uint32_t n, uint32_t *out_n, int wbits)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, wbits) != Z_OK) return NULL;

    uint32_t cap = n * 4 + 4096, got = 0;
    uint8_t *o = NULL;
    int r = Z_OK;
    z.next_in = (Bytef *)src;
    z.avail_in = n;

    while (r == Z_OK) {
        if (got == cap || !o) {
            if (o) cap *= 2;
            if (cap > MAX_OUT) cap = MAX_OUT;
            if (got >= cap) {
                r = Z_MEM_ERROR;
                break;
            }
            uint8_t *no = realloc(o, cap);
            if (!no) {
                r = Z_MEM_ERROR;
                break;
            }
            o = no;
        }
        z.next_out = o + got;
        z.avail_out = cap - got;
        r = inflate(&z, Z_NO_FLUSH);
        got = cap - z.avail_out;
    }

    inflateEnd(&z);
    if (r != Z_STREAM_END) {
        free(o);
        return NULL;
    }

    *out_n = got;
    return o;
}

static bool gpkg_probe(const src_t *f, const char *name)
{
    if (!arc_name_has_ext(name, ".pkg")) return false;
    uint8_t h[2];
    return head(f, h, 2) && h[0] == 0x78 && ((h[0] << 8 | h[1]) % 31) == 0;
}

static archive_t *gpkg_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len, dn = 0;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint8_t *p = inflate_all(d, len, &dn, 15);
    free(d);
    if (!p) return NULL;
    archive_t *a = NULL;
    mb_t m;
    uint32_t doff = dn >= 0x14 ? be32(p + 4) : 0, n = dn >= 0x14 ? be32(p + 16) : 0;
    if (dn >= 0x14 && !be32(p + 8) && n && n <= 0x100000 && doff <= dn &&
        0x14 + (uint64_t)n * 0x28 <= dn && mb_init(&m, "PKG")) {
        uint32_t base = dn - doff;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = p + 0x14 + i * 0x28;
            uint64_t off = (uint64_t)be32(e + 0x20) + base;
            uint32_t sz = be32(e + 0x24);
            if (off + sz > dn) continue;
            char nm[40];
            field_name(nm, sizeof(nm), e, 0x20, i);
            mb_file(&m, nm, p + off, sz);
        }
        a = mb_done(&m, f);
    }
    free(p);
    return a;
}

/* ---------------------------------------------------------------- BLPKG (Barking Lizards) */

static bool blpkg_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    if (!head(f, h, 16) || memcmp(h, "pkg\0", 4)) return false;
    uint32_t n = be32(h + 8);
    return n && n <= 0x10000 && 48 + (uint64_t)n * 52 <= f->size;
}

static archive_t *blpkg_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint32_t n = be32(d + 8);
    mb_t m;
    archive_t *a = NULL;

    if (mb_init(&m, "pkg")) {
        uint64_t pos = 48;
        for (uint32_t i = 0; i < n && pos + 52 <= len; i++, pos += 52) {
            const uint8_t *e = d + pos;
            uint32_t type = be32(e + 32) & 0xFFFF, ds = be32(e + 36), cs = be32(e + 40),
                     off = be32(e + 44);
            if (type > 1 || (uint64_t)off + cs > len || ds > MAX_OUT) continue;

            char nm[40];
            field_name(nm, sizeof(nm), e, 32, i);
            if (type == 0) {
                mb_file(&m, nm, d + off, cs);
                continue;
            }

            uint32_t got = 0;
            uint8_t *u = inflate_all(d + off, cs, &got, 15 + 32);
            if (u && got == ds) mb_file(&m, nm, u, got);
            free(u);
        }
        a = mb_done(&m, f);
    }

    free(d);
    return a;
}

/* ---------------------------------------------------------------- FSYS (Genius Sonority) */

/* "LZSS", u32 size, u32 packed size (with this header), u32 0; then LZSS in a 4 KiB ring. */
static bool fsys_lzss(uint8_t *out, uint32_t n, const uint8_t *in, uint32_t in_n)
{
    if (in_n < 16 || memcmp(in, "LZSS", 4) || be32(in + 4) != n || be32(in + 8) < 16 ||
        be32(in + 8) > in_n)
        return false;
    const uint8_t *ip = in + 16, *end = in + be32(in + 8);
    uint8_t ring[4096] = { 0 };
    uint32_t op = 0, rp = 4096 - 18, flags = 0;

    while (op < n) {
        if (!(flags & 0x100)) {
            if (ip >= end) return false;
            flags = 0xFF00 | *ip++;
        }
        if (flags & 1) {
            if (ip >= end) return false;
            ring[rp] = out[op++] = *ip++;
            rp = (rp + 1) & 4095;
        } else {
            if (end - ip < 2) return false;
            uint32_t pos = *ip++, b = *ip++;
            pos |= (b & 0xF0) << 4;
            for (uint32_t c = (b & 15) + 3; c-- && op < n;) {
                ring[rp] = out[op++] = ring[pos];
                pos = (pos + 1) & 4095;
                rp = (rp + 1) & 4095;
            }
        }
        flags >>= 1;
    }

    return true;
}

static bool fsys_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x40];
    return head(f, h, 0x40) && !memcmp(h, "FSYS", 4) && be32(h + 12) <= 65536 &&
           be32(h + 24) < f->size;
}

static archive_t *fsys_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    uint32_t n = be32(d + 12), tab = be32(d + 24);
    mb_t m;
    archive_t *a = NULL;

    if (tab <= len - 12 && mb_init(&m, "FSYS")) {
        uint32_t list = be32(d + tab);
        for (uint32_t i = 0; list <= len && i < n && i < (len - list) / 4; i++) {
            uint32_t e = be32(d + list + i * 4);
            if (e > len || len - e < 40) continue;
            uint32_t off = be32(d + e + 4), sz = be32(d + e + 8), fl = be32(d + e + 12),
                     cs = be32(d + e + 20);
            uint32_t stored = fl & 0x80000000u ? cs : sz;
            if (off > len || stored > len - off || sz > MAX_OUT) continue;

            char nm[24];
            snprintf(nm, sizeof(nm), "file_%04u.bin", (unsigned)i);
            if (fl & 0x80000000u) {
                uint8_t *u = malloc(sz ? sz : 1);
                if (u && fsys_lzss(u, sz, d + off, stored)) mb_file(&m, nm, u, sz);
                free(u);
            } else
                mb_file(&m, nm, d + off, sz);
        }
        a = mb_done(&m, f);
    }

    free(d);
    return a;
}

/* ---------------------------------------------------------------- PTD (Mario Party sound) */

static bool ptd_ok(const uint8_t *d, uint32_t n)
{
    if (n < 32) return false;
    uint32_t ver = be16(d), files = be16(d + 2), rate = be32(d + 8), ch = be32(d + 12);
    uint32_t eo = be32(d + 16), co = be32(d + 20), ho = be32(d + 24), so = be32(d + 28);
    return (ver == 1 || ver == 2) && files >= 1 && files <= 2000 && rate >= 4000 && rate <= 96000 &&
           ch >= 1 && ch <= 8 && eo >= 32 && eo + files * 4ull <= n && co < n && ho < n && so <= n;
}

static bool ptd_probe(const src_t *f, const char *name)
{
    if (!arc_name_has_ext(name, ".ptd") && !arc_name_has_ext(name, ".pdt")) return false;
    uint8_t h[32];
    return head(f, h, 32) && f->size >= 32 &&
           ptd_ok(h, (uint32_t)(f->size > 0xFFFFFFFFu ? 0xFFFFFFFFu : f->size));
}

static uint32_t ptd_samples(uint32_t nib)
{
    return nib % 16 ? nib / 16 * 14 + nib % 16 - 2 : nib / 16 * 14;
}

/* One channel as a standard .dsp (0x60-byte header, then the ADPCM bytes). */
static void ptd_dsp(mb_t *m, const char *nm, const uint8_t *d, uint32_t n, uint32_t fl,
                    uint32_t rate, uint32_t nib, uint32_t loop, uint32_t so, uint32_t ci,
                    uint32_t co)
{
    uint32_t bytes = (nib + 1) / 2;
    if ((uint64_t)so + bytes > n) return;
    uint8_t *x = calloc(1, 0x60 + bytes);
    if (!x) return;
    uint32_t smp = ptd_samples(nib), le = nib ? nib - 1 : 0;
    uint8_t hd[28] = { smp >> 24, smp >> 16, smp >> 8, smp, nib >> 24, nib >> 16, nib >> 8, nib,
                       rate >> 24, rate >> 16, rate >> 8, rate, 0, (fl & 0x02000000u) ? 1 : 0, 0, 0,
                       loop >> 24, loop >> 16, loop >> 8, loop, le >> 24, le >> 16, le >> 8, le,
                       0, 0, 0, 0 };
    memcpy(x, hd, 28);
    if ((uint64_t)co + ci * 32ull + 32 <= n) memcpy(x + 28, d + co + ci * 32, 32);
    if (bytes) {
        x[63] = d[so];
        memcpy(x + 0x60, d + so, bytes);
    }
    mb_file(m, nm, x, 0x60 + bytes);
    free(x);
}

static archive_t *ptd_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    mb_t m;
    archive_t *a = NULL;
    if (ptd_ok(d, len) && mb_init(&m, "PTD")) {
        uint32_t files = be16(d + 2), eo = be32(d + 16), co = be32(d + 20);

        for (uint32_t i = 0; i < files; i++) {
            uint32_t fo = be32(d + eo + i * 4);
            if (!fo || fo + 24 > len) continue;
            uint32_t fl = be32(d + fo), rate = be32(d + fo + 4), nib = be32(d + fo + 8),
                     loop = be32(d + fo + 12);
            bool st = fl & 0x01000000u;
            char nm[32];
            snprintf(nm, sizeof(nm), st ? "stream%03u_L.dsp" : "stream%03u.dsp", (unsigned)i);
            ptd_dsp(&m, nm, d, len, fl, rate, nib, loop, be32(d + fo + 16), be16(d + fo + 20), co);
            if (st && fo + 32 <= len) {
                snprintf(nm, sizeof(nm), "stream%03u_R.dsp", (unsigned)i);
                ptd_dsp(&m, nm, d, len, fl, rate, nib, loop, be32(d + fo + 24), be16(d + fo + 28),
                        co);
            }
        }
        a = mb_done(&m, f);
    }

    free(d);
    return a;
}

/* ---------------------------------------------------------------- And-Kensaku (.rz / "Pres") */

static void cstr_at(char *o, size_t n, const uint8_t *b, uint32_t len, uint32_t off)
{
    size_t k = 0;
    while (off < len && off + k < len && b[off + k] && k + 1 < n) {
        o[k] = (char)b[off + k];
        k++;
    }
    o[k] = 0;
}

static bool pres_scan(const uint8_t *b, uint32_t len, mb_t *m)
{
    if (len < 0x84 || memcmp(b, "Pres", 4)) return false;
#define TAB_OK(v) ((v) >= 0x80 && (v) <= len && !(((v) - 0x80) % 0x20))

    uint32_t end = le32(b + 0x60);
    if (!TAB_OK(end)) end = le32(b + 0x80);
    if (!TAB_OK(end)) {
        end = 0x80;
        while (end + 0x20 <= len) {
            bool z = true;
            for (int j = 0x10; j < 0x20; j++) if (b[end + j]) z = false;
            if (!z) break;
            end += 0x20;
        }
    }
#undef TAB_OK

    uint32_t n = (end - 0x80) / 0x20;
    if (!n || n > 0x100000) return false;
    if (!m) return true;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = b + 0x80 + i * 0x20;
        bool z = true;
        for (int j = 0x10; j < 0x20; j++) if (r[j]) z = false;
        if (!z) break;

        uint32_t off = le32(r), sz = le32(r + 4), no = le32(r + 8), ne = le32(r + 12);
        if (!off || (uint64_t)off + sz > len || no > len || len - no < 12) continue;
        char nm[96] = "", ex[32] = "", fd[96] = "", rel[240];
        if (ne >= 1) cstr_at(nm, sizeof(nm), b, len, le32(b + no));
        if (ne >= 2) cstr_at(ex, sizeof(ex), b, len, le32(b + no + 4));
        if (ne >= 3) cstr_at(fd, sizeof(fd), b, len, le32(b + no + 8));
        if (*fd && *ex) snprintf(rel, sizeof(rel), "%s/%s.%s", fd, nm, ex);
        else if (*ex) snprintf(rel, sizeof(rel), "%s.%s", nm, ex);
        else if (*nm) snprintf(rel, sizeof(rel), "%s", nm);
        else snprintf(rel, sizeof(rel), "file_%04u.bin", (unsigned)i);

        char uniq[256];
        snprintf(uniq, sizeof(uniq), "%s", rel);
        for (unsigned dup = 1; tree_find(&m->a->tree, uniq) >= 0 && dup < 1000; dup++) {
            const char *dot = strrchr(rel, '.');
            if (dot && dot != rel)
                snprintf(uniq, sizeof(uniq), "%.*s~%u%s", (int)(dot - rel), rel, dup, dot);
            else
                snprintf(uniq, sizeof(uniq), "%s~%u", rel, dup);
        }
        mb_file(m, uniq, b + off, sz);
    }

    return true;
}

static bool pres_probe(const src_t *f, const char *name)
{
    uint8_t h[4];
    if (!head(f, h, 4)) return false;
    if (!memcmp(h, "Pres", 4)) return f->size >= 0x84;
    return arc_name_has_ext(name, ".rz") && (h[0] == 0x10 || h[0] == 0x11);
}

static archive_t *pres_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    if (memcmp(d, "Pres", 4)) {                 /* .rz: the Pres archive inside LZ10 / LZ11 */
        uint32_t un;
        uint8_t *u = arc_lz_unwrap(NULL, d, len, &un);
        free(d);
        if (!u) return NULL;
        d = u;
        len = un;
    }
    mb_t m;
    archive_t *a = NULL;
    if (mb_init(&m, "Pres")) {
        if (pres_scan(d, len, &m)) a = mb_done(&m, f);
        else mb_drop(&m);
    }
    free(d);
    return a;
}

/* ---------------------------------------------------------------- Arika INFO.DAT + GAME.DAT */

/* INFO.DAT: 16-byte title (also the key when its first byte isn't 0), sector size at 0x24, count
 * at 0x2C, 0x30-byte records {name[32], packed size, sector, ?, size}. A packed member is "ALZ1"
 * (or "ZALZ" + 4 bytes) and LZSS with inverted flag bits; members may be "RF2" sub-archives. */
typedef struct {
    src_t game;
    uint8_t *cache;
    int cached;
} arika_t;

static void arika_destroy(archive_t *a)
{
    arika_t *p = a->priv;
    if (!p) return;
    if (p->game.close) p->game.close(p->game.ctx);
    free(p->cache);
    free(p);
}

/* Decodes until dn bytes are out, or (with grow) until the input runs out, enlarging *dst as it goes. */
static bool alz1_run(uint8_t **dst, uint32_t *dn, bool grow, const uint8_t *src, uint32_t sn)
{
    uint8_t ring[0x1000] = { 0 };
    uint32_t p = 0xFEE, sp = 0, dp = 0, flags = 0, bits = 0, cap = *dn;
    uint8_t *o = *dst;
#define ALZ_PUT(v)                                                                       \
    do {                                                                                 \
        if (dp == cap) {                                                                 \
            if (!grow || cap >= MAX_OUT) return false;                                   \
            uint32_t nc = cap < 0x10000 ? 0x10000 : cap > MAX_OUT / 2 ? MAX_OUT : cap * 2; \
            uint8_t *no = realloc(o, nc);                                                \
            if (!no) return false;                                                       \
            *dst = o = no;                                                               \
            cap = nc;                                                                    \
        }                                                                                \
        o[dp++] = (v);                                                                   \
    } while (0)
    while (grow || dp < *dn) {
        if (!bits) {
            if (sp >= sn) break;
            flags = src[sp++];
            bits = 8;
        }
        bool lit = flags & 1;
        flags >>= 1;
        bits--;
        if (lit) {
            if (sp >= sn) break;
            uint8_t c = ring[p++ & 0xFFF] = src[sp++];
            ALZ_PUT(c);
        } else {
            if (sp + 2 > sn) break;
            uint32_t q = src[sp] | (uint32_t)(src[sp + 1] >> 4) << 8, l = (src[sp + 1] & 15) + 3;
            sp += 2;
            for (uint32_t i = 0; i < l && (grow || dp < *dn); i++) {
                uint8_t c = ring[p++ & 0xFFF] = ring[q++ & 0xFFF];
                ALZ_PUT(c);
            }
        }
    }
#undef ALZ_PUT
    if (!grow) return dp == *dn;
    *dn = dp;
    return dp > 0;
}

static bool alz1(uint8_t *dst, uint32_t dn, const uint8_t *src, uint32_t sn)
{
    return alz1_run(&dst, &dn, false, src, sn);
}

/* A standalone "ALZ1" (or "ZALZ" + 4 bytes) file: no size is stored, so it runs to the end of the input. */
uint8_t *arc_alz1_unpack(const uint8_t *src, uint32_t n, uint32_t *out_len)
{
    uint32_t hdr = n >= 4 && !memcmp(src, "ALZ1", 4) ? 4 : n >= 8 && !memcmp(src, "ZALZ", 4) ? 8 : 0;
    if (!hdr) return NULL;
    uint32_t dn = 0;
    uint8_t *out = NULL;
    if (!alz1_run(&out, &dn, true, src + hdr, n - hdr)) {
        free(out);
        return NULL;
    }
    *out_len = dn;
    return out;
}

static int32_t arika_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    arika_t *p = a->priv;
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    if (!nd->c) return p->game.read(p->game.ctx, nd->a + off, buf, len);
    int idx = (int)(nd - a->tree.n);
    /* packed: the whole member, kept until another is read */
    if (p->cached != idx) {
        free(p->cache);
        p->cache = NULL;
        p->cached = -1;
        uint32_t zs = (uint32_t)nd->b;
        uint8_t *z = malloc(zs);
        uint8_t *u = malloc(nd->size ? nd->size : 1);
        bool ok = z && u && src_read_full(&p->game, nd->a, z, zs) &&
                  alz1(u, (uint32_t)nd->size, z + nd->c, zs - nd->c);
        free(z);
        if (!ok) {
            free(u);
            g_archive_err = "This member couldn't be unpacked";
            return -1;
        }
        p->cache = u;
        p->cached = idx;
    }
    memcpy(buf, p->cache + off, len);
    return (int32_t)len;
}

static bool arika_probe(const src_t *f, const char *name)
{
    const char *b = strrchr(name ? name : "", '/');
    b = b ? b + 1 : (name ? name : "");
    return !strcasecmp(b, "INFO.DAT") && f->size >= 0x30 && f->size <= (16u << 20);
}

static archive_t *arika_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    if (d[0])                                   /* decrypt: nibble swap, invert, minus the key */
        for (uint32_t i = 0x10; i < len; i++) {
            uint8_t b = (uint8_t)(d[i] >> 4 | d[i] << 4) ^ 0xFF;
            d[i] = (uint8_t)(b - d[i & 15]);
        }

    uint32_t sec = le32(d + 0x24) ? le32(d + 0x24) : 0x800, n = le32(d + 0x2C);
    archive_t *a = NULL;
    arika_t *p = calloc(1, sizeof(*p));
    if (!p || n > 0x40000 || 0x30 + (uint64_t)n * 0x30 > len ||
        (!archive_open_sibling("GAME.DAT", &p->game) &&
         !archive_open_sibling("game.dat", &p->game))) {
        g_archive_err = "INFO.DAT needs its GAME.DAT next to it";
        free(p);
        free(d);
        return NULL;
    }

    p->cached = -1;
    a = archive_alloc("Arika");
    if (!a) {
        arika_destroy(&(archive_t){ .priv = p });
        free(d);
        return NULL;
    }
    a->priv = p;
    a->read = arika_read;
    a->destroy = arika_destroy;
    a->base = *f;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = d + 0x30 + i * 0x30;
        char nm[40];
        if (!r[0]) continue;
        field_name(nm, sizeof(nm), r, 0x20, i);
        uint32_t zs = le32(r + 0x20), ds = le32(r + 0x2C);
        uint64_t off = (uint64_t)le32(r + 0x24) * sec;
        if ((!zs && !ds) || off > p->game.size) continue;
        uint32_t hdr = 0;
        if (zs != ds) {
            uint8_t m[4];
            if (!ds || ds > MAX_OUT || zs > p->game.size - off || zs < 8 ||
                !src_read_full(&p->game, off, m, 4))
                continue;
            if (!memcmp(m, "ALZ1", 4)) hdr = 4;
            else if (!memcmp(m, "ZALZ", 4)) hdr = 8;
            else continue;
        } else if (zs > p->game.size - off)
            continue;
        tree_add_path(&a->tree, nm, false, ds, off, zs, hdr);
    }

    free(d);
    if (a->tree.count <= 1) {
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        return NULL;
    }
    return a;
}

/* ---------------------------------------------------------------- RF2 (inside Arika archives) */

static bool rf2_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    if (!head(f, h, 16) || memcmp(h, "RF2", 3)) return false;
    uint32_t info = h[8] | h[9] << 8 | h[10] << 16, n = info / 0x20;
    return n && n <= 0x10000 && 16 + (uint64_t)n * 0x20 <= f->size;
}

static archive_t *rf2_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint32_t n = (d[8] | d[9] << 8 | d[10] << 16) / 0x20;
    mb_t m;
    archive_t *a = NULL;
    if (mb_init(&m, "RF2")) {
        for (uint32_t i = 0; i < n && 16 + (i + 1) * 0x20ull <= len; i++) {
            const uint8_t *r = d + 16 + i * 0x20;
            uint32_t sz = le32(r + 20), off = le32(r + 24);
            if (!sz || off >= len || sz > len - off) continue;
            char nm[32];
            field_name(nm, sizeof(nm), r, 20, i);
            mb_file(&m, nm, d + off, sz);
        }
        a = mb_done(&m, f);
    }
    free(d);
    return a;
}

/* ---------------------------------------------------------------- Camelot modules (.sbn) */

/* Camelot's models are PowerPC relocatable modules (elfbin/xcmdl_*.sbn on We Love Golf!) with their
 * texture banks inline, so a bank is found by scanning (4-byte aligned) for one that checks out, as
 * nintoolbox's lib-camtexbank.c does; a module may also be Camelot LZ packed. Only for .sbn names:
 * a signature scan is too weak to run on anything else. Each bank is listed as a TPL. */
static bool sbn_probe(const src_t *f, const char *name)
{
    return arc_name_has_ext(name, ".sbn") && f->size >= 16 && f->size <= MAX_OUT;
}

static archive_t *sbn_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    if (d[0] == 1 || d[0] == 2) {
        uint32_t un;
        uint8_t *u = arc_camelot_unpack(d, len, &un);
        if (u) {
            free(d);
            d = u;
            len = un;
        }
    }
    mb_t m;
    archive_t *a = NULL;
    unsigned banks = 0;
    if (mb_init(&m, "Camelot textures")) {
        for (uint32_t off = 0; off + 16 <= len; off += 4) {
            if (be32(d + off) != 0x0020AF30u || !arc_camelot_bank_valid(d + off, len - off)) continue;
            uint32_t tl;
            uint8_t *t = arc_camelot_bank_tpl(d + off, len - off, &tl);
            if (!t) continue;
            char nm[48];
            snprintf(nm, sizeof(nm), "bank_%02u_at_%06X.tpl", banks++, (unsigned)off);
            mb_file(&m, nm, t, tl);
            free(t);
        }
        if (banks) a = mb_done(&m, f);
        else mb_drop(&m);
    }
    free(d);
    if (!a && !banks) g_archive_err = "No Camelot texture bank in this module";
    return a;
}

/* ---------------------------------------------------------------- dispatch */

typedef struct {
    bool (*probe)(const src_t *, const char *);
    archive_t *(*open)(const src_t *, const char *);
} more_t;

static const more_t more[] = {
    { vcra_probe, vcra_open }, { sszl_probe, sszl_open }, { blpkg_probe, blpkg_open },
    { gpak_probe, gpak_open }, { gpkg_probe, gpkg_open }, { rfl_probe, rfl_open },
    { fsys_probe, fsys_open }, { ptd_probe, ptd_open }, { pres_probe, pres_open },
    { arika_probe, arika_open }, { rf2_probe, rf2_open }, { sbn_probe, sbn_open },
};

bool arc_more_probe(const src_t *f, const char *name)
{
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++)
        if (more[i].probe(f, name)) return true;
    return false;
}

archive_t *arc_more_open(const src_t *f, const char *name)
{
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++)
        if (more[i].probe(f, name)) return more[i].open(f, name);
    return NULL;
}

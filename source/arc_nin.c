/*
 * First-party GameCube / Wii containers, read-only, for browsing:
 *   Yaz0 (.szs, .yaz0)  and  Yay0     compressed; a compressed archive opens as the archive inside
 *   RARC (.arc, .rarc)                GameCube / Wii resource archive
 *   BRRES (.brres)                    NW4R resource file: its folders and sub-files (TEX0, PLT0,
 *                                     MDL0, ...)
 *   ASH0 (.ash)                       Wii Menu / Animal Crossing compression; opens as the file
 *                                     inside
 *   BRSAR (.brsar)                    NW4R sound archive: groups, their sequence / bank / sound
 *                                     files, and each wave archive's RWAV waves (playable)
 *   CCF (.ccf)                        Virtual Console container: 20 char names, zlib or stored
 *                                     members
 *   PAC (.pac)                        Super Smash Bros. Brawl "ARC" archive: typed, indexed members
 *   GFA (.gfa)                        Good-Feel archive (Kirby's Epic Yarn ...), BPE or LZ10
 *                                     payload, little endian
 *   BRWAR (.brwar, .rwar)             NW4R wave archive: its RWAV waves (playable)
 *   HAL SSM (.ssm)                    HAL Laboratory sound bank (Melee, Kururin Squash!): each sound's
 *                                     channels as plain DSP files (playable)
 * Layouts match quatric's nintoolbox (Wiimm's SZS tools lineage).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "archive.h"
#include "ash.h"

#define MAX_OUT (48u * 1024 * 1024)

static bool read_head(const src_t *f, uint8_t *h, uint32_t n)
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

static void inner_name(const char *name, char *out, size_t n, const char *const *exts)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    snprintf(out, n, "%s", base);
    for (int i = 0; exts[i]; i++)
        if (arc_name_has_ext(out, exts[i])) {
            char *dot = strrchr(out, '.');
            if (dot) *dot = '\0';
            break;
        }
    if (!out[0]) snprintf(out, n, "data.bin");
}

/* ---------------------------------------------------------------- Yaz0 / Yay0 */

bool arc_yaz0_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    return read_head(f, h, 16) && (!memcmp(h, "Yaz0", 4) || !memcmp(h, "Yay0", 4)) && be32(h + 4) &&
           be32(h + 4) <= MAX_OUT;
}

static bool yaz0_decode(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t osz)
{
    uint32_t ip = 16, op = 0;
    while (op < osz) {
        if (ip >= len) return false;
        uint8_t code = in[ip++];
        for (int bit = 0; bit < 8 && op < osz; bit++, code <<= 1) {
            if (code & 0x80) {
                if (ip >= len) return false;
                out[op++] = in[ip++];
                continue;
            }
            if (ip + 1 >= len) return false;
            uint8_t b1 = in[ip++], b2 = in[ip++];
            uint32_t dist = ((uint32_t)(b1 & 15) << 8 | b2) + 1, n = b1 >> 4;
            if (!n) {
                if (ip >= len) return false;
                n = in[ip++] + 0x12;
            } else
                n += 2;
            if (dist > op) return false;
            while (n-- && op < osz) {
                out[op] = out[op - dist];
                op++;
            }
        }
    }
    return true;
}

static bool yay0_decode(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t osz)
{
    uint32_t link = be32(in + 8), chunk = be32(in + 12), mp = 16, op = 0;
    uint32_t mask = 0;
    int bits = 0;
    while (op < osz) {
        if (!bits) {
            if (mp + 4 > len) return false;
            mask = be32(in + mp);
            mp += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            if (chunk >= len) return false;
            out[op++] = in[chunk++];
        } else {
            if (link + 2 > len) return false;
            uint16_t l = be16(in + link);
            link += 2;
            uint32_t dist = (l & 0xFFF) + 1, n = l >> 12;
            if (!n) {
                if (chunk >= len) return false;
                n = in[chunk++] + 0x12;
            } else
                n += 2;
            if (dist > op) return false;
            while (n-- && op < osz) {
                out[op] = out[op - dist];
                op++;
            }
        }

        mask <<= 1;
        bits--;
    }
    return true;
}

archive_t *arc_yaz0_open(const src_t *f, const char *name)
{
    uint32_t len;
    uint8_t *in = read_all(f, &len);
    if (!in) return NULL;
    uint32_t osz = be32(in + 4);
    uint8_t *out = malloc(osz ? osz : 1);
    bool ok = out && (!memcmp(in, "Yaz0", 4) ? yaz0_decode(in, len, out, osz) :
                                               yay0_decode(in, len, out, osz));
    free(in);
    if (!ok) {
        free(out);
        return NULL;
    }
    static const char *const exts[] = { ".szs", ".yaz0", ".yay0", ".szp", ".carc", NULL };
    char inner[256];
    inner_name(name, inner, sizeof(inner), exts);
    return archive_open_memory(out, osz, inner);
}

/* ---------------------------------------------------------------- RARC */

static int32_t raw_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    return a->src.read(a->src.ctx, n->a + off, buf, len);
}

static void raw_destroy(archive_t *a) { free(a->priv); }

static archive_t *raw_new(const char *kind, const src_t *f)
{
    archive_t *a = archive_alloc(kind);
    if (!a) return NULL;
    a->read = raw_read;
    a->destroy = raw_destroy;
    a->base = *f;
    a->src = a->base;
    return a;
}

static archive_t *raw_fail(archive_t *a)
{
    if (a) {
        memset(&a->base, 0, sizeof(a->base));
        memset(&a->src, 0, sizeof(a->src));
        archive_close(a);
    }
    return NULL;
}

bool arc_rarc_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x40];
    return read_head(f, h, 0x40) && !memcmp(h, "RARC", 4) && be32(h + 8) == 0x20;
}

typedef struct {
    const uint8_t *b;
    uint32_t len, nodes, nnodes, ents, nents, strs, strsz;
    uint64_t data;
} rarc_t;

static const char *rarc_str(const rarc_t *r, uint32_t off)
{
    if (off >= r->strsz) return NULL;
    const char *s = (const char *)r->b + r->strs + off;
    if (!memchr(s, 0, r->strsz - off)) return NULL;
    return s;
}

static bool rarc_dir(archive_t *a, const rarc_t *r, uint32_t node, int parent, int depth)
{
    if (node >= r->nnodes || depth > 32) return false;
    const uint8_t *nd = r->b + r->nodes + node * 0x10;
    uint32_t cnt = be16(nd + 10), first = be32(nd + 12);
    for (uint32_t i = 0; i < cnt; i++) {
        if (first + i >= r->nents) return false;
        const uint8_t *e = r->b + r->ents + (first + i) * 0x14;
        const char *nm = rarc_str(r, be16(e + 6));
        if (!nm || !strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        char clean[256];
        snprintf(clean, sizeof(clean), "%s", nm);
        tree_clean_name(clean);
        bool dir = (e[4] & 0x02) != 0;
        if (dir) {
            int idx = tree_add(&a->tree, parent, clean, true, 0, 0, 0, 0);
            if (idx < 0 || !rarc_dir(a, r, be32(e + 8), idx, depth + 1)) return false;
        } else {
            uint64_t off = r->data + be32(e + 8);
            uint32_t sz = be32(e + 12);
            if (off + sz > a->src.size) continue;
            if (tree_add(&a->tree, parent, clean, false, sz, off, 0, 0) < 0) return false;
        }
    }
    return true;
}

archive_t *arc_rarc_open(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x40];
    if (!read_head(f, h, 0x40)) return NULL;

    rarc_t r = { 0 };
    /* data offset, relative to 0x20 = size of the info part */
    uint32_t info_len = be32(h + 0x0C);
    if (info_len < 0x20 || info_len > 8 * 1024 * 1024 || 0x20 + (uint64_t)info_len > f->size)
        return NULL;

    uint8_t *b = malloc(0x20 + info_len);
    if (!b || !src_read_full(f, 0, b, 0x20 + info_len)) {
        free(b);
        return NULL;
    }

    r.b = b;
    r.len = 0x20 + info_len;
    r.nnodes = be32(b + 0x20);
    r.nodes = 0x20 + be32(b + 0x24);
    r.nents = be32(b + 0x28);
    r.ents = 0x20 + be32(b + 0x2C);
    r.strsz = be32(b + 0x30);
    r.strs = 0x20 + be32(b + 0x34);
    r.data = 0x20 + (uint64_t)info_len;

    archive_t *a = NULL;
    if ((uint64_t)r.nodes + r.nnodes * 0x10ull > r.len ||
        (uint64_t)r.ents + r.nents * 0x14ull > r.len || (uint64_t)r.strs + r.strsz > r.len ||
        !r.nnodes)
        goto out;

    a = raw_new("RARC", f);
    if (!a) goto out;
    if (!rarc_dir(a, &r, 0, 0, 0)) a = raw_fail(a);
out:
    free(b);
    return a;
}

/* ---------------------------------------------------------------- BRRES */

bool arc_brres_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x10];
    return read_head(f, h, 0x10) && !memcmp(h, "bres", 4) && be16(h + 4) == 0xFEFF;
}

static bool brres_name(const uint8_t *b, uint32_t len, uint32_t off, char *out, size_t n)
{
    if (off < 4 || off >= len) return false;
    uint32_t l = be32(b + off - 4);
    if (l > len - off || l >= n)
        l = (uint32_t)strnlen((const char *)b + off, len - off < n - 1 ? len - off : n - 1);
    memcpy(out, b + off, l);
    out[l] = '\0';
    tree_clean_name(out);
    return out[0] != '\0';
}

/* One index group: entry 0 is the root, then `count` entries of 16 bytes. */
static bool brres_group(archive_t *a, const uint8_t *b, uint32_t len, uint32_t grp, int parent,
                        int depth)
{
    if (depth > 2 || grp + 8 > len) return false;
    uint32_t count = be32(b + grp + 4);
    if (count > 20000 || grp + 8 + (count + 1) * 16ull > len) return false;
    for (uint32_t i = 1; i <= count; i++) {
        const uint8_t *e = b + grp + 8 + i * 16;
        uint32_t noff = be32(e + 8), doff = be32(e + 12);
        if (!doff) continue;
        char nm[200];
        if (!brres_name(b, len, grp + noff, nm, sizeof(nm))) continue;
        uint32_t at = grp + doff;
        if (at + 8 > len) continue;
        if (depth == 0) {                                   /* a folder: "Textures(NW4R)", ... */
            int idx = tree_add(&a->tree, parent, nm, true, 0, 0, 0, 0);
            if (idx < 0 || !brres_group(a, b, len, at, idx, depth + 1)) return false;
        } else {
            uint32_t sz = be32(b + at + 4);
            if (!sz || at + (uint64_t)sz > len) continue;
            /* show the type, so "mario" becomes "mario.tex0" */
            char full[220], ext[5];
            for (int k = 0; k < 4; k++) {
                char c = (char)b[at + k];
                ext[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') :
                         (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
            }
            ext[4] = '\0';
            snprintf(full, sizeof(full), "%s.%s", nm, ext);
            if (tree_add(&a->tree, parent, full, false, sz, at, 0, 0) < 0) return false;
        }
    }
    return true;
}

archive_t *arc_brres_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *b = read_all(f, &len);
    if (!b) return NULL;
    archive_t *a = NULL;
    uint32_t root = be16(b + 0x0C);
    if (root + 8 > len || memcmp(b + root, "root", 4)) goto out;
    a = raw_new("BRRES", f);
    if (!a) goto out;
    if (!brres_group(a, b, len, root + 8, 0, 0)) a = raw_fail(a);
out:
    free(b);
    return a;
}

/* ---------------------------------------------------------------- ASH0 */

bool arc_ash0_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[16];
    if (!read_head(f, h, 16) || memcmp(h, "ASH0", 4)) return false;
    uint32_t osz = be32(h + 4) & 0x00FFFFFF;
    return osz && osz <= MAX_OUT && be32(h + 8) + 4 <= f->size;
}

archive_t *arc_ash0_open(const src_t *f, const char *name)
{
    uint32_t len;
    uint8_t *in = read_all(f, &len);
    if (!in) return NULL;
    uint32_t osz = be32(in + 4) & 0x00FFFFFF;
    uint8_t *out = malloc(osz);
    bool ok = out && ash_decompress(in, len, out, osz);
    free(in);
    if (!ok) {
        free(out);
        return NULL;
    }
    static const char *const exts[] = { ".ash", ".ash0", NULL };
    char inner[256];
    inner_name(name, inner, sizeof(inner), exts);
    return archive_open_memory(out, osz, inner);
}

/* ---------------------------------------------------------------- GFA */

/* Byte pair encoding, Good-Feel's GFCP mode 1: per block, a pair table and a
 * big endian block length, then bytes expanded recursively through the table. */
bool arc_bpe_decode(uint8_t *dst, uint32_t dsz, const uint8_t *src, uint32_t ssz)
{
    uint32_t sp = 0, dp = 0;
    uint8_t t0[256], t1[256], stack[512];
    bool paired[256];
    while (dp < dsz) {
        for (int i = 0; i < 256; i++) {
            t0[i] = (uint8_t)i;
            t1[i] = 0;
            paired[i] = false;
        }

        uint32_t c = 0;
        while (c < 256) {
            if (sp >= ssz) return false;
            uint32_t m = src[sp++], n;
            if (m > 127) {
                c += m - 127;
                if (c == 256) break;
                n = 1;
            } else n = m + 1;
            for (uint32_t i = 0; i < n && c < 256; i++, c++) {
                if (sp >= ssz) return false;
                uint8_t lc = src[sp++];
                t0[c] = lc;
                if (lc != (uint8_t)c) {
                    if (sp >= ssz) return false;
                    t1[c] = src[sp++];
                    paired[c] = true;
                }
            }
        }

        if (sp + 2 > ssz) return false;
        uint32_t blk = (uint32_t)src[sp] << 8 | src[sp + 1], sn = 0;
        sp += 2;

        while (blk || sn) {
            uint8_t b;
            if (sn) b = stack[--sn];
            else {
                if (sp >= ssz) return false;
                b = src[sp++];
                blk--;
            }
            if (paired[b]) {
                if (sn + 2 > sizeof(stack)) return false;
                stack[sn++] = t1[b];
                stack[sn++] = t0[b];
            } else {
                if (dp >= dsz) return false;
                dst[dp++] = b;
            }
        }
    }
    return true;
}

/* Plain LZ10 bit stream without the usual 4 byte header (GFCP modes 2 / 3). */
bool arc_lz10_raw(uint8_t *dst, uint32_t dsz, const uint8_t *src, uint32_t ssz)
{
    uint32_t sp = 0, dp = 0;
    while (dp < dsz) {
        if (sp >= ssz) return false;
        uint8_t fl = src[sp++];
        for (int bit = 0; bit < 8 && dp < dsz; bit++, fl <<= 1) {
            if (!(fl & 0x80)) {
                if (sp >= ssz) return false;
                dst[dp++] = src[sp++];
            } else {
                if (sp + 2 > ssz) return false;
                uint8_t a = src[sp++], b = src[sp++];
                uint32_t n = (a >> 4) + 3, back = ((a & 15u) << 8 | b) + 1;
                if (back > dp || n > dsz - dp) return false;
                for (uint32_t k = 0; k < n; k++, dp++) dst[dp] = dst[dp - back];
            }
        }
    }
    return true;
}

bool arc_gfa_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x1C];
    return read_head(f, h, 0x1C) && !memcmp(h, "GFAC", 4);
}

archive_t *arc_gfa_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    archive_t *a = NULL;
    uint8_t *blob = NULL;
    uint32_t info = le32(d + 0x0C), doff = le32(d + 0x14), dsz = le32(d + 0x18);
    if ((uint64_t)info + 4 > len || (uint64_t)doff + 20 > len || (uint64_t)doff + dsz > len)
        goto out;

    uint32_t n = le32(d + info);
    if (!n || n > 100000 || (uint64_t)info + 4 + n * 16ull > len) goto out;
    const uint8_t *cp = d + doff;
    if (memcmp(cp, "GFCP", 4)) goto out;
    uint32_t mode = le32(cp + 8), olen = le32(cp + 12), zsz = le32(cp + 16);
    if (!olen || olen > MAX_OUT || 20ull + zsz > dsz) goto out;

    blob = malloc(olen);
    if (!blob) goto out;
    bool ok = mode == 1 ? arc_bpe_decode(blob, olen, cp + 20, zsz)
            : (mode == 2 || mode == 3) ? arc_lz10_raw(blob, olen, cp + 20, zsz) : false;
    if (!ok) goto out;

    a = archive_alloc("GFA");
    if (!a) goto out;
    a->read = raw_read;
    a->base = *f;
    a->src = src_from_memory(blob, olen);
    if (!a->src.ctx) {
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        a = NULL;
        goto out;
    }
    blob = NULL;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = d + info + 4 + i * 16;
        uint32_t no = le32(r + 4) & 0x00FFFFFF, sz = le32(r + 8), off = le32(r + 12);
        char nm[256];
        if (no && no < len) {
            uint32_t k = 0;
            while (no + k < len && d[no + k] && k + 1 < sizeof(nm)) {
                nm[k] = (char)d[no + k];
                k++;
            }
            nm[k] = 0;
        } else nm[0] = 0;

        if (!nm[0]) snprintf(nm, sizeof(nm), "file%04u.bin", (unsigned)i);
        if (off >= doff) off -= doff;
        if ((uint64_t)off + sz > olen) continue;
        if (tree_add_path(&a->tree, nm, false, sz, off, 0, 0) < 0) {
            memset(&a->base, 0, sizeof(a->base));
            archive_close(a);
            a = NULL;
            goto out;
        }
    }
out:
    free(blob);
    free(d);
    return a;
}

/* ---------------------------------------------------------------- BRSAR */

bool arc_brsar_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x28];
    return read_head(f, h, 0x28) && !memcmp(h, "RSAR", 4) && be16(h + 4) == 0xFEFF;
}

typedef struct {
    const uint8_t *b;
    uint32_t n;
} blk_t;

static uint32_t bu32(const blk_t *k, uint64_t off)
{
    return off + 4 <= k->n ? be32(k->b + off) : 0xFFFFFFFFu;
}

/* SYMB: a string table (count, then offsets) of NUL terminated names. */
static const char *rsar_name(const blk_t *sy, uint32_t id)
{
    uint32_t tab = bu32(sy, 0), cnt = bu32(sy, tab);
    if (id == 0xFFFFFFFFu || tab == 0xFFFFFFFFu || id >= cnt) return NULL;
    uint32_t o = bu32(sy, tab + 4 + (uint64_t)id * 4);
    if (o >= sy->n || !memchr(sy->b + o, 0, sy->n - o)) return NULL;
    return (const char *)sy->b + o;
}

static const char *rsar_ext(const uint8_t m[4])
{
    if (!memcmp(m, "RSEQ", 4)) return ".rseq";
    if (!memcmp(m, "RBNK", 4)) return ".rbnk";
    if (!memcmp(m, "RWSD", 4)) return ".rwsd";
    if (!memcmp(m, "RWAV", 4)) return ".rwav";
    if (!memcmp(m, "RSTM", 4)) return ".brstm";
    return ".bin";
}

/* An RWAR wave archive at `at` (absolute) becomes a folder of RWAVs. */
static void rsar_rwar(archive_t *a, const src_t *f, uint64_t at, uint32_t size, int parent)
{
    uint8_t h[0x20];
    if (size < 0x40 || !src_read_full(f, at, h, 0x20) || memcmp(h, "RWAR", 4)) return;
    uint32_t tabl = be32(h + 0x10), dat = be32(h + 0x18);
    uint8_t t[12];
    if ((uint64_t)tabl + 12 > size || (uint64_t)dat + 8 > size ||
        !src_read_full(f, at + tabl, t, 12) || memcmp(t, "TABL", 4))
        return;
    uint32_t cnt = be32(t + 8);
    if (cnt > 4096 || tabl + 12 + cnt * 12ull > size) return;
    uint8_t *e = malloc(cnt * 12 + 1);
    if (!e || !src_read_full(f, at + tabl + 12, e, cnt * 12)) {
        free(e);
        return;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t off = be32(e + i * 12 + 4), sz = be32(e + i * 12 + 8);
        if (!sz || (uint64_t)dat + off + sz > size) continue;
        char nm[24];
        snprintf(nm, sizeof(nm), "wave%04u.rwav", (unsigned)i);
        tree_add(&a->tree, parent, nm, false, sz, at + dat + off, 0, 0);
    }
    free(e);
}

archive_t *arc_brsar_open(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x28];
    if (!read_head(f, h, 0x28)) return NULL;

    uint32_t so = be32(h + 0x10), ss = be32(h + 0x14), io = be32(h + 0x18), is = be32(h + 0x1C);
    if (ss < 8 || is < 8 || ss > 16u << 20 || is > 16u << 20 || (uint64_t)so + ss > f->size ||
        (uint64_t)io + is > f->size)
        return NULL;

    uint8_t *sb = malloc(ss), *ib = malloc(is);
    archive_t *a = NULL;
    uint8_t *done = NULL;
    if (!sb || !ib || !src_read_full(f, so, sb, ss) || !src_read_full(f, io, ib, is) ||
        memcmp(sb, "SYMB", 4) || memcmp(ib, "INFO", 4))
        goto out;

    blk_t sy = { sb + 8, ss - 8 }, in = { ib + 8, is - 8 };
    uint32_t ftab = bu32(&in, 0x1C), fcnt = bu32(&in, ftab), gtab = bu32(&in, 0x24),
             gcnt = bu32(&in, gtab);
    uint32_t btab = bu32(&in, 0x0C), bcnt = bu32(&in, btab), stab = bu32(&in, 0x04),
             scnt = bu32(&in, stab);

    if (fcnt > in.n / 8 || gcnt > in.n / 8 || !gcnt) goto out;
    if (bcnt > in.n / 8) bcnt = 0;
    if (scnt > in.n / 8) scnt = 0;

    const char **fname = calloc(fcnt + 1, sizeof(*fname));
    done = calloc(fcnt + 1, 1);
    if (!fname || !done) {
        free(fname);
        goto out;
    }

    /* File names: a bank's own name, else the first sound that uses the file. */
    for (uint32_t i = 0; i < bcnt; i++) {
        uint32_t e = bu32(&in, btab + 8 + i * 8ull), fid = bu32(&in, e + 4ull);
        if (fid < fcnt && !fname[fid]) fname[fid] = rsar_name(&sy, bu32(&in, e));
    }
    for (uint32_t i = 0; i < scnt; i++) {
        uint32_t e = bu32(&in, stab + 8 + i * 8ull), fid = bu32(&in, e + 4ull);
        if (fid < fcnt && !fname[fid]) fname[fid] = rsar_name(&sy, bu32(&in, e));
    }

    a = raw_new("BRSAR", f);
    if (!a) {
        free(fname);
        goto out;
    }

    for (uint32_t g = 0; g < gcnt; g++) {
        uint32_t ge = bu32(&in, gtab + 8 + g * 8ull);
        uint32_t gdata = bu32(&in, ge + 0x10ull), gwave = bu32(&in, ge + 0x18ull);
        uint32_t itab = bu32(&in, ge + 0x24ull), icnt = bu32(&in, itab);
        if (icnt > in.n / 8) continue;
        const char *gn = rsar_name(&sy, bu32(&in, ge));
        char gname[160];
        if (gn) snprintf(gname, sizeof(gname), "%s", gn);
        else snprintf(gname, sizeof(gname), "group%03u", (unsigned)g);
        tree_clean_name(gname);
        int gi = -1;
        for (uint32_t i = 0; i < icnt; i++) {
            uint32_t ie = bu32(&in, itab + 8 + i * 8ull);
            uint32_t fid = bu32(&in, ie), doff = bu32(&in, ie + 4ull), dsz = bu32(&in, ie + 8ull);
            uint32_t woff = bu32(&in, ie + 0xCull), wsz = bu32(&in, ie + 0x10ull);
            if (fid < fcnt && done[fid]) continue;
            if (!dsz || dsz == 0xFFFFFFFFu || (uint64_t)gdata + doff + dsz > f->size) continue;
            uint8_t m[4];
            if (!src_read_full(f, (uint64_t)gdata + doff, m, 4)) continue;
            if (gi < 0 && (gi = tree_add(&a->tree, 0, gname, true, 0, 0, 0, 0)) < 0) break;
            char stem[160], full[180];
            if (fid < fcnt && fname[fid]) snprintf(stem, sizeof(stem), "%s", fname[fid]);
            else snprintf(stem, sizeof(stem), "file%03u", (unsigned)fid);
            tree_clean_name(stem);
            const char *ext = rsar_ext(m);
            snprintf(full, sizeof(full), "%s%s", stem, arc_name_has_ext(stem, ext) ? "" : ext);
            tree_add(&a->tree, gi, full, false, dsz, (uint64_t)gdata + doff, 0, 0);
            if (fid < fcnt) done[fid] = 1;
            /* older archives keep raw samples here instead of an RWAR */
            uint8_t wm[4];
            if (wsz >= 0x40 && wsz != 0xFFFFFFFFu && (uint64_t)gwave + woff + wsz <= f->size &&
                src_read_full(f, (uint64_t)gwave + woff, wm, 4) && !memcmp(wm, "RWAR", 4)) {
                snprintf(full, sizeof(full), "%s waves", stem);
                int wi = tree_add(&a->tree, gi, full, true, 0, 0, 0, 0);
                if (wi >= 0) rsar_rwar(a, f, (uint64_t)gwave + woff, wsz, wi);
            }
        }
    }

    /* Sequences: every sequence sound, playable with its bank (A in the browser). Each entry is
     * its RSEQ file; the player finds the sound again by name. */
    int si = -1;
    for (uint32_t i = 0; i < scnt; i++) {
        uint32_t e = bu32(&in, stab + 8 + i * 8ull);
        if ((e + 0x17ull > in.n) || in.b[e + 0x16] != 1) continue;
        uint32_t fid = bu32(&in, e + 4ull);
        if (fid >= fcnt) continue;
        uint32_t fe = bu32(&in, ftab + 8 + fid * 8ull);
        uint32_t pt = bu32(&in, fe + 0x18ull), pc = bu32(&in, pt);
        if (!pc || pc == 0xFFFFFFFFu) continue;
        uint32_t pe = bu32(&in, pt + 8ull), gid = bu32(&in, pe), idx = bu32(&in, pe + 4ull);
        if (gid >= gcnt) continue;
        uint32_t ge = bu32(&in, gtab + 8 + gid * 8ull);
        uint32_t it = bu32(&in, ge + 0x24ull), ic = bu32(&in, it);
        if (idx >= ic || ic > in.n / 8) continue;
        uint32_t ie = bu32(&in, it + 8 + idx * 8ull);
        uint64_t off = (uint64_t)bu32(&in, ge + 0x10ull) + bu32(&in, ie + 4ull);
        uint32_t sz = bu32(&in, ie + 8ull);
        if (!sz || sz == 0xFFFFFFFFu || off + sz > f->size) continue;
        const char *sn = rsar_name(&sy, bu32(&in, e));
        char nm[180];
        if (sn) snprintf(nm, sizeof(nm), "%s.seq", sn);
        else snprintf(nm, sizeof(nm), "sound%04u.seq", (unsigned)i);
        tree_clean_name(nm);
        if (si < 0 && (si = tree_add(&a->tree, 0, "Sequences", true, 0, 0, 0, 0)) < 0) break;
        tree_add(&a->tree, si, nm, false, sz, off, 0, 0);
    }

    free(fname);
    if (a->tree.count <= 1) a = raw_fail(a);
out:
    free(done);
    free(sb);
    free(ib);
    return a;
}

/* ---------------------------------------------------------------- BRWAR */

bool arc_brwar_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x20];
    return read_head(f, h, 0x20) && !memcmp(h, "RWAR", 4) && be16(h + 4) == 0xFEFF && f->size >= 0x40;
}

archive_t *arc_brwar_open(const src_t *f, const char *name)
{
    (void)name;
    archive_t *a = raw_new("BRWAR", f);
    if (!a) return NULL;
    rsar_rwar(a, f, 0, f->size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)f->size, 0);
    if (a->tree.count <= 1) return raw_fail(a);
    return a;
}

/* ---------------------------------------------------------------- HAL SSM */

/* After nintoolbox's hal_ssm.inc (vgmstream ngc_ssm.c): u32 head size, data size, sound count, id; then
 * per sound {u32 channels, u32 rate, per channel 0x40: loop flag, loop / end / first nibble (absolute in the
 * data block at the end of the file), 16 coefficients at 0x10, history at 0x34}. */
bool arc_ssm_probe(const src_t *f, const char *name)
{
    uint8_t h[0x10];
    if (!name || !arc_name_has_ext(name, ".ssm") || !read_head(f, h, 0x10)) return false;
    uint32_t head = be32(h), dsz = be32(h + 4), n = be32(h + 8);
    return n && n <= 0x1000 && be32(h + 12) <= 0x1000 && head >= n * 0x48 && 0x10ull + head + dsz <= f->size;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

archive_t *arc_ssm_open(const src_t *f, const char *name)
{
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;
    uint32_t head = be32(d), dsz = be32(d + 4), n = be32(d + 8);
    uint64_t data = len - dsz;
    char stem[64];
    static const char *const ext[] = { ".ssm", NULL };
    inner_name(name, stem, sizeof(stem), ext);

    /* first pass: size of every DSP file */
    uint64_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *blob = NULL;
        archive_t *a = NULL;
        if (pass) {
            blob = total && total <= MAX_OUT ? malloc(total) : NULL;
            a = blob ? archive_alloc("HAL SSM") : NULL;
            if (!a) {
                free(blob);
                break;
            }
            a->read = raw_read;
        }
        uint64_t pos = 0, off = 0x10;
        for (uint32_t i = 0; i < n && off + 8 <= 0x10ull + head; i++) {
            uint32_t ch = be32(d + off), rate = be32(d + off + 4);
            if (ch < 1 || ch > 2 || off + 8 + ch * 0x40ull > 0x10ull + head) break;
            const uint8_t *r0 = d + off + 8;
            off += 8 + ch * 0x40;
            uint32_t first = be32(r0 + 0x0C), end = be32(r0 + 8);
            if (end <= first || !rate || rate > 96000) continue;
            uint32_t nib = end - first, ns = nib / 16 * 14 + (nib % 16 > 2 ? nib % 16 - 2 : 0);
            uint32_t bytes = (nib + 1) / 2;
            for (uint32_t c = 0; c < ch; c++) {
                const uint8_t *rc = r0 + c * 0x40;
                uint64_t so = data + be32(rc + 0x0C) / 16 * 8;
                if (so + bytes > len) continue;
                if (pass) {
                    uint8_t *o = blob + pos;
                    memset(o, 0, 0x60);
                    put32(o, ns);
                    put32(o + 4, nib);
                    put32(o + 8, rate);
                    put32(o + 0x10, 2);
                    put32(o + 0x14, nib + 1);
                    put32(o + 0x18, 2);
                    memcpy(o + 0x1C, rc + 0x10, 32);
                    memcpy(o + 0x40, rc + 0x34, 4);
                    memcpy(o + 0x60, d + so, bytes);
                    char nm[96];
                    if (ch == 1) snprintf(nm, sizeof(nm), "%s_%03u.dsp", stem, (unsigned)i);
                    else snprintf(nm, sizeof(nm), "%s_%03u_%c.dsp", stem, (unsigned)i, c ? 'R' : 'L');
                    tree_add(&a->tree, 0, nm, false, 0x60 + bytes, pos, 0, 0);
                }
                pos += 0x60 + bytes;
            }
        }
        if (!pass) {
            total = pos;
            continue;
        }
        free(d);
        a->base = *f;
        a->src = src_from_memory(blob, total);
        if (!a->src.ctx || a->tree.count <= 1) {
            if (!a->src.ctx) free(blob);
            memset(&a->base, 0, sizeof(a->base));
            archive_close(a);
            return NULL;
        }
        return a;
    }
    free(d);
    return NULL;
}

/* ---------------------------------------------------------------- CCF */

bool arc_ccf_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[32];
    return read_head(f, h, 32) && !memcmp(h, "CCF\0", 4) && be32(h + 16) && be32(h + 20) &&
           be32(h + 20) < 0x10000 && 32 + be32(h + 20) * 32ull <= f->size;
}

/* Header: "CCF\0", 12 unknown bytes, offset unit, count; then 32 byte entries:
 * name[20], offset / unit, stored size, original size (equal when not zlib). */
archive_t *arc_ccf_open(const src_t *f, const char *name)
{
    (void)name;
    uint32_t len;
    uint8_t *d = read_all(f, &len);
    if (!d) return NULL;

    uint32_t unit = be32(d + 16), n = be32(d + 20);
    archive_t *a = NULL;
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += be32(d + 32 + i * 32 + 28);
    uint8_t *blob = total <= MAX_OUT ? malloc(total ? total : 1) : NULL;
    if (!blob) goto out;
    a = archive_alloc("CCF");
    if (!a) goto out;
    a->read = raw_read;

    uint64_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = d + 32 + i * 32;
        uint64_t off = (uint64_t)be32(e + 20) * unit;
        uint32_t csz = be32(e + 24), dsz = be32(e + 28);
        if (off + csz > len) continue;
        bool ok;
        if (csz == dsz) {
            memcpy(blob + pos, d + off, dsz);
            ok = true;
        } else {
            uLongf got = dsz;
            ok = uncompress(blob + pos, &got, d + off, csz) == Z_OK && got == dsz;
        }
        if (!ok) continue;
        char nm[24];
        memcpy(nm, e, 20);
        nm[20] = 0;
        tree_clean_name(nm);
        if (!nm[0]) snprintf(nm, sizeof(nm), "file%u", (unsigned)i);
        tree_add(&a->tree, 0, nm, false, dsz, pos, 0, 0);
        pos += dsz;
    }

    a->base = *f;
    a->src = src_from_memory(blob, total);
    if (!a->src.ctx) {
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        a = NULL;
        goto out;
    }
    blob = NULL;
out:
    free(blob);
    free(d);
    return a;
}

/* ---------------------------------------------------------------- Brawl PAC */

bool arc_pac_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x60];
    return read_head(f, h, 0x60) && !memcmp(h, "ARC\0", 4) && be16(h + 6) && be16(h + 6) < 4096;
}

/* "ARC\0", version, member count, then a 0x40 byte header; each member is a
 * 0x20 byte header (type, index, size, group, redirect) and its data, padded
 * to 0x20. Members are named after their type and given the extension their
 * contents call for (most are BRRES, MSBin or nested ARCs). */
archive_t *arc_pac_open(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x40];
    if (!read_head(f, h, 0x40)) return NULL;
    uint32_t n = be16(h + 6);
    archive_t *a = raw_new("PAC", f);
    if (!a) return NULL;

    static const char *const types[] = { "None",        "MiscData",      "ModelData",
                                         "TextureData", "AnimationData", "SceneData",
                                         "Type6",       "GroupData",     "EffectData" };

    uint64_t pos = 0x40;
    for (uint32_t i = 0; i < n && pos + 0x20 <= f->size; i++) {
        uint8_t e[0x24];
        if (!src_read_full(f, pos, e, 0x20)) break;
        uint32_t type = be16(e), idx = be16(e + 2), sz = be32(e + 4);
        uint64_t at = pos + 0x20;
        if (at + sz > f->size) break;

        uint8_t m[4] = { 0 };
        if (sz >= 4) src_read_full(f, at, m, 4);
        const char *ext = !memcmp(m, "bres", 4) ? ".brres" : !memcmp(m, "ARC\0", 4) ? ".pac" :
                          !memcmp(m, "REFF", 4) ? ".breff" : !memcmp(m, "REFT", 4) ? ".breft" :
                          !memcmp(m, "RSAR", 4) ? ".brsar" :
                          m[0] == 0x10 || m[0] == 0x11 ? ".lz" : ".bin";

        char nm[64];
        snprintf(nm, sizeof(nm), "%s[%u]%s", type < 9 ? types[type] : "Type", (unsigned)idx, ext);
        if (tree_find_child(&a->tree, 0, nm) >= 0)
            snprintf(nm, sizeof(nm), "%s[%u]-%u%s", type < 9 ? types[type] : "Type", (unsigned)idx,
                     (unsigned)i, ext);
        if (sz) tree_add(&a->tree, 0, nm, false, sz, at, 0, 0);
        pos = (at + sz + 0x1F) & ~0x1Full;
    }

    if (a->tree.count <= 1) return raw_fail(a);
    return a;
}

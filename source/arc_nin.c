/*
 * First-party GameCube / Wii containers, read-only, for browsing:
 *   Yaz0 (.szs, .yaz0)  and  Yay0     compressed; a compressed archive opens as the archive inside
 *   RARC (.arc, .rarc)                GameCube / Wii resource archive
 *   BRRES (.brres)                    NW4R resource file: its folders and sub-files (TEX0, PLT0, MDL0, ...)
 * Layouts follow quatric's nintoolbox (Wiimm's SZS tools lineage).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"

#define MAX_OUT (48u * 1024 * 1024)

static bool read_head(const src_t *f, uint8_t *h, uint32_t n)
{
    return f->size >= n && src_read_full(f, 0, h, n);
}

static uint8_t *read_all(const src_t *f, uint32_t *len)
{
    if (f->size > MAX_OUT) return NULL;
    uint8_t *b = malloc(f->size ? f->size : 1);
    if (b && !src_read_full(f, 0, b, (uint32_t)f->size)) { free(b); return NULL; }
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
    return read_head(f, h, 16) && (!memcmp(h, "Yaz0", 4) || !memcmp(h, "Yay0", 4)) && be32(h + 4) && be32(h + 4) <= MAX_OUT;
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
            if (!n) { if (ip >= len) return false; n = in[ip++] + 0x12; }
            else n += 2;
            if (dist > op) return false;
            while (n-- && op < osz) { out[op] = out[op - dist]; op++; }
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
        if (!bits) { if (mp + 4 > len) return false; mask = be32(in + mp); mp += 4; bits = 32; }
        if (mask & 0x80000000u) {
            if (chunk >= len) return false;
            out[op++] = in[chunk++];
        } else {
            if (link + 2 > len) return false;
            uint16_t l = be16(in + link); link += 2;
            uint32_t dist = (l & 0xFFF) + 1, n = l >> 12;
            if (!n) { if (chunk >= len) return false; n = in[chunk++] + 0x12; }
            else n += 2;
            if (dist > op) return false;
            while (n-- && op < osz) { out[op] = out[op - dist]; op++; }
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
    bool ok = out && (!memcmp(in, "Yaz0", 4) ? yaz0_decode(in, len, out, osz) : yay0_decode(in, len, out, osz));
    free(in);
    if (!ok) { free(out); return NULL; }
    static const char *const exts[] = { ".szs", ".yaz0", ".yay0", ".szp", ".carc", NULL };
    char inner[256];
    inner_name(name, inner, sizeof(inner), exts);
    return archive_open_memory(out, osz, inner);
}

/* ---------------------------------------------------------------- RARC */

typedef struct { uint64_t base; } rawpriv;

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
    uint32_t info_len = be32(h + 0x0C);          /* data offset, relative to 0x20 = size of the info part */
    if (info_len < 0x20 || info_len > 8 * 1024 * 1024 || 0x20 + (uint64_t)info_len > f->size) return NULL;
    uint8_t *b = malloc(0x20 + info_len);
    if (!b || !src_read_full(f, 0, b, 0x20 + info_len)) { free(b); return NULL; }
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
    if ((uint64_t)r.nodes + r.nnodes * 0x10ull > r.len || (uint64_t)r.ents + r.nents * 0x14ull > r.len ||
        (uint64_t)r.strs + r.strsz > r.len || !r.nnodes)
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
    if (l > len - off || l >= n) l = (uint32_t)strnlen((const char *)b + off, len - off < n - 1 ? len - off : n - 1);
    memcpy(out, b + off, l);
    out[l] = '\0';
    tree_clean_name(out);
    return out[0] != '\0';
}

/* One index group: entry 0 is the root, then `count` entries of 16 bytes. */
static bool brres_group(archive_t *a, const uint8_t *b, uint32_t len, uint32_t grp, int parent, int depth)
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
                ext[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
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

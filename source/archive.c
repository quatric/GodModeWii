/*
 * Read-only archives: U8, LZ10 / LZ11, plus the shared tree / source / CBC helpers.
 * U8 and the LZ formats follow Nintendo's published layouts as implemented by
 * libWiiSharp / Sharpii (U8, IMD5, LZ77) and wad2bin (IMET / u8.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archive.h"

const char *g_archive_err;
static archive_key_fn prng_provider;
static archive_wfs_key_fn wfs_provider;
static int open_depth;

/* ---------------------------------------------------------------- bytes */

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }

bool arc_name_has_ext(const char *name, const char *ext)
{
    if (!name) return false;
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && strcasecmp(name + nl - el, ext) == 0;
}

/* ---------------------------------------------------------------- keys */

/* Fixed Wii SD card key / IV for data.bin and content.bin headers (same on every console). */
const uint8_t ARC_SD_KEY[16] = { 0xAB, 0x01, 0xB9, 0xD8, 0xE1, 0x62, 0x2B, 0x08, 0xAF, 0xBA, 0xD8, 0x4D, 0xBF, 0xC2, 0xA5, 0x5D };
const uint8_t ARC_SD_IV[16]  = { 0x21, 0x67, 0x12, 0xE6, 0xAA, 0x1F, 0x68, 0x9F, 0x95, 0xC5, 0xA2, 0x23, 0x24, 0xDC, 0x6A, 0x98 };

/* Common keys 0 (Wii) and 1 (Korean Wii) decrypt the title key in a WAD's ticket. */
static const uint8_t COMMON_KEYS[2][16] = {
    { 0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4, 0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7 },
    { 0x63, 0xB8, 0x2B, 0xB4, 0xF4, 0x61, 0x4E, 0x2E, 0x13, 0xF2, 0xFE, 0xFB, 0xBA, 0x4C, 0x9B, 0x7E },
};

bool arc_common_key(int index, uint8_t key[16])
{
    if (index < 0 || index > 1) return false;
    memcpy(key, COMMON_KEYS[index], 16);
    return true;
}

void archive_set_prng_key_provider(archive_key_fn fn) { prng_provider = fn; }

void archive_set_wfs_key_provider(archive_wfs_key_fn fn) { wfs_provider = fn; }

bool arc_wfs_key_at(int index, uint8_t key[16])
{
    return wfs_provider && wfs_provider(index, key);
}

bool arc_prng_key(uint8_t key[16])
{
    return prng_provider && prng_provider(key);
}

/* ---------------------------------------------------------------- sources */

bool src_read_full(const src_t *s, uint64_t off, void *buf, uint32_t len)
{
    uint8_t *p = buf;
    while (len) {
        int32_t n = s->read(s->ctx, off, p, len);
        if (n <= 0) return false;
        off += n; p += n; len -= n;
    }
    return true;
}

typedef struct { uint8_t *data; uint64_t size; } memsrc;

static int32_t mem_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    memsrc *m = ctx;
    if (off >= m->size) return 0;
    if (len > m->size - off) len = (uint32_t)(m->size - off);
    memcpy(buf, m->data + off, len);
    return (int32_t)len;
}

static void mem_close(void *ctx)
{
    memsrc *m = ctx;
    free(m->data);
    free(m);
}

src_t src_from_memory(uint8_t *data, uint64_t size)
{
    src_t s;
    memset(&s, 0, sizeof(s));
    memsrc *m = malloc(sizeof(*m));
    if (!m) { free(data); return s; }
    m->data = data;
    m->size = size;
    s.read = mem_read;
    s.close = mem_close;
    s.ctx = m;
    s.size = size;
    return s;
}

static void src_close(src_t *s)
{
    if (s->close && s->ctx) s->close(s->ctx);
    memset(s, 0, sizeof(*s));
}

/* ---------------------------------------------------------------- tree */

bool tree_init(tree_t *t)
{
    memset(t, 0, sizeof(*t));
    return tree_add(t, -1, "", true, 0, 0, 0, 0) == 0;
}

void tree_free(tree_t *t)
{
    for (int i = 0; i < t->count; i++) free(t->n[i].name);
    free(t->n);
    memset(t, 0, sizeof(*t));
}

void tree_clean_name(char *name)
{
    for (char *p = name; *p; p++)
        if (*p == '/' || *p == '|' || *p == '\\' || (unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F)
            *p = '_';
}

int tree_add(tree_t *t, int parent, const char *name, bool is_dir, uint64_t size, uint64_t a, uint64_t b, uint32_t c)
{
    if (t->count == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 64;
        tnode *nn = realloc(t->n, (size_t)ncap * sizeof(tnode));
        if (!nn) return -1;
        t->n = nn;
        t->cap = ncap;
    }
    char *copy = strdup(name);
    if (!copy) return -1;
    tree_clean_name(copy);
    tnode *n = &t->n[t->count];
    n->name = copy;
    n->parent = parent;
    n->is_dir = is_dir;
    n->loaded = false;
    n->size = size;
    n->a = a;
    n->b = b;
    n->c = c;
    return t->count++;
}

int tree_find_child(const tree_t *t, int parent, const char *name)
{
    for (int i = 1; i < t->count; i++)
        if (t->n[i].parent == parent && !strcasecmp(t->n[i].name, name))
            return i;
    return -1;
}

int tree_find(const tree_t *t, const char *rel)
{
    int cur = 0;
    char part[256];
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t l = strcspn(p, "/");
        if (l >= sizeof(part)) return -1;
        memcpy(part, p, l);
        part[l] = '\0';
        p += l;
        if (!t->n[cur].is_dir) return -1;
        cur = tree_find_child(t, cur, part);
        if (cur < 0) return -1;
    }
    return cur;
}

/* Adds "a/b/c", creating any missing folders on the way. */
int tree_add_path(tree_t *t, const char *path, bool is_dir, uint64_t size, uint64_t a, uint64_t b, uint32_t c)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    int cur = 0;
    char *p = buf;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) return cur;
        char *slash = strchr(p, '/');
        bool last = !slash || !slash[strspn(slash, "/")];
        if (slash) *slash = '\0';
        int child = tree_find_child(t, cur, p);
        if (child < 0) {
            child = tree_add(t, cur, p, last ? is_dir : true, last && !is_dir ? size : 0, last ? a : 0, last ? b : 0, last ? c : 0);
            if (child < 0) return -1;
        } else if (last && !is_dir && !t->n[child].is_dir) {
            t->n[child].size = size; t->n[child].a = a; t->n[child].b = b; t->n[child].c = c;
        }
        if (last) return child;
        cur = child;
        p = slash + 1;
    }
}

/* ---------------------------------------------------------------- archive object */

archive_t *archive_alloc(const char *kind)
{
    archive_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->kind = kind;
    if (!tree_init(&a->tree)) { free(a); return NULL; }
    return a;
}

void archive_close(archive_t *a)
{
    if (!a) return;
    if (a->destroy) a->destroy(a);
    tree_free(&a->tree);
    if (a->src.ctx && a->src.ctx != a->base.ctx) src_close(&a->src);
    src_close(&a->base);
    free(a);
}

bool archive_expand(archive_t *a, int node)
{
    if (node < 0 || node >= a->tree.count || !a->tree.n[node].is_dir) return false;
    if (a->tree.n[node].loaded || !a->expand) return true;
    a->tree.n[node].loaded = true;
    return a->expand(a, node);
}

int archive_find(archive_t *a, const char *rel)
{
    int cur = 0;
    char part[256];
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t l = strcspn(p, "/");
        if (l >= sizeof(part)) return -1;
        memcpy(part, p, l);
        part[l] = '\0';
        p += l;
        if (!a->tree.n[cur].is_dir || !archive_expand(a, cur)) return -1;
        cur = tree_find_child(&a->tree, cur, part);
        if (cur < 0) return -1;
    }
    return cur;
}

/* ---------------------------------------------------------------- CBC streams */

/* Plaintext bytes [off, off+len) of a CBC stream stored at enc_off in src. Works from any
 * 16-byte block because the previous ciphertext block is the IV. */
int32_t cbc_stream_read(const src_t *src, const aes128_t *ctx, const uint8_t iv0[16], uint64_t enc_off,
                        uint64_t plain_size, uint64_t off, void *buf, uint32_t len)
{
    if (off >= plain_size) return 0;
    if (len > plain_size - off) len = (uint32_t)(plain_size - off);
    enum { CHUNK = 16384 };
    static uint8_t tmp[CHUNK + 16];
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t bs = pos & ~(uint64_t)15;
        uint64_t want_end = (pos + (len - done) + 15) & ~(uint64_t)15;
        uint64_t be = want_end;
        if (be - bs > CHUNK) be = bs + CHUNK;
        uint8_t iv[16];
        uint32_t nbytes = (uint32_t)(be - bs);
        if (bs == 0) {
            memcpy(iv, iv0, 16);
            if (!src_read_full(src, enc_off, tmp + 16, nbytes)) return done ? (int32_t)done : -1;
        } else {
            if (!src_read_full(src, enc_off + bs - 16, tmp, nbytes + 16)) return done ? (int32_t)done : -1;
            memcpy(iv, tmp, 16);
        }
        aes128_cbc_decrypt(ctx, iv, tmp + 16, tmp + 16, nbytes);
        uint32_t skip = (uint32_t)(pos - bs);
        uint32_t take = nbytes - skip;
        if (take > len - done) take = len - done;
        memcpy(dst + done, tmp + 16 + skip, take);
        done += take;
    }
    return (int32_t)done;
}

/* ---------------------------------------------------------------- LZ10 / LZ11 */

/* Nintendo LZ77: type byte 0x10 / 0x11, 24-bit size (0 = a 32-bit size follows). */
static bool lz_header(const uint8_t *h, uint32_t avail, uint8_t *type, uint32_t *size, uint32_t *hdr_len)
{
    if (avail < 4 || (h[0] != 0x10 && h[0] != 0x11)) return false;
    *type = h[0];
    *size = h[1] | (h[2] << 8) | ((uint32_t)h[3] << 16);
    *hdr_len = 4;
    if (*size == 0) {
        if (avail < 8) return false;
        *size = le32(h + 4);
        *hdr_len = 8;
    }
    return *size != 0;
}

static bool lz_decode(const uint8_t *in, uint32_t in_len, uint8_t type, uint32_t out_size, uint32_t pos, uint8_t *out)
{
    uint32_t op = 0;
    while (op < out_size) {
        if (pos >= in_len) return false;
        uint8_t flags = in[pos++];
        for (int i = 0; i < 8 && op < out_size; i++, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (pos >= in_len) return false;
                out[op++] = in[pos++];
                continue;
            }
            uint32_t len, disp;
            if (pos >= in_len) return false;
            uint8_t b0 = in[pos++];
            if (type == 0x10) {
                if (pos >= in_len) return false;
                uint8_t b1 = in[pos++];
                len = (b0 >> 4) + 3;
                disp = (((b0 & 0xF) << 8) | b1) + 1;
            } else {
                uint8_t ind = b0 >> 4;
                if (ind == 0) {
                    if (pos + 2 > in_len) return false;
                    uint8_t b1 = in[pos], b2 = in[pos + 1];
                    pos += 2;
                    len = (((b0 & 0xF) << 4) | (b1 >> 4)) + 0x11;
                    disp = (((b1 & 0xF) << 8) | b2) + 1;
                } else if (ind == 1) {
                    if (pos + 3 > in_len) return false;
                    uint8_t b1 = in[pos], b2 = in[pos + 1], b3 = in[pos + 2];
                    pos += 3;
                    len = (((b0 & 0xF) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111;
                    disp = (((b2 & 0xF) << 8) | b3) + 1;
                } else {
                    if (pos >= in_len) return false;
                    uint8_t b1 = in[pos++];
                    len = ind + 1;
                    disp = (((b0 & 0xF) << 8) | b1) + 1;
                }
            }
            if (disp > op) return false;
            for (uint32_t k = 0; k < len && op < out_size; k++, op++)
                out[op] = out[op - disp];
        }
    }
    return true;
}

static bool read_whole(const src_t *file, uint8_t **out, uint32_t *len)
{
    if (file->size > ARC_MAX_DECOMPRESSED) { g_archive_err = "File is too big to decompress in memory"; return false; }
    uint8_t *buf = malloc(file->size ? (size_t)file->size : 1);
    if (!buf) { g_archive_err = "Out of memory"; return false; }
    if (!src_read_full(file, 0, buf, (uint32_t)file->size)) { free(buf); g_archive_err = "Read error"; return false; }
    *out = buf;
    *len = (uint32_t)file->size;
    return true;
}

/* In-memory LZ decode of buf (starting at skip bytes in); NULL on error. */
static uint8_t *lz_unpack(const uint8_t *buf, uint32_t len, uint32_t skip, uint32_t *out_size)
{
    uint8_t type;
    uint32_t size, hl;
    if (skip > len || !lz_header(buf + skip, len - skip, &type, &size, &hl)) return NULL;
    if (size > ARC_MAX_DECOMPRESSED) { g_archive_err = "Decompressed size is too big for memory"; return NULL; }
    uint8_t *out = malloc(size);
    if (!out) { g_archive_err = "Out of memory"; return NULL; }
    if (!lz_decode(buf, len, type, size, skip + hl, out)) {
        free(out);
        g_archive_err = "Corrupt compressed data";
        return NULL;
    }
    *out_size = size;
    return out;
}

static bool lz_ext(const char *name)
{
    return arc_name_has_ext(name, ".lz") || arc_name_has_ext(name, ".lz10") || arc_name_has_ext(name, ".lz11") ||
           arc_name_has_ext(name, ".lz77");
}

/* A raw LZ stream only counts when the name says so; "LZ77" prefixed data is recognised by itself. */
static bool probe_lz(const src_t *file, const char *name)
{
    uint8_t h[16];
    uint32_t n = file->size < sizeof(h) ? (uint32_t)file->size : (uint32_t)sizeof(h);
    if (n < 8 || !src_read_full(file, 0, h, n)) return false;
    if (!memcmp(h, "LZ77", 4)) {
        uint8_t t; uint32_t s, hl;
        return lz_header(h + 4, n - 4, &t, &s, &hl);
    }
    uint8_t t; uint32_t s, hl;
    return lz_ext(name) && lz_header(h, n, &t, &s, &hl);
}

static void strip_name(const char *name, char *out, size_t outsz)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    snprintf(out, outsz, "%s", base);
    if (lz_ext(out)) {
        char *dot = strrchr(out, '.');
        if (dot) *dot = '\0';
    }
    if (!out[0]) snprintf(out, outsz, "data.bin");
}

static archive_t *open_lz(const src_t *file, const char *name)
{
    uint8_t *raw;
    uint32_t rlen;
    if (!read_whole(file, &raw, &rlen)) return NULL;
    uint32_t skip = !memcmp(raw, "LZ77", 4) ? 4 : 0;
    uint32_t osz;
    uint8_t *out = lz_unpack(raw, rlen, skip, &osz);
    free(raw);
    if (!out) return NULL;
    char inner[256];
    strip_name(name, inner, sizeof(inner));
    /* a compressed archive shows its contents directly; anything else is one file */
    archive_t *a = archive_open_memory(out, osz, inner);
    return a;
}

/* ---------------------------------------------------------------- one-file archive */

static int32_t single_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    return a->src.read(a->src.ctx, off, buf, len);
}

archive_t *archive_open_single(uint8_t *data, uint64_t size, const char *fname)
{
    archive_t *a = archive_alloc("file");
    if (!a) { free(data); return NULL; }
    a->src = src_from_memory(data, size);
    if (!a->src.ctx) { tree_free(&a->tree); free(a); return NULL; }
    a->read = single_read;
    if (tree_add(&a->tree, 0, fname, false, size, 0, 0, 0) < 0) {
        src_close(&a->src);
        tree_free(&a->tree);
        free(a);
        return NULL;
    }
    return a;
}

/* ---------------------------------------------------------------- U8 */

#define U8_MAGIC 0x55AA382Du
#define MAX_U8_NODES 200000u

typedef struct { uint64_t base; } u8priv;

static int32_t u8_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    return a->src.read(a->src.ctx, n->a + off, buf, len);
}

static void u8_destroy(archive_t *a) { free(a->priv); }

/* Where the U8 data starts: directly, behind an IMET header, or behind an IMD5 header. */
static bool u8_locate(const src_t *file, uint64_t *base)
{
    uint8_t h[0x50];
    uint32_t n = file->size < sizeof(h) ? (uint32_t)file->size : (uint32_t)sizeof(h);
    if (n < 0x20 || !src_read_full(file, 0, h, n)) return false;
    if (be32(h) == U8_MAGIC) { *base = 0; return true; }
    uint8_t m[4];
    if (n >= 0x44 && !memcmp(h + 0x40, "IMET", 4) && src_read_full(file, 0x640, m, 4) && be32(m) == U8_MAGIC) {
        *base = 0x640;
        return true;
    }
    if (!memcmp(h, "IMD5", 4) && src_read_full(file, 0x20, m, 4) && be32(m) == U8_MAGIC) {
        *base = 0x20;
        return true;
    }
    return false;
}

static bool probe_u8(const src_t *file, const char *name)
{
    (void)name;
    uint64_t base;
    return u8_locate(file, &base);
}

static archive_t *open_u8(const src_t *file, const char *name)
{
    (void)name;
    uint64_t base;
    if (!u8_locate(file, &base)) return NULL;
    uint8_t h[0x10];
    if (!src_read_full(file, base, h, 0x10)) return NULL;
    uint32_t root_off = be32(h + 4), hdr_size = be32(h + 8);
    if (root_off != 0x20 || hdr_size < 0x20 + 12 || hdr_size > 64u * 1024 * 1024) return NULL;

    uint8_t *tab = malloc(hdr_size);
    if (!tab) { g_archive_err = "Out of memory"; return NULL; }
    if (!src_read_full(file, base + root_off, tab, hdr_size)) { free(tab); return NULL; }
    uint32_t count = be32(tab + 8);                 /* root node: its size is the node count */
    if (count < 1 || count > MAX_U8_NODES || (uint64_t)count * 12 > hdr_size) { free(tab); return NULL; }
    const char *names = (const char *)tab + (size_t)count * 12;
    uint32_t names_len = hdr_size - count * 12;

    archive_t *a = archive_alloc("U8");
    u8priv *pr = calloc(1, sizeof(*pr));
    if (!a || !pr) { free(tab); free(pr); archive_close(a); return NULL; }
    a->priv = pr;
    a->destroy = u8_destroy;
    a->read = u8_read;
    pr->base = base;

    struct { uint32_t end; int idx; } stack[64];
    int sp = 0;
    stack[sp].end = count;
    stack[sp].idx = 0;
    bool ok = true;
    for (uint32_t i = 1; i < count && ok; i++) {
        while (sp > 0 && i >= stack[sp].end) sp--;
        const uint8_t *nd = tab + (size_t)i * 12;
        uint32_t name_off = be32(nd) & 0x00FFFFFF;
        bool is_dir = nd[0] != 0;
        uint32_t x = be32(nd + 4), y = be32(nd + 8);
        if (name_off >= names_len) { ok = false; break; }
        char name[256];
        size_t ml = strnlen(names + name_off, names_len - name_off);
        if (ml >= sizeof(name)) ml = sizeof(name) - 1;
        memcpy(name, names + name_off, ml);
        name[ml] = '\0';
        if (!name[0]) snprintf(name, sizeof(name), "node_%u", (unsigned)i);
        if (is_dir) {
            if (y <= i || y > count || sp >= 63) { ok = false; break; }
            int idx = tree_add(&a->tree, stack[sp].idx, name, true, 0, 0, 0, 0);
            if (idx < 0) { ok = false; break; }
            sp++;
            stack[sp].end = y;
            stack[sp].idx = idx;
        } else {
            if (tree_add(&a->tree, stack[sp].idx, name, false, y, base + x, 0, 0) < 0) { ok = false; break; }
        }
    }
    free(tab);
    if (!ok) { archive_close(a); return NULL; }
    a->base = *file;
    a->src = a->base;
    return a;
}

/* ---------------------------------------------------------------- IMD5 wrapper */

static bool probe_imd5(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x28];
    if (file->size < 0x28 || !src_read_full(file, 0, h, sizeof(h))) return false;
    return !memcmp(h, "IMD5", 4);
}

/* IMD5 header (0x20 bytes) then the payload, LZ77-compressed or not. A U8 payload is opened directly. */
static archive_t *open_imd5(const src_t *file, const char *name)
{
    uint8_t *raw;
    uint32_t rlen;
    if (!read_whole(file, &raw, &rlen)) return NULL;
    if (rlen < 0x28 || memcmp(raw, "IMD5", 4)) { free(raw); return NULL; }
    uint32_t psize = be32(raw + 4);
    if (psize > rlen - 0x20) psize = rlen - 0x20;
    uint8_t *payload = NULL;
    uint32_t plen = 0;
    if (!memcmp(raw + 0x20, "LZ77", 4))
        payload = lz_unpack(raw + 0x20, psize, 4, &plen);
    else
        payload = NULL;
    if (!payload) {
        uint8_t ty; uint32_t sz, hl;
        if (psize >= 4 && lz_header(raw + 0x20, psize, &ty, &sz, &hl))
            payload = lz_unpack(raw + 0x20, psize, 0, &plen);
    }
    if (!payload) {
        payload = malloc(psize ? psize : 1);
        if (!payload) { free(raw); return NULL; }
        memcpy(payload, raw + 0x20, psize);
        plen = psize;
    }
    free(raw);
    char inner[256];
    const char *base = strrchr(name ? name : "", '/');
    snprintf(inner, sizeof(inner), "%s", base ? base + 1 : (name ? name : "payload.bin"));
    return archive_open_memory(payload, plen, inner);
}

/* ---------------------------------------------------------------- dispatch */

typedef struct {
    const char *kind;
    bool (*probe)(const src_t *, const char *);
    archive_t *(*open)(const src_t *, const char *);
} fmt_t;

static const fmt_t formats[] = {
    { "content.bin", arc_contentbin_probe, arc_contentbin_open },
    { "data.bin",    arc_databin_probe,    arc_databin_open },
    { "WAD",         arc_wad_probe,        arc_wad_open },
    { "VFF",         arc_vff_probe,        arc_vff_open },
    { "U8",          probe_u8,             open_u8 },
    { "IMD5",        probe_imd5,           open_imd5 },
    { "LZ",          probe_lz,             open_lz },
    { "WFS",         arc_wfs_probe,        arc_wfs_open },
};

bool archive_probe(const src_t *file, const char *name)
{
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        if (formats[i].probe(file, name))
            return true;
    return false;
}

archive_t *archive_open(const src_t *file, const char *name)
{
    g_archive_err = NULL;
    if (open_depth >= 4) { g_archive_err = "Archives are nested too deeply"; return NULL; }
    open_depth++;
    archive_t *a = NULL;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]) && !a; i++) {
        if (!formats[i].probe(file, name))
            continue;
        a = formats[i].open(file, name);
        /* formats that read straight from the file keep it (base == file); the ones that
         * decompress into memory are done with it, so close it now */
        if (a && a->base.ctx != file->ctx && file->close && file->ctx)
            file->close(file->ctx);
    }
    open_depth--;
    if (!a && !g_archive_err) g_archive_err = "Not a supported archive";
    return a;
}

/* An in-memory buffer that may itself be an archive; otherwise it becomes a one-file archive. */
archive_t *archive_open_memory(uint8_t *data, uint64_t size, const char *name)
{
    src_t s = src_from_memory(data, size);
    if (!s.ctx) return NULL;
    if (open_depth < 4 && archive_probe(&s, name)) {
        archive_t *a = archive_open(&s, name);
        if (a) return a;
    }
    /* not an archive: hand the buffer back as a single file (take it out of the src) */
    memsrc *m = s.ctx;
    uint8_t *d = m->data;
    m->data = NULL;
    free(m);
    return archive_open_single(d, size, name && name[0] ? name : "data.bin");
}

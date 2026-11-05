/*
 * Read-only archives: U8, LZ10 / LZ11, plus the shared tree / source / CBC helpers.
 * U8 and the LZ formats match Nintendo's published layouts as implemented by
 * libWiiSharp / Sharpii (U8, IMD5, LZ77) and wad2bin (IMET / u8.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "archive.h"
#include "qlz.h"

const char *g_archive_err;
static archive_key_fn prng_provider;
static archive_wfs_key_fn wfs_provider;
static int open_depth;

/* ---------------------------------------------------------------- bytes */

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

bool arc_name_has_ext(const char *name, const char *ext)
{
    if (!name) return false;
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && strcasecmp(name + nl - el, ext) == 0;
}

/* ---------------------------------------------------------------- keys */

/* Fixed Wii SD card key / IV for data.bin and content.bin headers (same on every console). */
const uint8_t ARC_SD_KEY[16] = { 0xAB, 0x01, 0xB9, 0xD8, 0xE1, 0x62, 0x2B, 0x08,
                                 0xAF, 0xBA, 0xD8, 0x4D, 0xBF, 0xC2, 0xA5, 0x5D };
const uint8_t ARC_SD_IV[16]  = { 0x21, 0x67, 0x12, 0xE6, 0xAA, 0x1F, 0x68, 0x9F,
                                 0x95, 0xC5, 0xA2, 0x23, 0x24, 0xDC, 0x6A, 0x98 };

/* Common keys 0 (Wii), 1 (Korean Wii) and 2 (vWii) decrypt the title key in a WAD's ticket. */
static const uint8_t COMMON_KEYS[3][16] = {
    { 0xEB, 0xE4, 0x2A, 0x22, 0x5E, 0x85, 0x93, 0xE4,
      0x48, 0xD9, 0xC5, 0x45, 0x73, 0x81, 0xAA, 0xF7 },
    { 0x63, 0xB8, 0x2B, 0xB4, 0xF4, 0x61, 0x4E, 0x2E,
      0x13, 0xF2, 0xFE, 0xFB, 0xBA, 0x4C, 0x9B, 0x7E },
    { 0x30, 0xBF, 0xC7, 0x6E, 0x7C, 0x19, 0xAF, 0xBB,
      0x23, 0x16, 0x33, 0x30, 0xCE, 0xD7, 0xC2, 0x8D },
};

bool arc_common_key(int index, uint8_t key[16])
{
    if (index < 0 || index > 2) return false;
    memcpy(key, COMMON_KEYS[index], 16);
    return true;
}

void archive_set_prng_key_provider(archive_key_fn fn) { prng_provider = fn; }

void archive_set_wfs_key_provider(archive_wfs_key_fn fn) { wfs_provider = fn; }

static archive_sibling_fn sibling_provider;
void archive_set_sibling_provider(archive_sibling_fn fn) { sibling_provider = fn; }
bool archive_open_sibling(const char *name, src_t *out)
{
    return sibling_provider && sibling_provider(name, out);
}

bool arc_wfs_key_at(int index, uint8_t key[16])
{
    return wfs_provider && wfs_provider(index, key);
}

bool arc_prng_key(uint8_t key[16])
{
    return prng_provider && prng_provider(key);
}

/* ---------------------------------------------------------------- sources */

void (*busy_hook)(void);

bool src_read_full(const src_t *s, uint64_t off, void *buf, uint32_t len)
{
    uint8_t *p = buf;
    while (len) {
        if (busy_hook) busy_hook();
        int32_t n = s->read(s->ctx, off, p, len > 0x40000 ? 0x40000 : len);
        if (n <= 0) return false;
        off += n;
        p += n;
        len -= n;
    }
    return true;
}

typedef struct {
    uint8_t *data;
    uint64_t size;
} memsrc;

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
    if (!m) {
        free(data);
        return s;
    }
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
        if (*p == '/' || *p == '|' || *p == '\\' || (unsigned char)*p < 0x20 ||
            (unsigned char)*p == 0x7F)
            *p = '_';
}

int tree_add(tree_t *t, int parent, const char *name, bool is_dir, uint64_t size, uint64_t a,
             uint64_t b, uint32_t c)
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
int tree_add_path(tree_t *t, const char *path, bool is_dir, uint64_t size, uint64_t a, uint64_t b,
                  uint32_t c)
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
            child = tree_add(t, cur, p, last ? is_dir : true, last && !is_dir ? size : 0,
                             last ? a : 0, last ? b : 0, last ? c : 0);
            if (child < 0) return -1;
        } else if (last && !is_dir && !t->n[child].is_dir) {
            t->n[child].size = size;
            t->n[child].a = a;
            t->n[child].b = b;
            t->n[child].c = c;
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
    if (!tree_init(&a->tree)) {
        free(a);
        return NULL;
    }
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
int32_t cbc_stream_read(const src_t *src, const aes128_t *ctx, const uint8_t iv0[16],
                        uint64_t enc_off, uint64_t plain_size, uint64_t off, void *buf,
                        uint32_t len)
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
            if (!src_read_full(src, enc_off + bs - 16, tmp, nbytes + 16))
                return done ? (int32_t)done : -1;
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

/* Nintendo LZ77: type byte 0x10 / 0x11, 24-bit size (0 = a 32-bit size comes next). */
static bool lz_header(const uint8_t *h, uint32_t avail, uint8_t *type, uint32_t *size,
                      uint32_t *hdr_len)
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

/* LZ_DONE: out_size bytes written; LZ_SHORT: the input ran out first; LZ_BAD: a copy reaches back
 * before the start. *end_pos is where the input was left. */
enum { LZ_BAD, LZ_DONE, LZ_SHORT };

static int lz_run(const uint8_t *in, uint32_t in_len, uint8_t type, uint32_t out_size, uint32_t pos,
                  uint8_t *out, uint32_t *end_pos)
{
    uint32_t op = 0;
    int r = LZ_DONE;
#define LZ_NEED(k) do { if (pos + (k) > in_len) { r = LZ_SHORT; goto out; } } while (0)
    while (op < out_size) {
        LZ_NEED(1);
        uint8_t flags = in[pos++];
        for (int i = 0; i < 8 && op < out_size; i++, flags <<= 1) {
            if (!(flags & 0x80)) {
                LZ_NEED(1);
                out[op++] = in[pos++];
                continue;
            }

            uint32_t len, disp;
            LZ_NEED(1);
            uint8_t b0 = in[pos];
            if (type == 0x10) {
                LZ_NEED(2);
                uint8_t b1 = in[pos + 1];
                pos += 2;
                len = (b0 >> 4) + 3;
                disp = (((b0 & 0xF) << 8) | b1) + 1;
            } else {
                uint8_t ind = b0 >> 4;
                if (ind == 0) {
                    LZ_NEED(3);
                    uint8_t b1 = in[pos + 1], b2 = in[pos + 2];
                    pos += 3;
                    len = (((b0 & 0xF) << 4) | (b1 >> 4)) + 0x11;
                    disp = (((b1 & 0xF) << 8) | b2) + 1;
                } else if (ind == 1) {
                    LZ_NEED(4);
                    uint8_t b1 = in[pos + 1], b2 = in[pos + 2], b3 = in[pos + 3];
                    pos += 4;
                    len = (((b0 & 0xF) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111;
                    disp = (((b2 & 0xF) << 8) | b3) + 1;
                } else {
                    LZ_NEED(2);
                    uint8_t b1 = in[pos + 1];
                    pos += 2;
                    len = ind + 1;
                    disp = (((b0 & 0xF) << 8) | b1) + 1;
                }
            }

            if (disp > op) {
                r = LZ_BAD;
                goto out;
            }
            for (uint32_t k = 0; k < len && op < out_size; k++, op++)
                out[op] = out[op - disp];
        }
    }
#undef LZ_NEED
out:
    if (end_pos) *end_pos = pos;
    return r;
}

/* An LZ10 / LZ11 stream with no telling name or contents: decode what the first bytes give (up to
 * 64 KiB out) and keep it only if no copy reaches back before the start, and, when the whole file
 * was read, it ends where the stream does (word padding aside). Random data fails this early. */
static bool lz_plausible(const uint8_t *h, uint32_t n, uint64_t size)
{
    uint8_t t;
    uint32_t s, hl, end;
    if (!lz_header(h, n, &t, &s, &hl) || s > ARC_MAX_DECOMPRESSED) return false;
    if (size > (uint64_t)s + s / 8 + hl + 32) return false;      /* LZ never grows data by more than 1/8 */
    if (s > 0x10000 && size < 64) return false;
    uint32_t cap = s < 0x10000 ? s : 0x10000;
    uint8_t *out = malloc(cap);
    if (!out) return false;
    int r = lz_run(h, n, t, cap, hl, out, &end);
    free(out);
    if (r == LZ_BAD) return false;
    if (r == LZ_SHORT) return n < size && n >= 64;               /* ran past the bytes read: fine if more follow */
    if (cap < s) return true;                                     /* 64 KiB decoded cleanly */
    return size <= n ? size - end <= 32 : true;
}

static bool lz_decode(const uint8_t *in, uint32_t in_len, uint8_t type, uint32_t out_size,
                      uint32_t pos, uint8_t *out)
{
    return lz_run(in, in_len, type, out_size, pos, out, NULL) == LZ_DONE;
}

static bool read_whole(const src_t *file, uint8_t **out, uint32_t *len)
{
    if (file->size > ARC_MAX_DECOMPRESSED) {
        g_archive_err = "File is too big to decompress in memory";
        return false;
    }
    uint8_t *buf = malloc(file->size ? (size_t)file->size : 1);
    if (!buf) {
        g_archive_err = "Out of memory";
        return false;
    }
    if (!src_read_full(file, 0, buf, (uint32_t)file->size)) {
        free(buf);
        g_archive_err = "Read error";
        return false;
    }
    *out = buf;
    *len = (uint32_t)file->size;
    return true;
}

/* ---------------------------------------------------------------- Huffman / RL / LZH8 */

/* The other Nintendo SDK ("CX") codecs share the LZ header: type byte, 24-bit
 * little endian size. Huffman 0x24 / 0x28 (4 / 8 bit symbols, as in the HOME
 * Menu files), run length 0x30 and LZH8 0x40 (Virtual Console, WiiWare).
 * Each decoder stops after `osz` bytes, so a probe can peek at the start. */

static bool huff_decode(const uint8_t *src, uint32_t n, uint32_t pos, uint8_t *out, uint32_t osz,
                        bool four)
{
    if (pos >= n) return false;
    uint32_t tsz = 2u * (src[pos] + 1u);
    if (tsz > n - pos) return false;

    const uint8_t *tree = src + pos, *bits = tree + tsz;
    uint32_t blen = n - pos - tsz, bp = 0, left = 0, op = 0, word = 0;
    int half = -1;

    while (op < osz) {
        uint32_t node = 1;
        uint8_t sym;
        for (;;) {
            if (node >= tsz) return false;
            if (!left) {
                if (blen - bp < 4) return false;
                word = le32(bits + bp);
                bp += 4;
                left = 32;
            }
            bool bit = (word >> (left - 1)) & 1;
            left--;
            uint8_t e = tree[node];
            uint32_t child = (node & ~1u) + 2 + 2 * (e & 0x3F) + (bit ? 1 : 0);
            if (child >= tsz) return false;
            if (e & (bit ? 0x40 : 0x80)) {
                sym = tree[child];
                break;
            }
            node = child;
        }

        if (!four) out[op++] = sym;
        else if (half < 0) half = sym & 15;
        else {
            out[op++] = (uint8_t)(half | (sym & 15) << 4);
            half = -1;
        }
    }

    return true;
}

static bool rl_decode(const uint8_t *src, uint32_t n, uint32_t pos, uint8_t *out, uint32_t osz)
{
    uint32_t op = 0;
    while (op < osz) {
        if (pos >= n) return false;
        uint8_t h = src[pos++];
        if (h & 0x80) {
            uint32_t len = (h & 0x7F) + 3u;
            if (pos >= n) return false;
            uint8_t b = src[pos++];
            for (uint32_t k = 0; k < len && op < osz; k++) out[op++] = b;
        } else {
            uint32_t len = (h & 0x7F) + 1u;
            if (pos + len > n) return false;
            for (uint32_t k = 0; k < len && op < osz; k++) out[op++] = src[pos + k];
            pos += len;
        }
    }
    return true;
}

typedef struct {
    const uint8_t *s;
    uint32_t n, pos;
    uint8_t pool;
    int left;
} bitrd;

static bool br_bits(bitrd *r, uint32_t cnt, uint32_t *v)
{
    uint32_t x = 0;
    while (cnt--) {
        if (!r->left) {
            if (r->pos >= r->n) return false;
            r->pool = r->s[r->pos++];
            r->left = 8;
        }
        x = x << 1 | ((r->pool >> --r->left) & 1);
    }
    *v = x;
    return true;
}

/* LZH8: two Huffman tables (9-bit lengths / literals, 5-bit displacement
 * lengths), after hcs's public decoder as used by nintoolbox. */
static bool lzh8_decode(const uint8_t *src, uint32_t n, uint32_t pos, uint8_t *out, uint32_t osz)
{
    uint16_t lt[1024];
    uint8_t dt[64];
    memset(lt, 0, sizeof(lt));
    memset(dt, 0, sizeof(dt));
    bitrd r = { src, n, pos, 0, 0 };
    if (pos + 2 > n) return false;
    uint32_t lbytes = (uint32_t)(src[pos] | src[pos + 1] << 8) * 4 + 4, lstart = pos;
    r.pos = pos + 2;
    for (uint32_t i = 1; r.pos - lstart < lbytes && i < 1024; i++) {
        uint32_t v;
        if (!br_bits(&r, 9, &v)) return false;
        lt[i] = (uint16_t)v;
    }

    r.pos = lstart + lbytes;
    r.left = 0;
    if (r.pos + 1 > n) return false;

    uint32_t dbytes = (src[r.pos] + 1u) * 4, dstart = r.pos;
    r.pos++;
    for (uint32_t i = 1; r.pos - dstart < dbytes && i < 64; i++) {
        uint32_t v;
        if (!br_bits(&r, 5, &v)) return false;
        dt[i] = (uint8_t)v;
    }

    r.pos = dstart + dbytes;
    r.left = 0;
    if (r.pos > n) return false;

    uint32_t op = 0;
    while (op < osz) {
        uint32_t lo = 1;
        for (;;) {
            uint32_t bit;
            if (!br_bits(&r, 1, &bit)) return false;
            uint32_t nx = (lo / 2 * 2) + ((lt[lo] & 0x7F) + 1) * 2 + bit;
            if (nx >= 1024) return false;
            if (lt[lo] & (0x100u >> bit)) {
                uint32_t len = lt[nx];
                if (len < 0x100) {
                    out[op++] = (uint8_t)len;
                    break;
                }

                len = (len & 0xFF) + 3;
                uint32_t dlo = 1;
                for (;;) {
                    uint32_t db;
                    if (!br_bits(&r, 1, &db)) return false;
                    uint32_t dn = (dlo / 2 * 2) + ((dt[dlo] & 7) + 1) * 2 + db;
                    if (dn >= 64) return false;
                    if (dt[dlo] & (0x10u >> db)) {
                        uint32_t dl = dt[dn], disp = 0;
                        if (dl) {
                            disp = 1;
                            for (uint32_t k = dl - 1; k; k--) {
                                uint32_t b2;
                                if (!br_bits(&r, 1, &b2)) return false;
                                disp = disp * 2 | b2;
                            }
                        }
                        if (disp + 1 > op) return false;
                        for (uint32_t k = 0; k < len && op < osz; k++, op++)
                            out[op] = out[op - disp - 1];
                        break;
                    }
                    dlo = dn;
                }
                break;
            }
            lo = nx;
        }
    }

    return true;
}

/* type byte, 24-bit size (0 = a 32-bit size comes next); every CX codec */
static bool cx_header(const uint8_t *h, uint32_t avail, uint8_t *type, uint32_t *size,
                      uint32_t *hdr_len)
{
    if (avail < 4) return false;
    uint8_t t = h[0];
    if (t != 0x10 && t != 0x11 && t != 0x24 && t != 0x28 && t != 0x30 && t != 0x40) return false;
    *type = t;
    *size = h[1] | (h[2] << 8) | ((uint32_t)h[3] << 16);
    *hdr_len = 4;
    if (*size == 0) {
        if (avail < 8) return false;
        *size = le32(h + 4);
        *hdr_len = 8;
    }
    return *size != 0;
}

static bool cx_decode(const uint8_t *in, uint32_t in_len, uint8_t type, uint32_t pos, uint8_t *out,
                      uint32_t osz)
{
    switch (type) {
    case 0x10: case 0x11: return lz_decode(in, in_len, type, osz, pos, out);
    case 0x24: case 0x28: return huff_decode(in, in_len, pos, out, osz, type == 0x24);
    case 0x30: return rl_decode(in, in_len, pos, out, osz);
    case 0x40: return lzh8_decode(in, in_len, pos, out, osz);
    }
    return false;
}

/* The first outn bytes a bare CX stream (LZ10 / LZ11 / Huffman / RL / LZH8) at h decodes to. */
bool arc_cx_peek(const uint8_t *h, uint32_t n, uint8_t *out, uint32_t outn)
{
    uint8_t t;
    uint32_t s, hl;
    return cx_header(h, n, &t, &s, &hl) && s >= outn && cx_decode(h, n, t, hl, out, outn);
}

/* In-memory decode of buf (starting at skip bytes in); NULL on error. */
static uint8_t *lz_unpack(const uint8_t *buf, uint32_t len, uint32_t skip, uint32_t *out_size)
{
    uint8_t type;
    uint32_t size, hl;
    if (skip > len || !cx_header(buf + skip, len - skip, &type, &size, &hl)) return NULL;
    if (size > ARC_MAX_DECOMPRESSED) {
        g_archive_err = "Decompressed size is too big for memory";
        return NULL;
    }
    uint8_t *out = malloc(size);
    if (!out) {
        g_archive_err = "Out of memory";
        return NULL;
    }
    if (!cx_decode(buf, len, type, skip + hl, out, size)) {
        free(out);
        g_archive_err = "Corrupt compressed data";
        return NULL;
    }
    *out_size = size;
    return out;
}

/* ---------------------------------------------------------------- Camelot (STPL) / zlib */

/* Camelot Software Planning's LZ (Mario Golf: Toadstool Tour, Mario Power Tennis ".stpl"), after nintoolbox's
 * lib-camelot.c: a codec byte (1 or 2), a 24-bit big endian output size, then flag bytes (MSB first); a clear
 * flag is a literal, a set one is a back reference { len:4 | dist-hi:4, dist-lo [, long length - 17] }. Its
 * window is zero-filled before the first byte, which early references use. Inside is a TPL texture bank. */
static bool camelot_header(const uint8_t *h, uint32_t avail, uint32_t *size)
{
    if (avail < 5 || (h[0] != 1 && h[0] != 2)) return false;
    *size = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    return *size != 0;
}

/* Decodes the first `want` bytes of a stream that holds `out_len`; a peek stops early. */
static bool camelot_decode(const uint8_t *src, uint32_t n, uint32_t out_len, uint8_t *out, uint32_t want)
{
    uint32_t sp = 4, dp = 0;
    while (sp < n && dp < want) {
        uint8_t flags = src[sp++];
        for (int bit = 0; bit < 8 && dp < want; bit++) {
            if (flags & (0x80 >> bit)) {
                if (sp + 2 > n) return false;
                uint8_t a = src[sp++], b = src[sp++];
                uint32_t back = ((uint32_t)(a >> 4) << 8) | b, len = a & 15;
                if (!len) {
                    if (sp >= n) return false;
                    len = src[sp++] + 17u;
                } else
                    len++;
                if (!back || len > out_len - dp) return false;
                for (; len && dp < want; len--, dp++) out[dp] = back <= dp ? out[dp - back] : 0;
            } else {
                if (sp >= n) return false;
                out[dp++] = src[sp++];
            }
        }
    }
    return dp == want;
}

static uint8_t *camelot_unpack(const uint8_t *src, uint32_t n, uint32_t *osz)
{
    uint32_t out_len;
    if (!camelot_header(src, n, &out_len)) return NULL;
    if (out_len > ARC_MAX_DECOMPRESSED) { g_archive_err = "Decompressed size is too big for memory"; return NULL; }
    uint8_t *out = malloc(out_len);
    if (!out) { g_archive_err = "Out of memory"; return NULL; }
    if (!camelot_decode(src, n, out_len, out, out_len)) {
        free(out);
        g_archive_err = "Corrupt Camelot data";
        return NULL;
    }
    *osz = out_len;
    return out;
}

/* zlib (78 xx) or gzip (1F 8B) data; the size is not stored, so the buffer grows */
static bool zlib_header(const uint8_t *h, uint32_t n)
{
    if (n < 6) return false;
    if (h[0] == 0x1F && h[1] == 0x8B && h[2] == 8) return true;
    return (h[0] & 0x0F) == 8 && (h[0] >> 4) <= 7 && ((h[0] << 8) | h[1]) % 31 == 0 && !(h[1] & 0x20);
}

static uint8_t *zlib_unpack(const uint8_t *src, uint32_t n, uint32_t *osz)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 15 + 32) != Z_OK) return NULL;       /* zlib or gzip, whichever it is */
    uint32_t cap = n < 0x4000 ? 0x10000 : n * 3;
    if (cap > ARC_MAX_DECOMPRESSED) cap = ARC_MAX_DECOMPRESSED;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); g_archive_err = "Out of memory"; return NULL; }
    z.next_in = (Bytef *)src;
    z.avail_in = n;
    int rc;
    for (;;) {
        z.next_out = out + z.total_out;
        z.avail_out = cap - (uint32_t)z.total_out;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && !(rc == Z_BUF_ERROR && z.avail_out == 0)) break;
        if (z.avail_out == 0) {
            if (cap >= ARC_MAX_DECOMPRESSED) { rc = Z_MEM_ERROR; break; }
            uint32_t nc = cap > ARC_MAX_DECOMPRESSED / 2 ? ARC_MAX_DECOMPRESSED : cap * 2;
            uint8_t *no = realloc(out, nc);
            if (!no) { rc = Z_MEM_ERROR; break; }
            out = no;
            cap = nc;
        } else if (z.avail_in == 0)
            break;
    }
    uint32_t total = (uint32_t)z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END || !total) {
        free(out);
        g_archive_err = rc == Z_MEM_ERROR ? "Decompressed size is too big for memory" : "Corrupt zlib data";
        return NULL;
    }
    *osz = total;
    return out;
}

static bool camelot_ext(const char *name) { return arc_name_has_ext(name, ".stpl") || arc_name_has_ext(name, ".camelot"); }
static bool zlib_ext(const char *name)
{
    return arc_name_has_ext(name, ".z") || arc_name_has_ext(name, ".zlib") || arc_name_has_ext(name, ".zz") ||
           arc_name_has_ext(name, ".gz");
}
static bool qlz_ext(const char *name) { return arc_name_has_ext(name, ".qlz") || arc_name_has_ext(name, ".quicklz"); }

static bool lz_ext(const char *name)
{
    static const char *const ext[] = { ".lz",  ".lz10", ".lz11", ".lz77", ".huff", ".huf",     ".rl",   ".rle",
                                       ".lzh8", ".lh",  ".cx",   ".stpl", ".camelot", ".z",    ".zlib", ".zz",
                                       ".gz",  ".gfcp", ".alz",  ".alz1", ".qlz",  ".quicklz", NULL };
    for (int i = 0; ext[i]; i++)
        if (arc_name_has_ext(name, ext[i])) return true;
    return false;
}

/* Nintendo files a compressed stream may hold, by their first four bytes. */
static bool known_magic(const uint8_t *m)
{
    static const char *const tags[] = { "RSAR", "RSTM", "RWSD", "RBNK", "RSEQ", "RWAR", "RWAV",
                                        "bres", "RLYT", "RLAN", "RFNT", "RFNA", "RFNU", "REFF",
                                        "REFT", "RARC", "Yaz0", "Yay0", "MESG", "J3D2", "THP\0",
                                        "IMD5", "BNR1", "BNR2", "WIBN", "RKGD", "RKMD", "MsgS",
                                        "GFAC", "ASH0", "CRAR", "SARC", "TPL\0",
                                        "OCN.", "FJG\0", NULL };   /* Paon (DK: Barrel Blast) */
    for (int i = 0; tags[i]; i++)
        if (!memcmp(m, tags[i], 4)) return true;
    return be32(m) == 0x55AA382Du || be32(m) == 0x0020AF30u;
}

static void wbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wbe32(uint8_t *p, uint32_t v) { wbe16(p, (uint16_t)(v >> 16)); wbe16(p + 2, (uint16_t)v); }

/* ---- Camelot GX texture bank: what a Camelot stream usually holds (Mario Golf: Toadstool Tour, Mario
 * Power Tennis, We Love Golf!). It starts with the TPL magic, 0x0020AF30, but is not a TPL: u32 count,
 * u32 table offset, table of {u32 entry offset, u32 0}, entries of {u16 width, u16 height, u32 GX format,
 * u32 data offset, sampler}, every offset relative to the bank. Validity and layout follow nintoolbox's
 * lib-camtexbank.c, which declines the palettised formats (where their palette lives is unknown). */
static uint32_t gx_level(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t bw, bh, bpp;
    switch (fmt) {
    case 0: case 14: bw = 8, bh = 8, bpp = 4; break;
    case 1: case 2: bw = 8, bh = 4, bpp = 8; break;
    case 3: case 4: case 5: bw = 4, bh = 4, bpp = 16; break;
    case 6: bw = 4, bh = 4, bpp = 32; break;
    default: return 0;
    }
    return (w + bw - 1) / bw * ((h + bh - 1) / bh) * bw * bh * bpp / 8;
}

bool arc_camelot_bank_valid(const uint8_t *b, uint32_t n)
{
    if (n < 16 || be32(b) != 0x0020AF30u) return false;
    uint32_t count = be32(b + 4), tbl = be32(b + 8);
    if (!count || count > 0x1000 || tbl < 12 || (uint64_t)tbl + count * 8ull > n) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = be32(b + tbl + i * 8);
        if ((uint64_t)e + 16 > n) return false;
        uint32_t w = be16(b + e), h = be16(b + e + 2), need = gx_level(be32(b + e + 4), w, h);
        if (!w || !h || !need || (uint64_t)be32(b + e + 8) + need > n) return false;
    }
    return true;
}

/* The bank rewritten as a real TPL (one image per entry), so every viewer reads it the usual way. */
uint8_t *arc_camelot_bank_tpl(const uint8_t *b, uint32_t n, uint32_t *out_len)
{
    if (!arc_camelot_bank_valid(b, n)) return NULL;
    uint32_t count = be32(b + 4), tbl = be32(b + 8);
    uint64_t total = 12 + count * (8ull + 0x24);
    total = (total + 31) & ~31ull;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = be32(b + tbl + i * 8);
        total += (gx_level(be32(b + e + 4), be16(b + e), be16(b + e + 2)) + 31) & ~31u;
    }
    if (total > ARC_MAX_DECOMPRESSED) return NULL;
    uint8_t *t = calloc(1, (size_t)total);
    if (!t) return NULL;
    wbe32(t, 0x0020AF30u);
    wbe32(t + 4, count);
    wbe32(t + 8, 12);
    uint32_t hdr = 12 + count * 8, data = (uint32_t)((hdr + count * 0x24ull + 31) & ~31ull);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = be32(b + tbl + i * 8), w = be16(b + e), h = be16(b + e + 2), f = be32(b + e + 4);
        uint32_t need = gx_level(f, w, h), ih = hdr + i * 0x24;
        wbe32(t + 12 + i * 8, ih);
        wbe16(t + ih, (uint16_t)h);
        wbe16(t + ih + 2, (uint16_t)w);
        wbe32(t + ih + 4, f);
        wbe32(t + ih + 8, data);
        memcpy(t + data, b + be32(b + e + 8), need);
        data += (need + 31) & ~31u;
    }
    *out_len = (uint32_t)total;
    return t;
}

uint8_t *arc_camelot_unpack(const uint8_t *src, uint32_t n, uint32_t *out_len)
{
    return camelot_unpack(src, n, out_len);
}

/* Camelot data decoded, and a texture bank in it turned into a TPL. */
static uint8_t *camelot_unpack_tpl(const uint8_t *src, uint32_t n, uint32_t *osz)
{
    uint8_t *u = camelot_unpack(src, n, osz);
    uint32_t tl;
    uint8_t *t = u ? arc_camelot_bank_tpl(u, *osz, &tl) : NULL;
    if (!t) return u;
    free(u);
    *osz = tl;
    return t;
}

/* zlib data inflates cleanly from its first bytes: no error, and either 16 bytes out or the whole stream */
static bool zlib_peek(const uint8_t *h, uint32_t n)
{
    uint8_t out[64];
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 15 + 32) != Z_OK) return false;
    z.next_in = (Bytef *)h;
    z.avail_in = n;
    z.next_out = out;
    z.avail_out = sizeof(out);
    int rc = inflate(&z, Z_NO_FLUSH);
    uint32_t got = (uint32_t)z.total_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END ? got > 0 : (rc == Z_OK || rc == Z_BUF_ERROR) && got >= 16;
}

/* "LZ77" then an LZ10 / LZ11 stream, as on Wii banners; like nintoolbox, no other codec goes behind it */
static bool lz77_tagged(const uint8_t *h, uint32_t n)
{
    return n > 8 && !memcmp(h, "LZ77", 4) && (h[4] == 0x10 || h[4] == 0x11);
}

/* Good-Feel "GFCP": u32 mode at 8 (1 byte pair, 2 / 3 headerless LZ10), output size at 12, packed size at 16 */
static bool gfcp_header(const uint8_t *h, uint32_t n, uint64_t size)
{
    if (n < 20 || memcmp(h, "GFCP", 4)) return false;
    uint32_t mode = le32(h + 8), olen = le32(h + 12), zsz = le32(h + 16);
    return mode >= 1 && mode <= 3 && olen && olen <= ARC_MAX_DECOMPRESSED && 20ull + zsz <= size;
}

static uint8_t *gfcp_unpack(const uint8_t *src, uint32_t n, uint32_t *osz)
{
    uint32_t mode = le32(src + 8), olen = le32(src + 12), zsz = le32(src + 16);
    uint8_t *out = malloc(olen);
    if (!out) { g_archive_err = "Out of memory"; return NULL; }
    bool ok = mode == 1 ? arc_bpe_decode(out, olen, src + 20, zsz) : arc_lz10_raw(out, olen, src + 20, zsz);
    if (!ok) {
        free(out);
        g_archive_err = "Corrupt GFCP data";
        return NULL;
    }
    *osz = olen;
    return out;
}

/* Monster Games' "PMCr" (or "rMCP") wrapper: u32 stream length at 8 (either byte order), the QuickLZ
 * stream at 16; a bare QuickLZ stream's header holds its own exact length. */
static bool qlz_at(const uint8_t *h, uint32_t n, uint64_t size, uint32_t *skip, uint32_t *plen)
{
    uint32_t ds, hl;
    int v;
    if (n >= 25 && (!memcmp(h, "PMCr", 4) || !memcmp(h, "rMCP", 4)))
        for (int be = 0; be < 2; be++) {
            uint32_t l = be ? be32(h + 8) : le32(h + 8);
            if (l && l <= size - 16 && qlz_header(h + 16, l, &ds, &hl, &v)) {
                *skip = 16;
                *plen = l;
                return true;
            }
        }
    if (n >= 9 && size <= 0xFFFFFFFFu && qlz_header(h, (uint32_t)size, &ds, &hl, &v)) {
        *skip = 0;
        *plen = (uint32_t)size;
        return true;
    }
    return false;
}

enum { PK_NONE, PK_CX, PK_CAMELOT, PK_ZLIB, PK_GFCP, PK_ALZ1, PK_QLZ };

/* What compression the data in h (the first n bytes of a size-byte file) is in, if any. A name decides
 * Camelot and zlib on its own; "LZ77", gzip / zlib, GFCP, ALZ1 and PMCr carry their own magic, and a bare
 * QuickLZ header its exact length. A bare CX stream is only taken when what it decodes to starts with a
 * known Nintendo magic. A Camelot stream without its .stpl name only when it decodes to the start of a
 * texture bank: nintoolbox claims Camelot by name alone after its signature scan matched inside other
 * archives, so this never scans, it only looks at the first bytes out. `loose` (an explicit unwrap of
 * data the caller knows is compressed) takes any valid CX header. */
static int pack_kind(const char *name, const uint8_t *h, uint32_t n, uint64_t size, bool loose, uint32_t *skip,
                     uint32_t *plen)
{
    uint8_t t, peek[16];
    uint32_t s, hl;
    *skip = 0;
    *plen = n;
    if (camelot_ext(name)) return camelot_header(h, n, &s) ? PK_CAMELOT : PK_NONE;
    if (zlib_ext(name)) return zlib_header(h, n) ? PK_ZLIB : PK_NONE;
    if (lz77_tagged(h, n)) {
        *skip = 4;
        return lz_header(h + 4, n - 4, &t, &s, &hl) ? PK_CX : PK_NONE;
    }
    if (gfcp_header(h, n, size)) return PK_GFCP;
    if (n >= 8 && (!memcmp(h, "ALZ1", 4) || !memcmp(h, "ZALZ", 4))) return PK_ALZ1;
    if (qlz_at(h, n, size, skip, plen)) return PK_QLZ;
    if (qlz_ext(name)) return PK_NONE;
    if (cx_header(h, n, &t, &s, &hl)) {
        if (loose || lz_ext(name)) return PK_CX;
        if (s >= 16 && s <= ARC_MAX_DECOMPRESSED && (uint64_t)s * 64 >= size &&
            cx_decode(h, n, t, hl, peek, sizeof(peek)) && known_magic(peek))
            return PK_CX;
        if ((t == 0x10 || t == 0x11) && lz_plausible(h, n, size)) return PK_CX;
    }
    if (n >= 6 && ((h[0] == 0x1F && h[1] == 0x8B) || h[0] == 0x78) && zlib_header(h, n) && zlib_peek(h, n))
        return PK_ZLIB;
    if (camelot_header(h, n, &s) && s >= 16 && s <= ARC_MAX_DECOMPRESSED && (uint64_t)s * 64 >= size &&
        size <= (uint64_t)s + s / 8 + 64 && camelot_decode(h, n, s, peek, sizeof(peek)) &&
        be32(peek) == 0x0020AF30u && be32(peek + 4) && be32(peek + 4) <= 0x1000 && be32(peek + 8) >= 12 &&
        be32(peek + 8) < s)
        return PK_CAMELOT;
    return PK_NONE;
}

/* By name (.lz, .stpl, .gz ...), by magic ("LZ77", zlib, gzip, GFCP, ALZ1, QuickLZ) or by what is inside */
static bool probe_lz(const src_t *file, const char *name)
{
    uint8_t h[4096];
    uint32_t n = file->size < sizeof(h) ? (uint32_t)file->size : (uint32_t)sizeof(h), skip, plen;
    if (n < 6 || !src_read_full(file, 0, h, n)) return false;
    return pack_kind(name, h, n, file->size, false, &skip, &plen) != PK_NONE;
}

static void strip_name(const char *name, char *out, size_t outsz)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    snprintf(out, outsz, "%s", base);
    if (lz_ext(out)) {
        bool cam = camelot_ext(out);
        char *dot = strrchr(out, '.');
        if (dot) *dot = '\0';
        if (cam) {                                    /* a Camelot texture bank is a TPL */
            size_t l = strlen(out);
            if (l + 5 < outsz) memcpy(out + l, ".tpl", 5);
        }
    }
    if (!out[0]) snprintf(out, outsz, "data.bin");
}

/* The right decoder for a compressed single file, by its name or its contents. */
static uint8_t *wrap_unpack(const char *name, const uint8_t *buf, uint32_t len, uint32_t *osz)
{
    uint32_t skip, plen;
    uint8_t *u;
    switch (pack_kind(name, buf, len, len, true, &skip, &plen)) {
    case PK_CAMELOT: return camelot_unpack_tpl(buf, len, osz);
    case PK_ZLIB: return zlib_unpack(buf, len, osz);
    case PK_CX: return lz_unpack(buf, len, skip, osz);
    case PK_GFCP: return gfcp_unpack(buf, len, osz);
    case PK_ALZ1:
        if (!(u = arc_alz1_unpack(buf, len, osz))) g_archive_err = "Corrupt ALZ1 data";
        return u;
    case PK_QLZ:
        if (!(u = qlz_unpack(buf + skip, plen, ARC_MAX_DECOMPRESSED, osz))) g_archive_err = "Corrupt QuickLZ data";
        return u;
    }
    g_archive_err = zlib_ext(name) ? "Not zlib data" : camelot_ext(name) ? "Not Camelot data" : "Not compressed data";
    return NULL;
}

static archive_t *open_lz(const src_t *file, const char *name)
{
    uint8_t *raw;
    uint32_t rlen;
    if (!read_whole(file, &raw, &rlen)) return NULL;
    uint32_t osz;
    uint8_t *out = wrap_unpack(name, raw, rlen, &osz);
    free(raw);
    if (!out) return NULL;
    char inner[256];
    strip_name(name, inner, sizeof(inner));
    /* a compressed archive shows its contents directly; anything else is one file */
    return archive_open_memory(out, osz, inner);
}

/* "x.brres.lz" -> true, "x.brres" in out (when out is given). */
bool arc_lz_name(const char *name, char *out, size_t outsz)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    if (!lz_ext(base)) return false;
    if (out) strip_name(base, out, outsz);
    return true;
}

/* A whole LZ10 / LZ11 (optionally behind "LZ77") / Huffman / RL / LZH8, Camelot or zlib / gzip file
 * decompressed into a new 32-byte aligned buffer; NULL when it isn't one or doesn't decode. */
uint8_t *arc_lz_unwrap(const char *name, const uint8_t *buf, uint32_t len, uint32_t *out_len)
{
    uint32_t n;
    uint8_t *u = wrap_unpack(name, buf, len, &n);
    if (!u) return NULL;
    uint8_t *a = memalign(32, (n + 31) & ~31u);
    if (a) memcpy(a, u, n);
    free(u);
    if (a) *out_len = n;
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
    if (!a) {
        free(data);
        return NULL;
    }
    a->src = src_from_memory(data, size);
    if (!a->src.ctx) {
        tree_free(&a->tree);
        free(a);
        return NULL;
    }
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

typedef struct {
    uint64_t base;
} u8priv;

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
    uint8_t h[0x90];
    uint32_t n = file->size < sizeof(h) ? (uint32_t)file->size : (uint32_t)sizeof(h);
    if (n < 0x20 || !src_read_full(file, 0, h, n)) return false;
    if (be32(h) == U8_MAGIC) {
        *base = 0;
        return true;
    }
    uint8_t m[4];
    /* IMET at 0x40 (a disc's opening.bnr): U8 at 0x600;
     * at 0x80 (a channel's 00000000.app): 0x640 */
    for (uint32_t at = 0x40; at <= 0x80; at += 0x40)
        if (n >= at + 4 && !memcmp(h + at, "IMET", 4) && src_read_full(file, at + 0x5C0, m, 4) &&
            be32(m) == U8_MAGIC) {
            *base = at + 0x5C0;
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
    if (!tab) {
        g_archive_err = "Out of memory";
        return NULL;
    }
    if (!src_read_full(file, base + root_off, tab, hdr_size)) {
        free(tab);
        return NULL;
    }
    uint32_t count = be32(tab + 8);                 /* root node: its size is the node count */
    if (count < 1 || count > MAX_U8_NODES || (uint64_t)count * 12 > hdr_size) {
        free(tab);
        return NULL;
    }
    const char *names = (const char *)tab + (size_t)count * 12;
    uint32_t names_len = hdr_size - count * 12;

    archive_t *a = archive_alloc("U8");
    u8priv *pr = calloc(1, sizeof(*pr));
    if (!a || !pr) {
        free(tab);
        free(pr);
        archive_close(a);
        return NULL;
    }
    a->priv = pr;
    a->destroy = u8_destroy;
    a->read = u8_read;
    pr->base = base;

    struct {
        uint32_t end;
        int idx;
    } stack[64];

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

        if (name_off >= names_len) {
            ok = false;
            break;
        }

        char name[256];
        size_t ml = strnlen(names + name_off, names_len - name_off);
        if (ml >= sizeof(name)) ml = sizeof(name) - 1;
        memcpy(name, names + name_off, ml);
        name[ml] = '\0';
        if (!name[0]) snprintf(name, sizeof(name), "node_%u", (unsigned)i);

        if (is_dir) {
            if (y <= i || y > count || sp >= 63) {
                ok = false;
                break;
            }
            /* a folder named "." (many channel contents keep everything in one): its contents go
             * straight into the folder holding it, so opening the archive shows them at once */
            int idx = !strcmp(name, ".") || !strcmp(name, "..") ?
                      stack[sp].idx :
                      tree_add(&a->tree, stack[sp].idx, name, true, 0, 0, 0, 0);
            if (idx < 0) {
                ok = false;
                break;
            }
            sp++;
            stack[sp].end = y;
            stack[sp].idx = idx;
        } else {
            if (tree_add(&a->tree, stack[sp].idx, name, false, y, base + x, 0, 0) < 0) {
                ok = false;
                break;
            }
        }
    }

    free(tab);
    if (!ok) {
        archive_close(a);
        return NULL;
    }

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

/* IMD5 header (0x20 bytes) then the payload, LZ77-compressed or not.
 * A U8 payload is opened directly. */
static archive_t *open_imd5(const src_t *file, const char *name)
{
    uint8_t *raw;
    uint32_t rlen;
    if (!read_whole(file, &raw, &rlen)) return NULL;
    if (rlen < 0x28 || memcmp(raw, "IMD5", 4)) {
        free(raw);
        return NULL;
    }

    uint32_t psize = be32(raw + 4);
    if (psize > rlen - 0x20) psize = rlen - 0x20;
    uint8_t *payload = NULL;
    uint32_t plen = 0;
    if (lz77_tagged(raw + 0x20, psize))
        payload = lz_unpack(raw + 0x20, psize, 4, &plen);
    else
        payload = NULL;
    if (!payload) {
        uint8_t ty;
        uint32_t sz, hl;
        if (psize >= 4 && lz_header(raw + 0x20, psize, &ty, &sz, &hl))
            payload = lz_unpack(raw + 0x20, psize, 0, &plen);
    }

    if (!payload) {
        payload = malloc(psize ? psize : 1);
        if (!payload) {
            free(raw);
            return NULL;
        }
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
    { "Yaz0",        arc_yaz0_probe,       arc_yaz0_open },
    { "RARC",        arc_rarc_probe,       arc_rarc_open },
    { "BRRES",       arc_brres_probe,      arc_brres_open },
    { "ASH0",        arc_ash0_probe,       arc_ash0_open },
    { "GFA",         arc_gfa_probe,        arc_gfa_open },
    { "BRSAR",       arc_brsar_probe,      arc_brsar_open },
    { "CCF",         arc_ccf_probe,        arc_ccf_open },
    { "PAC",         arc_pac_probe,        arc_pac_open },
    { "Retro PAK",   arc_retro_probe,      arc_retro_open },
    { "RST",         arc_rst_probe,        arc_rst_open },
    { "LZO",         arc_lzop_probe,       arc_lzop_open },
    { "EPUB",        arc_epub_probe,       arc_epub_open },
    { "disc image",  arc_disc_probe,       arc_disc_open },
    { "zip/7z/rar",  arc_zip7zrar_probe,   arc_zip7zrar_open },
    { "more",        arc_more_probe,       arc_more_open },
    { "WFS",         arc_wfs_probe,        arc_wfs_open },
    { "ext",         arc_ext_probe,        arc_ext_open },
    { "HFS+",        arc_hfs_probe,        arc_hfs_open },
    { "XFS",         arc_xfs_probe,        arc_xfs_open },
    { "APFS",        arc_apfs_probe,       arc_apfs_open },
    { "ZFS",         arc_zfs_probe,        arc_zfs_open },
    { "BootMii NAND", arc_nand_probe,      arc_nand_open },
    { "HSD",         arc_hsd_probe,        arc_hsd_open },
    { "FJG",         arc_fjg_probe,        arc_fjg_open },
    { "PDF",         arc_pdf_probe,        arc_pdf_open },
    { "partitions",  arc_disk_probe,       arc_disk_open },
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
    if (open_depth >= 4) {
        g_archive_err = "Archives are nested too deeply";
        return NULL;
    }
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

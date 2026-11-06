/*
 * mym -> csm. See mym.h.
 *
 * A C port of cmyksoda's mym-to-csm (build_csm.py, u8.py, tpl.py, lz77.c), which builds what
 * ThemeMii builds. Same node order, data order, LZ77 matches and TPL encodings, so a csm made here
 * is byte for byte the one mym-to-csm makes (resized images aside: the resampling isn't Pillow's).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>

#include <png.h>

#include "mym.h"
#include "ash.h"

#define U8_MAGIC 0x55AA382Du
#define MAX_ARCHIVE (64u * 1024 * 1024)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static uint32_t align32(uint32_t v) { return (v + 31) & ~31u; }

/* ---------------------------------------------------------------- U8 */

static void u8_paths(u8arc *u)
{
    if (u->paths)
        for (int i = 0; i < u->count; i++) free(u->paths[i]);
    free(u->paths);
    u->paths = calloc(u->count ? u->count : 1, sizeof(char *));
    /* stack of the open folders: node index and end index */
    int *stk = malloc(sizeof(int) * (u->count + 1)), sp = 0;
    for (int i = 0; i < u->count && u->paths && stk; i++) {
        while (sp && (int)u->n[stk[sp - 1]].b <= i) sp--;
        size_t len = strlen(u->n[i].name) + 1;
        for (int k = 0; k < sp; k++) len += strlen(u->n[stk[k]].name) + 1;
        char *p = malloc(len + 1);
        if (!p) continue;
        p[0] = '\0';
        for (int k = 0; k <= sp; k++) {
            const char *nm = k < sp ? u->n[stk[k]].name : u->n[i].name;
            if (!nm[0] || !strcmp(nm, ".")) continue;
            if (p[0]) strcat(p, "/");
            strcat(p, nm);
        }
        u->paths[i] = p;
        if (u->n[i].dir && i > 0) stk[sp++] = i;
    }
    free(stk);
}

bool u8_parse(u8arc *u, const uint8_t *data, uint32_t len)
{
    memset(u, 0, sizeof(*u));
    if (len < 0x20 || rd32(data) != U8_MAGIC) return false;
    uint32_t root = rd32(data + 4), hsize = rd32(data + 8);
    if (root < 0x20 || root > len - 12 || hsize > len - root) return false;
    uint32_t count = rd32(data + root + 8);
    if (!count || count > hsize / 12 || data[root] != 1) return false;
    uint32_t strings = root + count * 12, strend = root + hsize;
    memcpy(u->reserved, data + 16, 16);
    u->n = calloc(count, sizeof(u8node));
    if (!u->n) return false;
    u->cap = u->count = (int)count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = data + root + i * 12;
        uint32_t word = rd32(e), a = rd32(e + 4), b = rd32(e + 8);
        u8node *nd = &u->n[i];
        nd->dir = (word >> 24) == 1;
        if ((word >> 24) > 1) goto bad;
        uint32_t no = strings + (word & 0xFFFFFF);
        if (no >= strend) goto bad;
        const uint8_t *z = memchr(data + no, 0, strend - no);
        if (!z) goto bad;
        nd->name = strdup((const char *)data + no);
        if (!nd->name) goto bad;
        nd->a = a;
        nd->b = b;
        if (nd->dir) {
            if (b > count || (i > 0 && (b <= i || a >= i))) goto bad;
        } else {
            if (a > len || b > len - a) goto bad;
            nd->data = data + a;
            nd->len = b;
            nd->order = a;
        }
    }
    if (u->n[0].b != count) goto bad;
    u8_paths(u);
    if (!u->paths) goto bad;
    return true;
bad:
    u8_free(u);
    return false;
}

void u8_free(u8arc *u)
{
    for (int i = 0; i < u->count; i++) {
        free(u->n[i].name);
        free(u->n[i].owned);
        if (u->paths) free(u->paths[i]);
    }
    free(u->n);
    free(u->paths);
    memset(u, 0, sizeof(*u));
}

static void trim_path(const char *in, char *out, size_t n)
{
    while (*in && (isspace((unsigned char)*in) || *in == '/')) in++;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (isspace((unsigned char)out[l - 1]) || out[l - 1] == '/')) out[--l] = '\0';
}

int u8_find(const u8arc *u, const char *path, bool exact)
{
    char key[512];
    trim_path(path, key, sizeof(key));
    for (int i = 0; i < u->count; i++) {
        if (u->n[i].dir || !u->paths[i]) continue;
        if (exact ? !strcmp(u->paths[i], key) : !strcasecmp(u->paths[i], key)) return i;
    }
    return -1;
}

static int u8_find_dir(const u8arc *u, const char *path)
{
    for (int i = 0; i < u->count; i++)
        if (u->n[i].dir && u->paths[i] && !strcasecmp(u->paths[i], path)) return i;
    return -1;
}

void u8_set(u8arc *u, int index, uint8_t *data, uint32_t len)
{
    free(u->n[index].owned);
    u->n[index].owned = data;
    u->n[index].data = data;
    u->n[index].len = len;
}

/* Adds an empty file under a folder that exists; its index, or -1. */
static int u8_add_file(u8arc *u, const char *path)
{
    char p[512];
    trim_path(path, p, sizeof(p));
    char *slash = strrchr(p, '/');
    if (!slash || !slash[1]) return -1;
    *slash = '\0';
    int parent = u8_find_dir(u, p);
    if (parent < 0) return -1;
    if (u->count == u->cap) {
        int cap = u->cap ? u->cap * 2 : 16;
        u8node *nn = realloc(u->n, sizeof(u8node) * cap);
        if (!nn) return -1;
        u->n = nn;
        u->cap = cap;
    }
    char *name = strdup(slash + 1);
    if (!name) return -1;
    uint32_t at = u->n[parent].b, last = 0;
    for (int i = 0; i < u->count; i++) {
        u8node *nd = &u->n[i];
        if (nd->dir) {
            if (nd->a >= at) nd->a++;
            if (nd->b >= at) nd->b++;
        } else if (nd->order > last) {
            last = nd->order;
        }
    }
    if (u->n[parent].b < at + 1) u->n[parent].b = at + 1;
    memmove(&u->n[at + 1], &u->n[at], sizeof(u8node) * (u->count - at));
    memset(&u->n[at], 0, sizeof(u8node));
    u->n[at].name = name;
    u->n[at].order = last + 1;
    u->n[at].data = (const uint8_t *)"";
    /* the path table follows the nodes */
    for (int i = 0; i < u->count; i++) free(u->paths[i]);
    free(u->paths);
    u->paths = NULL;
    u->count++;
    u8_paths(u);
    return u->paths ? (int)at : -1;
}

static const u8arc *sort_arc;
static int by_order(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    uint32_t oa = sort_arc->n[a].order, ob = sort_arc->n[b].order;
    if (oa != ob) return oa < ob ? -1 : 1;
    return a < b ? -1 : a > b;          /* stable, as Python's sorted() */
}

uint8_t *u8_build(const u8arc *u, uint32_t *out_len)
{
    uint32_t names = 0;
    for (int i = 0; i < u->count; i++) names += strlen(u->n[i].name) + 1;
    uint32_t hsize = u->count * 12 + names, pos = align32(0x20 + hsize);
    int *ids = malloc(sizeof(int) * (u->count ? u->count : 1)), nf = 0;
    uint32_t *placed = calloc(u->count ? u->count : 1, sizeof(uint32_t));
    if (!ids || !placed) goto fail;
    for (int i = 0; i < u->count; i++)
        if (!u->n[i].dir) ids[nf++] = i;
    sort_arc = u;
    qsort(ids, nf, sizeof(int), by_order);
    uint32_t total = pos;
    for (int k = 0; k < nf; k++) {
        placed[ids[k]] = pos;
        total = pos + u->n[ids[k]].len;
        if (total > MAX_ARCHIVE) goto fail;
        pos = align32(total);
    }
    uint8_t *out = calloc(1, total);
    if (!out) goto fail;
    wr32(out, U8_MAGIC);
    wr32(out + 4, 0x20);
    wr32(out + 8, hsize);
    wr32(out + 12, align32(0x20 + hsize));
    memcpy(out + 16, u->reserved, 16);
    uint8_t *e = out + 0x20, *s = out + 0x20 + u->count * 12;
    uint32_t so = 0;
    for (int i = 0; i < u->count; i++, e += 12) {
        const u8node *nd = &u->n[i];
        wr32(e, (nd->dir ? 1u << 24 : 0) | so);
        wr32(e + 4, nd->dir ? nd->a : placed[i]);
        wr32(e + 8, nd->dir ? nd->b : nd->len);
        size_t l = strlen(nd->name) + 1;
        memcpy(s + so, nd->name, l);
        so += l;
    }
    for (int k = 0; k < nf; k++)
        memcpy(out + placed[ids[k]], u->n[ids[k]].data, u->n[ids[k]].len);
    free(ids);
    free(placed);
    *out_len = total;
    return out;
fail:
    free(ids);
    free(placed);
    return NULL;
}

/* ---------------------------------------------------------------- LZ77 */

uint8_t *lz77_unpack(const uint8_t *in, uint32_t len, uint32_t *out_len)
{
    if (len >= 4 && !memcmp(in, "LZ77", 4)) {
        in += 4;
        len -= 4;
    }
    if (len < 4 || in[0] != 0x10) return NULL;
    uint32_t size = in[1] | in[2] << 8 | in[3] << 16, p = 4;
    if (!size) {
        if (len < 8) return NULL;
        size = in[4] | in[5] << 8 | in[6] << 16 | (uint32_t)in[7] << 24;
        p = 8;
    }
    if (!size || size > MAX_ARCHIVE) return NULL;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    uint32_t o = 0;
    while (o < size) {
        if (p >= len) goto bad;
        uint8_t flags = in[p++];
        for (int bit = 0; bit < 8 && o < size; bit++) {
            if (flags & (0x80 >> bit)) {
                if (p + 2 > len) goto bad;
                uint32_t n = (in[p] >> 4) + 3, d = ((in[p] & 15) << 8 | in[p + 1]) + 1;
                p += 2;
                if (d > o) goto bad;
                for (uint32_t k = 0; k < n && o < size; k++, o++) out[o] = out[o - d];
            } else {
                if (p >= len) goto bad;
                out[o++] = in[p++];
            }
        }
    }
    *out_len = size;
    return out;
bad:
    free(out);
    return NULL;
}

#define LZ_MIN 3
#define LZ_MAX 18
#define LZ_DIST 4096
#define LZ_HASH_BITS 15
#define LZ_CHAIN 256
/* The chains only reach back LZ_DIST bytes, so a ring of 2 * LZ_DIST links is enough: a link is
 * overwritten LZ_WIN positions later, after it can last be followed. A link per position would take
 * 4 bytes for every byte of input (23 MB for the Wii Menu's health.ash). */
#define LZ_WIN 8192

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)(p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u) >> (32 - LZ_HASH_BITS);
}

/* True when `in` (LZ77-packed) decompresses to exactly `want`, without a buffer: back references
 * are read from `want`, which is what they must reproduce. */
static bool lz77_matches(const uint8_t *in, uint32_t len, const uint8_t *want, uint32_t wlen)
{
    if (len < 8 || memcmp(in, "LZ77\x10", 5)) return false;
    uint32_t size = in[5] | in[6] << 8 | in[7] << 16, p = 8, o = 0;
    if (size != wlen) return false;
    while (o < size) {
        if (p >= len) return false;
        uint8_t flags = in[p++];
        for (int bit = 0; bit < 8 && o < size; bit++) {
            if (flags & (0x80 >> bit)) {
                if (p + 2 > len) return false;
                uint32_t n = (in[p] >> 4) + 3, d = ((in[p] & 15) << 8 | in[p + 1]) + 1;
                p += 2;
                if (d > o) return false;
                for (uint32_t k = 0; k < n && o < size; k++, o++)
                    if (want[o] != want[o - d]) return false;
            } else {
                if (p >= len || in[p++] != want[o++]) return false;
            }
        }
    }
    return true;
}

/* mym-to-csm's lz77.c: hash chains, the longest match within 256 candidates, nearest first. */
uint8_t *lz77_pack(const uint8_t *in, uint32_t n, uint32_t *out_len)
{
    if (n > 0xFFFFFF) return NULL;
    const uint8_t *src = in;
    int32_t *head = malloc(sizeof(int32_t) << LZ_HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * LZ_WIN);
    uint8_t *out = malloc(8 + n + n / 8 + 16);
    if (!head || !prev || !out) {
        free(head);
        free(prev);
        free(out);
        return NULL;
    }
    memset(head, 0xFF, sizeof(int32_t) << LZ_HASH_BITS);
    memcpy(out, "LZ77\x10", 5);
    out[5] = n & 0xFF;
    out[6] = (n >> 8) & 0xFF;
    out[7] = (n >> 16) & 0xFF;
    size_t o = 8;
    long pos = 0, inserted = 0;
    while (pos < (long)n) {
        size_t flag_at = o++;
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && pos < (long)n; bit++) {
            for (; inserted < pos && inserted + LZ_MIN <= (long)n; inserted++) {
                uint32_t h = hash3(src + inserted);
                prev[inserted & (LZ_WIN - 1)] = head[h];
                head[h] = inserted;
            }
            int best_len = 0, best_dist = 0;
            if (pos + LZ_MIN <= (long)n) {
                int32_t cand = head[hash3(src + pos)];
                for (int steps = 0; cand >= 0 && steps < LZ_CHAIN; steps++, cand = prev[cand & (LZ_WIN - 1)]) {
                    long dist = pos - cand;
                    if (dist > LZ_DIST) break;
                    int len = 0, limit = ((long)n - pos < LZ_MAX) ? (int)(n - pos) : LZ_MAX;
                    while (len < limit && src[cand + len] == src[pos + len]) len++;
                    if (len > best_len) {
                        best_len = len;
                        best_dist = (int)dist;
                        if (len == LZ_MAX) break;
                    }
                }
            }
            if (best_len >= LZ_MIN) {
                int code = ((best_len - LZ_MIN) << 12) | (best_dist - 1);
                out[o++] = code >> 8;
                out[o++] = code & 0xFF;
                flags |= 0x80 >> bit;
                pos += best_len;
            } else {
                out[o++] = src[pos++];
            }
        }
        out[flag_at] = flags;
    }
    free(head);
    free(prev);
    *out_len = (uint32_t)o;
    return out;
}

/* ---------------------------------------------------------------- TPL */

static const struct { const char *name; int bpp, bw, bh; } FMT[7] = {
    { "I4", 4, 8, 8 },     { "I8", 8, 8, 4 },     { "IA4", 8, 8, 4 },    { "IA8", 16, 4, 4 },
    { "RGB565", 16, 4, 4 }, { "RGB5A3", 16, 4, 4 }, { "RGBA8", 32, 4, 4 },
};

int tpl_format_by_name(const char *name)
{
    char key[16];
    trim_path(name, key, sizeof(key));
    for (int i = 0; i < 7; i++)
        if (!strcasecmp(key, FMT[i].name)) return i;
    return -1;
}

static uint32_t q(uint32_t c, int bits) { return (c * ((1u << bits) - 1) + 127) / 255; }

uint8_t *tpl_encode(const uint8_t *rgba, int w, int h, int fmt, const uint8_t *tmpl, uint32_t tmpl_len,
                    uint32_t *out_len)
{
    if (fmt < 0 || fmt > 6 || w <= 0 || h <= 0 || w > 1024 || h > 1024) return NULL;
    int bw = FMT[fmt].bw, bh = FMT[fmt].bh;
    int tx = (w + bw - 1) / bw, ty = (h + bh - 1) / bh;
    uint32_t dsize = (uint32_t)tx * ty * bw * bh * FMT[fmt].bpp / 8;

    uint32_t header_off = 0x14, data_off = 0x40;
    bool keep = tmpl && tmpl_len >= 20 && rd32(tmpl) == 0x0020AF30 && rd32(tmpl + 4) == 1 && rd32(tmpl + 16) == 0;
    if (keep) {
        header_off = rd32(tmpl + 12);
        keep = header_off <= tmpl_len - 12;
        if (keep) {
            data_off = rd32(tmpl + header_off + 8);
            keep = data_off <= tmpl_len && data_off >= header_off + 8 && data_off <= 0x10000;
        }
        if (!keep) {
            header_off = 0x14;
            data_off = 0x40;
        }
    }
    uint8_t *out = calloc(1, data_off + dsize);
    if (!out) return NULL;
    if (keep) {
        memcpy(out, tmpl, data_off);
    } else {
        wr32(out, 0x0020AF30);
        wr32(out + 4, 1);
        wr32(out + 8, 0x0C);
        wr32(out + 12, header_off);
        uint8_t *hd = out + header_off;
        wr32(hd + 8, data_off);
        wr32(hd + 20, 1);               /* min filter */
        wr32(hd + 24, 1);               /* mag filter */
    }
    wr16(out + header_off, h);
    wr16(out + header_off + 2, w);
    wr32(out + header_off + 4, fmt);

    uint8_t *d = out + data_off;
    for (int by = 0; by < ty; by++) {
        for (int bx = 0; bx < tx; bx++) {
            uint32_t r[64], g[64], b[64], a[64], in[64];
            int k = 0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++, k++) {
                    int px = bx * bw + x, py = by * bh + y;
                    if (px < w && py < h) {
                        const uint8_t *s = rgba + ((size_t)py * w + px) * 4;
                        r[k] = s[0]; g[k] = s[1]; b[k] = s[2]; a[k] = s[3];
                    } else {
                        r[k] = g[k] = b[k] = a[k] = 0;
                    }
                    in[k] = (r[k] * 299 + g[k] * 587 + b[k] * 114 + 500) / 1000;
                }
            switch (fmt) {
            case 6:
                for (k = 0; k < 16; k++) { *d++ = a[k]; *d++ = r[k]; }
                for (k = 0; k < 16; k++) { *d++ = g[k]; *d++ = b[k]; }
                break;
            case 5:
                for (k = 0; k < 16; k++, d += 2) {
                    uint32_t a3 = q(a[k], 3);
                    uint32_t v = a3 == 7 ? 0x8000 | q(r[k], 5) << 10 | q(g[k], 5) << 5 | q(b[k], 5)
                                         : a3 << 12 | q(r[k], 4) << 8 | q(g[k], 4) << 4 | q(b[k], 4);
                    wr16(d, v);
                }
                break;
            case 4:
                for (k = 0; k < 16; k++, d += 2) wr16(d, q(r[k], 5) << 11 | q(g[k], 6) << 5 | q(b[k], 5));
                break;
            case 3:
                for (k = 0; k < 16; k++, d += 2) wr16(d, a[k] << 8 | in[k]);
                break;
            case 2:
                for (k = 0; k < 32; k++) *d++ = q(a[k], 4) << 4 | q(in[k], 4);
                break;
            case 1:
                for (k = 0; k < 32; k++) *d++ = in[k];
                break;
            case 0:
                for (k = 0; k < 64; k += 2) *d++ = q(in[k], 4) << 4 | q(in[k + 1], 4);
                break;
            }
        }
    }
    *out_len = data_off + dsize;
    return out;
}

/* ---------------------------------------------------------------- PNG */

typedef struct { const uint8_t *p; size_t left; } pngmem;
static void png_mem_read(png_structp png, png_bytep out, png_size_t n)
{
    pngmem *m = png_get_io_ptr(png);
    if (n > m->left) png_error(png, "short");
    memcpy(out, m->p, n);
    m->p += n;
    m->left -= n;
}

static uint8_t *png_rgba(const uint8_t *data, uint32_t len, int *w, int *h)
{
    if (len < 8 || png_sig_cmp((png_const_bytep)data, 0, 8)) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    uint8_t *volatile pix = NULL;
    png_bytep *volatile rows = NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        free(pix);
        free(rows);
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        return NULL;
    }
    pngmem m = { data, len };
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);
    uint32_t pw = png_get_image_width(png, info), ph = png_get_image_height(png, info);
    if (!pw || !ph || pw > 2048 || ph > 2048) png_error(png, "size");
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    pix = malloc((size_t)pw * ph * 4);
    rows = malloc(ph * sizeof(png_bytep));
    if (!pix || !rows) png_error(png, "memory");
    for (uint32_t y = 0; y < ph; y++) rows[y] = pix + (size_t)y * pw * 4;
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);
    *w = pw;
    *h = ph;
    return pix;
}

/* Bicubic (a = -0.5, widened when shrinking) on premultiplied alpha, as Pillow resizes RGBA. */
static double cubic(double x)
{
    const double a = -0.5;
    x = fabs(x);
    if (x < 1) return ((a + 2) * x - (a + 3)) * x * x + 1;
    if (x < 2) return (((x - 5) * x + 8) * x - 4) * a;
    return 0;
}

static void resample_axis(const float *src, float *dst, int sn, int dn, int count, int sstride, int dstride,
                          int step)
{
    double scale = (double)sn / dn, fs = scale > 1 ? scale : 1, support = 2 * fs;
    for (int o = 0; o < dn; o++) {
        double center = (o + 0.5) * scale;
        int lo = (int)floor(center - support), hi = (int)ceil(center + support);
        if (lo < 0) lo = 0;
        if (hi > sn) hi = sn;
        double wsum = 0, wt[64];
        int nw = 0;
        for (int i = lo; i < hi && nw < 64; i++, nw++) wsum += wt[nw] = cubic((i + 0.5 - center) / fs);
        for (int c = 0; c < count; c++) {
            double acc = 0;
            for (int k = 0; k < nw; k++) acc += wt[k] * src[(lo + k) * sstride + c * step];
            dst[o * dstride + c * step] = (float)(wsum ? acc / wsum : 0);
        }
    }
}

static uint8_t *resize_rgba(const uint8_t *in, int w, int h, int nw, int nh)
{
    if (nw <= 0 || nh <= 0 || nw > 1024 || nh > 1024) return NULL;
    float *a = malloc(sizeof(float) * w * h * 4), *b = malloc(sizeof(float) * nw * h * 4),
          *c = malloc(sizeof(float) * nw * nh * 4);
    uint8_t *out = malloc((size_t)nw * nh * 4);
    if (!a || !b || !c || !out) {
        free(a); free(b); free(c); free(out);
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        float al = in[i * 4 + 3];
        for (int k = 0; k < 3; k++) a[i * 4 + k] = in[i * 4 + k] * al / 255.f;
        a[i * 4 + 3] = al;
    }
    /* rows: each row's 4 * w values, x stride 4 */
    for (int y = 0; y < h; y++)
        resample_axis(a + y * w * 4, b + y * nw * 4, w, nw, 4, 4, 4, 1);
    /* columns */
    for (int x = 0; x < nw; x++)
        resample_axis(b + x * 4, c + x * 4, h, nh, 4, nw * 4, nw * 4, 1);
    for (int i = 0; i < nw * nh; i++) {
        float al = c[i * 4 + 3];
        al = al < 0 ? 0 : al > 255 ? 255 : al;
        for (int k = 0; k < 3; k++) {
            float v = al > 0 ? c[i * 4 + k] * 255.f / al : 0;
            out[i * 4 + k] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f);
        }
        out[i * 4 + 3] = (uint8_t)(al + 0.5f);
    }
    free(a); free(b); free(c);
    return out;
}

/* ---------------------------------------------------------------- mym.ini */

typedef struct { char key[32]; char *val; } ikv;
typedef struct { char name[64]; ikv *kv; int nkv; } isec;

static void ini_free(isec *s, int n)
{
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < s[i].nkv; k++) free(s[i].kv[k].val);
        free(s[i].kv);
    }
    free(s);
}

static char *strip(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
    return s;
}

/* configparser with strict=False: sections in order of first appearance, repeats merged, keys lower
 * case, '#' / ';' comment lines. */
static isec *ini_parse(const char *text, uint32_t len, int *count)
{
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, text, len);
    buf[len] = '\0';
    isec *s = NULL;
    int n = 0, cur = -1;
    for (char *line = buf, *next; line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) *next++ = '\0';
        char *t = strip(line);
        if (!*t || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char *e = strchr(t, ']');
            if (!e) continue;
            *e = '\0';
            char *nm = t + 1;
            cur = -1;
            for (int i = 0; i < n; i++)
                if (!strcmp(s[i].name, nm)) cur = i;
            if (cur < 0) {
                isec *ns = realloc(s, sizeof(isec) * (n + 1));
                if (!ns) break;
                s = ns;
                memset(&s[n], 0, sizeof(isec));
                snprintf(s[n].name, sizeof(s[n].name), "%s", nm);
                cur = n++;
            }
            continue;
        }
        if (cur < 0) continue;
        char *eq = strpbrk(t, "=:");
        if (!eq) continue;
        *eq = '\0';
        char *k = strip(t), *v = strip(eq + 1);
        for (char *c = k; *c; c++) *c = tolower((unsigned char)*c);
        isec *sc = &s[cur];
        int at = -1;
        for (int i = 0; i < sc->nkv; i++)
            if (!strcmp(sc->kv[i].key, k)) at = i;
        if (at < 0) {
            ikv *nk = realloc(sc->kv, sizeof(ikv) * (sc->nkv + 1));
            if (!nk) continue;
            sc->kv = nk;
            at = sc->nkv++;
            snprintf(sc->kv[at].key, sizeof(sc->kv[at].key), "%s", k);
            sc->kv[at].val = NULL;
        }
        free(sc->kv[at].val);
        sc->kv[at].val = strdup(v);
    }
    free(buf);
    *count = n;
    return s;
}

static const char *ini_get(const isec *s, const char *key)
{
    for (int i = 0; i < s->nkv; i++)
        if (!strcmp(s->kv[i].key, key)) return s->kv[i].val;
    return NULL;
}

/* build_csm.py's norm(): '\' -> '/', no leading / trailing '/', lower case */
static void norm(const char *in, char *out, size_t n)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", in ? in : "");
    for (char *c = tmp; *c; c++) *c = *c == '\\' ? '/' : tolower((unsigned char)*c);
    trim_path(strip(tmp), out, n);
}

/* ---------------------------------------------------------------- build */

typedef struct {
    char prefix[520];
    int index;                  /* the container's node in the menu */
    uint8_t *raw;               /* its unpacked bytes (owned), or NULL when it was a plain U8 */
    u8arc u;
    bool edited;
} cont_t;

static void note(mym_result *res, const char *section, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void note(mym_result *res, const char *section, const char *fmt, ...)
{
    res->skipped++;
    size_t l = strlen(res->report);
    if (l + 2 >= sizeof(res->report)) return;
    l += snprintf(res->report + l, sizeof(res->report) - l, "[%s] ", section);
    if (l >= sizeof(res->report)) return;
    va_list ap;
    va_start(ap, fmt);
    l += vsnprintf(res->report + l, sizeof(res->report) - l, fmt, ap);
    va_end(ap);
    if (l + 1 < sizeof(res->report)) strcat(res->report, "\n");
}

uint8_t *mym_build(const uint8_t *base_data, uint32_t base_len, mym_fetch_fn fetch, void *fctx,
                   mym_progress_fn progress, void *pctx, uint32_t *out_len, mym_result *res,
                   char *err, size_t errsz)
{
    memset(res, 0, sizeof(*res));
    u8arc base;
    if (!u8_parse(&base, base_data, base_len)) {
        snprintf(err, errsz, "The System Menu archive isn't a valid U8 archive.");
        return NULL;
    }
    uint8_t *ini_data = NULL;
    uint32_t ini_len = 0;
    if (!fetch(fctx, "mym.ini", &ini_data, &ini_len)) {
        snprintf(err, errsz, "This mym has no mym.ini.");
        u8_free(&base);
        return NULL;
    }
    int nsec = 0;
    isec *sec = ini_parse((const char *)ini_data, ini_len, &nsec);
    free(ini_data);
    if (!sec || !nsec) {
        snprintf(err, errsz, "mym.ini lists nothing to change.");
        ini_free(sec, nsec);
        u8_free(&base);
        return NULL;
    }

    cont_t *ct = calloc(nsec, sizeof(cont_t));
    int nct = 0;
    uint8_t *out = NULL;
    if (!ct) {
        snprintf(err, errsz, "Out of memory.");
        goto done;
    }

    for (int si = 0; si < nsec; si++) {
        const isec *s = &sec[si];
        if (progress) progress(pctx, s->name, si, nsec);
        char kind[64], target[512];
        snprintf(kind, sizeof(kind), "%s", s->name);
        size_t kl = strlen(kind);
        while (kl && isdigit((unsigned char)kind[kl - 1])) kind[--kl] = '\0';
        for (char *c = kind; *c; c++) *c = tolower((unsigned char)*c);
        norm(ini_get(s, "file"), target, sizeof(target));

        if (!strcmp(kind, "cont")) {
            int idx = u8_find(&base, target, false);
            if (idx < 0) {
                note(res, s->name, "container %s isn't in this menu", target);
                continue;
            }
            const u8node *nd = &base.n[idx];
            uint8_t *raw = NULL;
            uint32_t rlen = nd->len;
            uint32_t asz = ash_size(nd->data, nd->len);
            if (asz) {
                raw = asz <= MAX_ARCHIVE ? malloc(asz) : NULL;
                if (!raw || !ash_decompress(nd->data, nd->len, raw, asz)) {
                    free(raw);
                    note(res, s->name, "container %s couldn't be unpacked (ASH0)", target);
                    continue;
                }
                rlen = asz;
            } else if ((nd->len >= 4 && !memcmp(nd->data, "LZ77", 4)) || (nd->len && nd->data[0] == 0x10)) {
                raw = lz77_unpack(nd->data, nd->len, &rlen);
                if (!raw) {
                    note(res, s->name, "container %s couldn't be unpacked (LZ77)", target);
                    continue;
                }
            }
            u8arc cu;
            if (!u8_parse(&cu, raw ? raw : nd->data, rlen)) {
                free(raw);
                note(res, s->name, "container %s isn't a U8 archive", target);
                continue;
            }
            char prefix[520];
            snprintf(prefix, sizeof(prefix), "%s", target);
            for (char *c = prefix; *c; c++) if (*c == '.') *c = '_';
            strncat(prefix, "_out/", sizeof(prefix) - strlen(prefix) - 1);
            int at = -1;
            for (int i = 0; i < nct; i++)
                if (!strcmp(ct[i].prefix, prefix)) at = i;
            if (at >= 0) {
                /* listed again: unpacked afresh, like the dict entry it replaces */
                u8_free(&ct[at].u);
                free(ct[at].raw);
            } else {
                at = nct++;
                snprintf(ct[at].prefix, sizeof(ct[at].prefix), "%s", prefix);
                ct[at].edited = false;
            }
            ct[at].index = idx;
            ct[at].raw = raw;
            ct[at].u = cu;
            continue;
        }

        if (strcmp(kind, "sdta") && strcmp(kind, "simg")) {
            note(res, s->name, "%s entries need user input and aren't supported", kind);
            continue;
        }

        u8arc *arc = &base;
        const char *inner = target;
        cont_t *owner = NULL;
        for (int i = 0; i < nct; i++) {
            size_t pl = strlen(ct[i].prefix);
            if (!strncmp(target, ct[i].prefix, pl)) {
                arc = &ct[i].u;
                inner = target + pl;
                owner = &ct[i];
                break;
            }
        }
        if (!owner && strstr(target, "_out/")) {
            note(res, s->name, "%s is inside a container this menu doesn't have", target);
            continue;
        }

        int idx = u8_find(arc, inner, false);
        if (idx < 0 && owner && !strcmp(kind, "sdta"))
            idx = u8_add_file(arc, inner);       /* ThemeMii adds a static file whose folder exists */
        if (idx < 0) {
            note(res, s->name, "%s isn't in this menu", target);
            continue;
        }

        char srcname[512];
        const char *sv = ini_get(s, "source");
        norm(sv, srcname, sizeof(srcname));
        uint8_t *data = NULL;
        uint32_t dlen = 0;
        if (!sv || !fetch(fctx, srcname, &data, &dlen)) {
            note(res, s->name, "source %s is missing from the mym", sv ? sv : "(none)");
            continue;
        }

        if (!strcmp(kind, "simg")) {
            int w, h;
            uint8_t *rgba = png_rgba(data, dlen, &w, &h);
            free(data);
            data = NULL;
            if (!rgba) {
                note(res, s->name, "source %s isn't a PNG this can read", sv);
                continue;
            }
            const char *ws = ini_get(s, "width"), *hs = ini_get(s, "height"), *fs = ini_get(s, "format");
            int nw = ws ? atoi(ws) : w, nh = hs ? atoi(hs) : h;
            if (nw != w || nh != h) {
                uint8_t *r2 = resize_rgba(rgba, w, h, nw, nh);
                free(rgba);
                rgba = r2;
                w = nw;
                h = nh;
                if (!rgba) {
                    note(res, s->name, "%s: can't resize to %dx%d", target, nw, nh);
                    continue;
                }
            }
            int fmt = fs ? tpl_format_by_name(fs) : 5;
            if (fmt < 0) {
                free(rgba);
                note(res, s->name, "unknown TPL format %s", fs);
                continue;
            }
            data = tpl_encode(rgba, w, h, fmt, arc->n[idx].data, arc->n[idx].len, &dlen);
            free(rgba);
            if (!data) {
                note(res, s->name, "%s couldn't be encoded", target);
                continue;
            }
        }

        u8_set(arc, idx, data, dlen);
        res->applied++;
        if (owner) owner->edited = true;
    }

    for (int i = 0; i < nct; i++) {
        if (!ct[i].edited) continue;
        if (progress) progress(pctx, ct[i].prefix, nsec, nsec);
        uint32_t ul, pl;
        uint8_t *built = u8_build(&ct[i].u, &ul);
        /* the unpacked container isn't needed once it is rebuilt: free it before compressing */
        u8_free(&ct[i].u);
        free(ct[i].raw);
        ct[i].raw = NULL;
        uint8_t *packed = built ? lz77_pack(built, ul, &pl) : NULL;
        /* the round trip, as mym-to-csm checks it */
        bool oom = !built || !packed;
        bool same = !oom && lz77_matches(packed, pl, built, ul);
        free(built);
        if (!same) {
            free(packed);
            snprintf(err, errsz, "Couldn't rebuild %s (%s).", ct[i].prefix,
                     oom ? "out of memory" : "LZ77 round trip failed");
            goto done;
        }
        u8_set(&base, ct[i].index, packed, pl);
    }

    out = u8_build(&base, out_len);
    if (!out) snprintf(err, errsz, "Couldn't build the archive (out of memory?).");

done:
    for (int i = 0; i < nct; i++) {
        u8_free(&ct[i].u);
        free(ct[i].raw);
    }
    free(ct);
    ini_free(sec, nsec);
    u8_free(&base);
    return out;
}

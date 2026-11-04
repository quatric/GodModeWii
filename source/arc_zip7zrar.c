/*
 * zip, 7z and rar archives, read-only, for browsing: unarr (LGPL-3.0, source/unarr: zip, 7z,
 * RAR 1.5 - 4.x) and rar5.c (RAR5), the readers the FTP installer already uses.
 *
 * Files of up to 16 MiB are unpacked whole the first time they are read and kept until another
 * file is read, so archives and images inside an archive open quickly. Larger files are
 * streamed: reading forward continues where the last read stopped, reading backward starts the
 * file over. RAR5 files only unpack whole, so there the limit is 48 MiB; a solid RAR5 also needs
 * its dictionary (at most the archive's unpacked size) in memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"
#include "unarr-imp.h"
#include "rar5.h"

#define WHOLE_MAX  (16u << 20)
#define RAR5_MAX   (48u << 20)

/* ---------------------------------------------------------------- the file as an unarr stream */

typedef struct { const src_t *src; uint64_t pos; } sstream;

static void ss_close(void *d) { free(d); }
static size_t ss_read(void *d, void *buf, size_t n)
{
    sstream *s = d;
    if (s->pos >= s->src->size) return 0;
    if (n > s->src->size - s->pos) n = (size_t)(s->src->size - s->pos);
    size_t got = 0;
    while (got < n) {
        uint32_t c = n - got > 0x100000 ? 0x100000 : (uint32_t)(n - got);
        int32_t r = s->src->read(s->src->ctx, s->pos + got, (uint8_t *)buf + got, c);
        if (r <= 0) break;
        got += (size_t)r;
    }
    s->pos += got;
    return got;
}
static bool ss_seek(void *d, off64_t off, int origin)
{
    sstream *s = d;
    int64_t t = origin == SEEK_SET ? off : origin == SEEK_CUR ? (int64_t)s->pos + off : (int64_t)s->src->size + off;
    if (t < 0 || (uint64_t)t > s->src->size) return false;
    s->pos = (uint64_t)t;
    return true;
}
static off64_t ss_tell(void *d) { return (off64_t)((sstream *)d)->pos; }

/* ---------------------------------------------------------------- the archive */

typedef struct {
    ar_stream *st;
    ar_archive *ar;             /* unarr */
    rar5_t *r5;                 /* or RAR5 */
    int r5_at;                  /* RAR5: index of the entry rar5_next last returned, -1 before the first */
    int node;                   /* entry whose bytes are in whole / being streamed, or -1 */
    uint8_t *whole;
    uint64_t pos;               /* streaming: bytes of `node` already unpacked */
    uint8_t *scratch;
} zpriv;

enum { E_UNARR, E_RAR5 };       /* tnode.c; a = unarr entry offset, b = RAR5 entry index */

static void z_destroy(archive_t *a)
{
    zpriv *p = a->priv;
    if (!p) return;
    if (p->ar) ar_close_archive(p->ar);
    if (p->r5) rar5_close(p->r5);
    if (p->st) ar_close(p->st);
    free(p->whole);
    free(p->scratch);
    free(p);
}

/* RAR5: positions the reader at entry `idx` with its data not yet read. r5_at is the last entry
 * rar5_next returned, whose data has been read or skipped already. */
static bool r5_goto(zpriv *p, int idx)
{
    if (p->r5_at >= idx) {
        if (!rar5_rewind(p->r5)) return false;
        p->r5_at = -1;
    }
    rar5_entry e;
    for (;;) {
        if (rar5_next(p->r5, &e) != 1) return false;
        p->r5_at++;
        if (p->r5_at == idx) return true;
        if (!rar5_extract(p->r5, NULL, NULL)) return false;
    }
}

typedef struct { uint8_t *dst; size_t got, cap; } fill_t;
static bool fill_sink(void *ud, const uint8_t *d, size_t n)
{
    fill_t *f = ud;
    if (n > f->cap - f->got) return false;
    memcpy(f->dst + f->got, d, n);
    f->got += n;
    return true;
}

/* Starts `nd` over: unarr parses its header again, RAR5 walks to it. */
static bool restart(zpriv *p, const tnode *nd)
{
    p->pos = 0;
    if (nd->c == E_RAR5) return r5_goto(p, (int)nd->b);
    return ar_parse_entry_at(p->ar, (off64_t)nd->a);
}

static int32_t z_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    zpriv *p = a->priv;
    int idx = (int)(nd - a->tree.n);
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    bool whole = nd->size <= (nd->c == E_RAR5 ? RAR5_MAX : WHOLE_MAX);

    if (p->node != idx || (!whole && off < p->pos)) {
        free(p->whole);
        p->whole = NULL;
        p->node = -1;
        if (!restart(p, nd)) { g_archive_err = "This file couldn't be found in the archive again"; return -1; }
        if (whole) {
            p->whole = malloc(nd->size ? nd->size : 1);
            if (!p->whole) { g_archive_err = "Not enough memory to unpack this file"; return -1; }
            bool ok;
            if (nd->c == E_RAR5) {
                fill_t f = { p->whole, 0, nd->size };
                ok = rar5_extract(p->r5, fill_sink, &f) && f.got == nd->size;
            } else
                ok = !nd->size || ar_entry_uncompress(p->ar, p->whole, (size_t)nd->size);
            if (!ok) {
                free(p->whole);
                p->whole = NULL;
                g_archive_err = "This file couldn't be unpacked (damaged, encrypted or an unsupported method)";
                return -1;
            }
        } else if (nd->c == E_RAR5) {
            g_archive_err = "Files over 48 MiB can't be opened from inside a RAR5 archive";
            return -1;
        }
        p->node = idx;
    }
    if (whole) {
        memcpy(buf, p->whole + off, len);
        return (int32_t)len;
    }
    /* streaming (unarr): unpack and drop bytes up to off, then the bytes asked for */
    if (!p->scratch && !(p->scratch = malloc(0x10000))) return -1;
    while (p->pos < off) {
        uint64_t k = off - p->pos;
        if (k > 0x10000) k = 0x10000;
        if (!ar_entry_uncompress(p->ar, p->scratch, (size_t)k)) { p->node = -1; return -1; }
        p->pos += k;
    }
    if (!ar_entry_uncompress(p->ar, buf, len)) { p->node = -1; return -1; }
    p->pos += len;
    return (int32_t)len;
}

/* "a\b|c" -> "a/b_c": the tree splits on '/', and '|' separates archive levels in GodModeWii's paths. */
static void clean_path(char *s)
{
    for (char *q = s; *q; q++) {
        if (*q == '\\') *q = '/';
        else if (*q == '|' || (unsigned char)*q < 0x20 || (unsigned char)*q == 0x7F) *q = '_';
    }
}

static int kind_of(const uint8_t *m)
{
    if (!memcmp(m, "Rar!\x1a\x07\x01\x00", 8)) return 5;
    if (!memcmp(m, "Rar!\x1a\x07\x00", 7)) return 4;
    if (!memcmp(m, "7z\xBC\xAF\x27\x1C", 6)) return 7;
    if (m[0] == 'P' && m[1] == 'K' && ((m[2] == 3 && m[3] == 4) || (m[2] == 5 && m[3] == 6))) return 1;
    return 0;
}

bool arc_zip7zrar_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t m[8];
    return f->size >= 22 && src_read_full(f, 0, m, 8) && kind_of(m);
}

archive_t *arc_zip7zrar_open(const src_t *f, const char *name)
{
    (void)name;
    uint8_t m[8];
    if (!src_read_full(f, 0, m, 8)) return NULL;
    int kind = kind_of(m);
    static const char *const kinds[] = { [1] = "zip", [4] = "RAR", [5] = "RAR5", [7] = "7z" };
    archive_t *a = archive_alloc(kinds[kind]);
    zpriv *p = calloc(1, sizeof(*p));
    sstream *ss = calloc(1, sizeof(*ss));
    if (!a || !p || !ss) { free(p); free(ss); if (a) archive_close(a); return NULL; }
    a->base = *f;
    a->src = a->base;
    a->priv = p;
    a->read = z_read;
    a->destroy = z_destroy;
    p->node = -1;
    p->r5_at = -1;
    ss->src = &a->src;
    p->st = ar_open_stream(ss, ss_close, ss_read, ss_seek, ss_tell);
    if (!p->st) { free(ss); goto fail; }
    char nm[512];
    int count = 0;
    if (kind == 5) {
        p->r5 = rar5_open(p->st);
        if (!p->r5) { g_archive_err = "This RAR5 archive couldn't be read"; goto fail; }
        rar5_entry e;
        int r;
        uint64_t total = 0;
        for (int i = 0; (r = rar5_next(p->r5, &e)) == 1; i++) {
            total += e.dir ? 0 : e.size;
            p->r5_at = i;
            snprintf(nm, sizeof(nm), "%s", e.name);
            clean_path(nm);
            if (!e.skip || e.dir) tree_add_path(&a->tree, nm, e.dir, e.dir ? 0 : e.size, 0, (uint64_t)i, E_RAR5);
            count++;                            /* rar5_next steps over the data itself: nothing is unpacked */
        }
        if (r < 0 && !count) { g_archive_err = rar5_error(p->r5); goto fail; }
        rar5_limit_dict(p->r5, total);
    } else {
        p->ar = kind == 1 ? ar_open_zip_archive(p->st, false) : kind == 4 ? ar_open_rar_archive(p->st) : ar_open_7z_archive(p->st);
        if (!p->ar) { g_archive_err = "This archive couldn't be read (damaged, encrypted or unsupported)"; goto fail; }
        while (ar_parse_entry(p->ar)) {
            const char *en = ar_entry_get_name(p->ar);
            if (!en) continue;
            snprintf(nm, sizeof(nm), "%s", en);
            clean_path(nm);
            size_t l = strlen(nm);
            bool dir = l && nm[l - 1] == '/';
            tree_add_path(&a->tree, nm, dir, dir ? 0 : ar_entry_get_size(p->ar), (uint64_t)ar_entry_get_offset(p->ar), 0, E_UNARR);
            count++;
        }
    }
    if (!count) { if (!g_archive_err) g_archive_err = "The archive is empty or damaged"; goto fail; }
    return a;
fail:
    memset(&a->base, 0, sizeof(a->base));      /* the caller keeps the file on failure */
    memset(&a->src, 0, sizeof(a->src));
    archive_close(a);
    return NULL;
}

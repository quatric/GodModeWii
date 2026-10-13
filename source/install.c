/*
 * Archive installer for the FTP server. See install.h.
 *
 * Reads .zip, .7z and .rar with unarr (LGPL-3.0, source/unarr). Nothing is
 * written before the whole archive has been listed and the layout decided, so
 * an archive that can't be read leaves the drive untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>

#include "install.h"
#include "vfs.h"
#include "unarr-imp.h"
#include "rar5.h"

#define CHUNK (64 * 1024)
#define MAX_NAME 400

/* ---------------------------------------------------------------- layout */

#ifdef __WIIU__
static const char *const ROOTS[] = { "wiiu", "install", "config", "apps", "wad", "private", NULL };
#define APPS_BASE "wiiu/apps/"
#else
static const char *const ROOTS[] = { "apps", "wad", "private", "games", "wbfs", "riivolution", "codes", "txtcodes",
                                     "wiiflow", "usb-loader", "nincfg", NULL };
#define APPS_BASE "apps/"
#endif

static bool is_root_name(const char *c, size_t n)
{
    for (int i = 0; ROOTS[i]; i++)
        if (strlen(ROOTS[i]) == n && !strncasecmp(ROOTS[i], c, n))
            return true;
    return false;
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.'), *sl = strrchr(name, '/');
    return (dot && (!sl || dot > sl)) ? dot + 1 : "";
}

static bool is_doc_ext(const char *e)
{
    static const char *const d[] = { "txt", "md", "nfo", "url", "pdf", "html", "htm", NULL };
    for (int i = 0; d[i]; i++)
        if (!strcasecmp(e, d[i])) return true;
    return false;
}

static bool is_junk(const char *n)
{
    return !strncasecmp(n, "__MACOSX", 8) || strstr(n, ".DS_Store") || strstr(n, "Thumbs.db") || strstr(n, "desktop.ini");
}

/* Cleans an archive path in place: forward slashes, no leading "/", no "." or
 * empty parts. Returns false for paths that try to leave the target (".."),
 * contain characters FAT can't hold, or are too long. */
static bool clean_path(char *p)
{
    char *w = p;
    char *r = p;
    while (*r) {
        while (*r == '/' || *r == '\\') r++;
        if (!*r) break;
        char *e = r;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t n = (size_t)(e - r);
        if ((n == 1 && r[0] == '.') ) { r = e; continue; }
        if (n == 2 && r[0] == '.' && r[1] == '.') return false;
        if (w != p) *w++ = '/';
        for (size_t i = 0; i < n; i++) {
            char c = r[i];
            if ((unsigned char)c < 0x20 || strchr(":*?\"<>|", c)) c = '_';
            *w++ = c;
        }
        r = e;
    }
    *w = '\0';
    return p[0] != '\0';
}

typedef struct {
    int strip;                 /* leading folders to drop (0 or 1) */
    char prepend[MAX_NAME];    /* folder to put in front, "" or "apps/x/" */
    bool drop_docs;            /* skip loose readme-type files at the top */
} layout_t;

typedef struct { char *name; bool dir; u64 size; } ent_t;

static const char *after_first(const char *p)
{
    const char *s = strchr(p, '/');
    return s ? s + 1 : NULL;
}

static void archive_stem(const char *arc_path, char *out, size_t n)
{
    const char *b = vfs_basename(arc_path);
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
    for (char *c = out; *c; c++)
        if (strchr(":*?\"<>|/\\", *c)) *c = '_';
    if (!out[0]) snprintf(out, n, "app");
}

static void decide_layout(const ent_t *e, int n, const char *stem, layout_t *L)
{
    memset(L, 0, sizeof(*L));
    char top1[MAX_NAME] = "";
    int ndirs_top = 0, nloose = 0, nroots = 0, nfiles = 0;
    bool multi_top = false, only_wad = true, has_exec = false;
#ifdef __WIIU__
    bool only_wps = true;
#endif

    for (int i = 0; i < n; i++) {
        const char *p = e[i].name;
        const char *sl = strchr(p, '/');
        if (!sl) {
            if (!e[i].dir) nloose++;
            else if (!top1[0]) { snprintf(top1, sizeof(top1), "%s", p); ndirs_top++; }
            else if (strcasecmp(top1, p)) multi_top = true;
        } else {
            size_t l = (size_t)(sl - p);
            char t[MAX_NAME];
            snprintf(t, sizeof(t), "%.*s", (int)l, p);
            if (!top1[0]) { snprintf(top1, sizeof(top1), "%s", t); ndirs_top++; }
            else if (strcasecmp(top1, t)) multi_top = true;
            if (is_root_name(t, l)) nroots++;
        }
        if (!e[i].dir) {
            nfiles++;
            const char *x = ext_of(p);
            if (strcasecmp(x, "wad")) only_wad = false;
#ifdef __WIIU__
            if (strcasecmp(x, "wps")) only_wps = false;
#endif
            if (!strcasecmp(x, "dol") || !strcasecmp(x, "elf") || !strcasecmp(x, "rpx") || !strcasecmp(x, "wuhb"))
                has_exec = true;
        }
    }

    if (nroots) {                       /* already laid out like the drive */
        L->drop_docs = true;
        return;
    }
    if (ndirs_top == 1 && !multi_top && nloose == 0) {
        /* one wrapper folder: is the drive layout inside it? */
        bool inner_roots = false;
        for (int i = 0; i < n; i++) {
            const char *rest = after_first(e[i].name);
            if (!rest) continue;
            const char *sl = strchr(rest, '/');
            if (sl && is_root_name(rest, (size_t)(sl - rest))) { inner_roots = true; break; }
            if (!sl && e[i].dir && is_root_name(rest, strlen(rest))) { inner_roots = true; break; }
        }
        if (inner_roots) { L->strip = 1; L->drop_docs = true; return; }
        if (nfiles && only_wad) { L->strip = 1; snprintf(L->prepend, sizeof(L->prepend), "wad/"); return; }
#ifdef __WIIU__
        if (nfiles && only_wps) { L->strip = 1; snprintf(L->prepend, sizeof(L->prepend), "wiiu/environments/aroma/plugins/"); return; }
#endif
        snprintf(L->prepend, sizeof(L->prepend), "%s", APPS_BASE);      /* the folder is the app */
        return;
    }
    /* loose files, or several unrelated folders */
    if (nfiles && only_wad) { snprintf(L->prepend, sizeof(L->prepend), "wad/"); return; }
#ifdef __WIIU__
    if (nfiles && only_wps) { snprintf(L->prepend, sizeof(L->prepend), "wiiu/environments/aroma/plugins/"); return; }
    if (nfiles == 1 && nloose == 1) {
        for (int i = 0; i < n; i++)
            if (!e[i].dir && !strcasecmp(ext_of(e[i].name), "wuhb")) { snprintf(L->prepend, sizeof(L->prepend), "wiiu/apps/"); return; }
    }
#endif
    (void)has_exec;
    snprintf(L->prepend, sizeof(L->prepend), "%s%s/", APPS_BASE, stem);
}

/* Archive path -> path relative to the drive root, or false to skip it. */
static bool map_name(const layout_t *L, const char *name, char *out, size_t n)
{
    const char *p = name;
    for (int i = 0; i < L->strip; i++) {
        p = after_first(p);
        if (!p) return false;
    }
    if (L->drop_docs && !strchr(p, '/') && is_doc_ext(ext_of(p)))
        return false;
    int w = snprintf(out, n, "%s%s", L->prepend, p);
    return w > 0 && (size_t)w < n;
}

/* ---------------------------------------------------------------- stream */

typedef struct { vfs_file f; u64 pos, size; } vstream;

static void vs_close(void *d) { vstream *s = d; vfs_close(&s->f); free(s); }
static size_t vs_read(void *d, void *buf, size_t n)
{
    vstream *s = d;
    size_t got = 0;
    while (got < n) {
        s32 r = vfs_read(&s->f, (u8 *)buf + got, (u32)(n - got > 0x100000 ? 0x100000 : n - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    s->pos += got;
    return got;
}
static bool vs_seek(void *d, off64_t off, int origin)
{
    vstream *s = d;
    s64 t = origin == SEEK_SET ? off : origin == SEEK_CUR ? (s64)s->pos + off : (s64)s->size + off;
    if (t < 0 || (u64)t > s->size) return false;
    if (!vfs_seek(&s->f, (u64)t)) return false;
    s->pos = (u64)t;
    return true;
}
static off64_t vs_tell(void *d) { return (off64_t)((vstream *)d)->pos; }

static ar_stream *open_stream(const char *path)
{
    vstream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (!vfs_open_read(&s->f, path)) { free(s); return NULL; }
    s->size = s->f.size;
    ar_stream *st = ar_open_stream(s, vs_close, vs_read, vs_seek, vs_tell);
    if (!st) { vfs_close(&s->f); free(s); }
    return st;
}

/* ---------------------------------------------------------------- helpers */

bool install_is_archive_name(const char *name)
{
    const char *e = ext_of(name);
    return !strcasecmp(e, "zip") || !strcasecmp(e, "7z") || !strcasecmp(e, "rar");
}

int install_pick_device(bool want_usb)
{
    if (!want_usb)
        return g_devs[DEV_SD].mounted ? DEV_SD : -1;
    for (int d = DEV_USB0; d <= DEV_USB7; d++)
        if (g_devs[d].mounted)
            return d;
    return -1;
}

static void set_err(char *err, size_t n, const char *m) { if (err && n) snprintf(err, n, "%s", m); }

static bool mkdirs(const char *dir)
{
    bool is_dir;
    if (vfs_stat(dir, &is_dir, NULL)) return is_dir;
    char parent[VFS_PATH_MAX];
    if (!vfs_parent(dir, parent, sizeof(parent))) return false;
    if (!vfs_is_root(parent) && !mkdirs(parent)) return false;
    return vfs_mkdir(dir) || (vfs_stat(dir, &is_dir, NULL) && is_dir);
}

/* One reader over both back ends: unarr (zip, 7z, RAR 1.5 - 4.x) and rar5.c. */
typedef struct {
    ar_archive *ar;
    rar5_t *r5;
    rar5_entry e5;
    bool have_cur;      /* unarr: an entry is already parsed (after a rewind) */
} reader_t;

static bool reader_open(reader_t *rd, ar_stream *st, char *err, size_t errsz)
{
    u8 m[8] = {0};
    memset(rd, 0, sizeof(*rd));
    ar_read(st, m, 8);
    ar_seek(st, 0, SEEK_SET);
    if (!memcmp(m, "Rar!\x1a\x07\x01", 7)) {
        rd->r5 = rar5_open(st);
        if (rd->r5) return true;
    } else if (!memcmp(m, "Rar!\x1a\x07\x00", 7)) rd->ar = ar_open_rar_archive(st);
    else if (!memcmp(m, "7z\xBC\xAF\x27\x1C", 6)) rd->ar = ar_open_7z_archive(st);
    else if (m[0] == 'P' && m[1] == 'K') rd->ar = ar_open_zip_archive(st, false);
    if (!rd->ar) set_err(err, errsz, "Not a readable zip / 7z / rar archive");
    return rd->ar != NULL;
}

static void reader_close(reader_t *rd)
{
    if (rd->ar) ar_close_archive(rd->ar);
    if (rd->r5) rar5_close(rd->r5);
}

/* Next entry: 1 = got one, 0 = end, -1 = error. `name` is the raw archive path. */
static int reader_next(reader_t *rd, char *name, size_t n, bool *dir, u64 *size, char *err, size_t errsz)
{
    if (rd->r5) {
        for (;;) {
            int r = rar5_next(rd->r5, &rd->e5);
            if (r < 0) { set_err(err, errsz, rar5_error(rd->r5)); return -1; }
            if (r == 0) return 0;
            if (rd->e5.skip && !rd->e5.dir) {          /* link: nothing to install */
                rar5_extract(rd->r5, NULL, NULL);
                continue;
            }
            snprintf(name, n, "%s", rd->e5.name);
            *dir = rd->e5.dir;
            *size = rd->e5.size;
            return 1;
        }
    }
    if (rd->have_cur)
        rd->have_cur = false;
    else if (!ar_parse_entry(rd->ar))
        return ar_at_eof(rd->ar) ? 0 : (set_err(err, errsz, "The archive is damaged or uses an unsupported method"), -1);
    const char *nm = ar_entry_get_name(rd->ar);
    if (!nm) { name[0] = '\0'; *dir = false; *size = 0; return 1; }
    snprintf(name, n, "%s", nm);
    size_t nl = strlen(name);
    *dir = nl && (name[nl - 1] == '/' || name[nl - 1] == '\\');
    *size = *dir ? 0 : ar_entry_get_size(rd->ar);
    return 1;
}

static bool reader_rewind(reader_t *rd)
{
    if (rd->r5) return rar5_rewind(rd->r5);
    rd->have_cur = ar_parse_entry_at(rd->ar, 0);
    return rd->have_cur;
}

typedef struct {
    vfs_file f;
    install_cb cb;
    const char *rel;
    u64 *done, total;
    char *err; size_t errsz;
    bool ok;
} wsink_t;

static bool wsink(void *ud, const u8 *p, size_t n)
{
    wsink_t *w = ud;
    if (vfs_write(&w->f, p, (u32)n) < 0) { set_err(w->err, w->errsz, g_write_err ? g_write_err : "Write error (drive full?)"); w->ok = false; return false; }
    *w->done += n;
    if (w->cb && !w->cb(w->rel, *w->done, w->total)) { set_err(w->err, w->errsz, "Cancelled"); w->ok = false; return false; }
    return true;
}

/* Unpacks the current entry into `w` (w == NULL: just skip it). */
static bool reader_extract(reader_t *rd, u64 size, wsink_t *w, char *buf, char *err, size_t errsz)
{
    if (rd->r5) {
        if (!w) return rar5_extract(rd->r5, NULL, NULL) || (set_err(err, errsz, rar5_error(rd->r5)), false);
        if (rar5_extract(rd->r5, wsink, w)) return true;
        if (w->ok) set_err(err, errsz, rar5_error(rd->r5));
        return false;
    }
    if (!w) return true;
    size_t left = (size_t)size;
    while (left) {
        size_t c = left > CHUNK ? CHUNK : left;
        if (!ar_entry_uncompress(rd->ar, buf, c)) { set_err(err, errsz, "Couldn't unpack a file (damaged, encrypted or out of memory)"); return false; }
        if (!wsink(w, (u8 *)buf, c)) return false;
        left -= c;
    }
    return true;
}

/* ---------------------------------------------------------------- install */

int install_archive(const char *arc_path, int dev, install_cb cb, char *err, size_t errsz)
{
    if (err && errsz) err[0] = '\0';
    if (dev < 0 || !g_devs[dev].mounted) { set_err(err, errsz, "No SD card / USB drive to install to"); return -1; }

    ar_stream *st = open_stream(arc_path);
    if (!st) { set_err(err, errsz, "Can't open the archive"); return -1; }
    reader_t rd;
    if (!reader_open(&rd, st, err, errsz)) { ar_close(st); return -1; }

    ent_t *ents = NULL;
    int n = 0, cap = 0;
    u64 total = 0, done_bytes = 0;
    int result = -1;
    layout_t L;
    char stem[128], *buf = NULL, root[VFS_PATH_MAX], raw[MAX_NAME + 8];
    snprintf(root, sizeof(root), "%s", vfs_dev_root(dev));

    /* pass 1: list everything */
    for (;;) {
        bool dir;
        u64 size;
        int r = reader_next(&rd, raw, sizeof(raw), &dir, &size, err, errsz);
        if (r < 0) goto done;
        if (!r) break;
        if (strlen(raw) >= MAX_NAME) { set_err(err, errsz, "A file name in the archive is too long"); goto done; }
        if (!clean_path(raw) || is_junk(raw)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            ent_t *ne = realloc(ents, (size_t)cap * sizeof(*ents));
            if (!ne) { set_err(err, errsz, "Out of memory"); goto done; }
            ents = ne;
        }
        ents[n].name = strdup(raw);
        ents[n].dir = dir;
        ents[n].size = size;
        if (!ents[n].name) { set_err(err, errsz, "Out of memory"); goto done; }
        total += size;
        n++;
    }
    if (!n) { set_err(err, errsz, "The archive is empty"); goto done; }

    archive_stem(arc_path, stem, sizeof(stem));
    decide_layout(ents, n, stem, &L);

    vfs_update_space(dev);
    if (g_devs[dev].space_known && total + (1u << 20) > g_devs[dev].free) {
        set_err(err, errsz, "Not enough free space on the drive");
        goto done;
    }
    buf = memalign(32, CHUNK);
    if (!buf) { set_err(err, errsz, "Out of memory"); goto done; }

    /* pass 2: unpack */
    if (!reader_rewind(&rd)) { set_err(err, errsz, "Couldn't re-read the archive"); goto done; }
    int written = 0;
    char made[VFS_PATH_MAX] = "";
    for (;;) {
        bool dir;
        u64 size;
        int r = reader_next(&rd, raw, sizeof(raw), &dir, &size, err, errsz);
        if (r < 0) goto done;
        if (!r) break;
        char rel[MAX_NAME];
        char dest[VFS_PATH_MAX];
        bool want = strlen(raw) < MAX_NAME && clean_path(raw) && !is_junk(raw) && map_name(&L, raw, rel, sizeof(rel)) &&
                    vfs_join(dest, sizeof(dest), root, rel);
        if (!want || dir) {
            if (want && dir && !mkdirs(dest)) { set_err(err, errsz, "Can't create a folder on the drive"); goto done; }
            if (!dir && !reader_extract(&rd, size, NULL, buf, err, errsz)) goto done;
            continue;
        }
        char parent[VFS_PATH_MAX];
        if (!vfs_parent(dest, parent, sizeof(parent))) continue;
        if (strcmp(parent, made)) {
            if (!mkdirs(parent)) { set_err(err, errsz, "Can't create a folder on the drive"); goto done; }
            snprintf(made, sizeof(made), "%s", parent);
        }
        bool is_dir;
        if (vfs_stat(dest, &is_dir, NULL) && is_dir) { set_err(err, errsz, "A folder is in the way of a file"); goto done; }

        wsink_t w = { .cb = cb, .rel = rel, .done = &done_bytes, .total = total, .err = err, .errsz = errsz, .ok = true };
        if (!vfs_open_write(&w.f, dest)) { set_err(err, errsz, "Can't create a file on the drive"); goto done; }
        bool ok = (!cb || cb(rel, done_bytes, total)) ? reader_extract(&rd, size, &w, buf, err, errsz) : false;
        if (!vfs_close_checked(&w.f) && ok) { set_err(err, errsz, "Write error (drive full?)"); ok = false; }
        if (!ok) {
            vfs_remove(dest, false);
            if (err && errsz && !err[0]) set_err(err, errsz, "Cancelled");
            goto done;
        }
        written++;
    }
    result = written;

done:
    for (int i = 0; i < n; i++) free(ents[i].name);
    free(ents);
    free(buf);
    reader_close(&rd);
    ar_close(st);
    return result;
}

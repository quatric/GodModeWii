/*
 * Making a .zip from files and folders (folders with everything in them). Deflated with zlib, a file at a time
 * from the drive to the zip, so any size fits in memory; files that are compressed already (.zip, .png, .mp3,
 * disc images...) are stored as they are. No ZIP64: each file and the whole zip stay under 4 GB. Names are
 * UTF-8 (flag bit 11), relative to the folder the items are in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <time.h>
#include <zlib.h>

#include "zipwrite.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"

#define BUF (64 * 1024)
#define LIMIT 0xFFFFFFFFull

typedef struct {
    char *name;          /* in the zip: "a/b.txt", "a/" for a folder */
    u32 crc, csize, usize, off;
    u16 method;
} zent;

typedef struct {
    vfs_file out;
    u64 pos;
    zent *e;
    int n, cap;
    u16 dtime, ddate;
    u64 done, total;
    u8 *in, *zb;
    char *err;
    size_t en;
    bool cancelled;
} zctx;

static void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool out(zctx *z, const void *b, u32 n)
{
    if (!n) return true;
    if (vfs_write(&z->out, b, n) != (s32)n) {
        snprintf(z->err, z->en, "Couldn't write the zip (is the drive full?).");
        return false;
    }
    z->pos += n;
    return true;
}

static bool stored_ext(const char *name)
{
    static const char *const exts[] = { ".zip", ".7z", ".rar", ".gz", ".bz2", ".xz", ".png", ".jpg", ".jpeg", ".webp",
                                        ".gif", ".mp3", ".m4a", ".ogg", ".flac", ".mp4", ".mkv", ".rvz", ".wia",
                                        ".ciso", ".wbfs", ".lzma", ".zst" };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (!strcasecmp(dot, exts[i])) return true;
    return false;
}

static zent *new_entry(zctx *z, const char *name)
{
    if (z->n == z->cap) {
        int nc = z->cap ? z->cap * 2 : 64;
        zent *e = realloc(z->e, nc * sizeof(*e));
        if (!e) return NULL;
        z->e = e;
        z->cap = nc;
    }
    zent *e = &z->e[z->n];
    memset(e, 0, sizeof(*e));
    e->name = strdup(name);
    if (!e->name) return NULL;
    z->n++;
    return e;
}

/* Local header now; its CRC and sizes are filled in once the data is written. */
static bool local_header(zctx *z, zent *e)
{
    u8 h[30];
    size_t nl = strlen(e->name);
    if (z->pos > LIMIT) { snprintf(z->err, z->en, "The zip would pass 4 GB, which a zip without ZIP64 can't hold."); return false; }
    e->off = (u32)z->pos;
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 6, 0x0800);
    put16(h + 8, e->method);
    put16(h + 10, z->dtime);
    put16(h + 12, z->ddate);
    put32(h + 14, e->crc);
    put32(h + 18, e->csize);
    put32(h + 22, e->usize);
    put16(h + 26, (u16)nl);
    put16(h + 28, 0);
    return out(z, h, 30) && out(z, e->name, nl);
}

static bool add_file(zctx *z, const char *path, const char *name, u64 size)
{
    if (size >= LIMIT) {
        snprintf(z->err, z->en, "%s is 4 GB or more, which a zip without ZIP64 can't hold.", name);
        return false;
    }
    zent *e = new_entry(z, name);
    if (!e) { snprintf(z->err, z->en, "Out of memory."); return false; }
    e->method = size && !stored_ext(name) ? 8 : 0;
    e->usize = (u32)size;
    if (!local_header(z, e)) return false;
    u64 data_start = z->pos;

    vfs_file f;
    if (!vfs_open_read(&f, path)) { snprintf(z->err, z->en, "Couldn't open %s.", path); return false; }
    vfs_unbuffered(&f);
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (e->method == 8 && deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        vfs_close(&f);
        snprintf(z->err, z->en, "Out of memory.");
        return false;
    }
    uLong crc = crc32(0, NULL, 0);
    u64 left = size;
    bool ok = true;
    do {
        u32 k = left < BUF ? (u32)left : BUF;
        if (k && vfs_read(&f, z->in, k) != (s32)k) { snprintf(z->err, z->en, "Couldn't read %s.", path); ok = false; break; }
        crc = crc32(crc, z->in, k);
        left -= k;
        if (e->method == 0) {
            ok = out(z, z->in, k);
        } else {
            s.next_in = z->in;
            s.avail_in = k;
            int flush = left ? Z_NO_FLUSH : Z_FINISH, r;
            do {
                s.next_out = z->zb;
                s.avail_out = BUF;
                r = deflate(&s, flush);
                ok = r != Z_STREAM_ERROR && out(z, z->zb, BUF - s.avail_out);
            } while (ok && s.avail_out == 0);
        }
        z->done += k;
        ui_busy_progress(z->done, z->total);
        if (input_cancel_held()) { z->cancelled = true; ok = false; }
    } while (ok && left);
    if (e->method == 8) deflateEnd(&s);
    vfs_close(&f);
    if (!ok) return false;

    u64 csize = z->pos - data_start;
    if (csize >= LIMIT) { snprintf(z->err, z->en, "The zip would pass 4 GB, which a zip without ZIP64 can't hold."); return false; }
    e->crc = (u32)crc;
    e->csize = (u32)csize;
    u8 t[12];
    put32(t, e->crc);
    put32(t + 4, e->csize);
    put32(t + 8, e->usize);
    if (!vfs_seek(&z->out, e->off + 14) || vfs_write(&z->out, t, 12) != 12 || !vfs_seek(&z->out, z->pos)) {
        snprintf(z->err, z->en, "Couldn't write the zip.");
        return false;
    }
    return true;
}

/* Sizes first, for the progress bar. */
static u64 tree_size(const char *path, int depth)
{
    bool is_dir;
    u64 size = 0;
    if (!vfs_stat(path, &is_dir, &size)) return 0;
    if (!is_dir || depth > 32) return size;
    vfs_entry *list;
    int cnt;
    if (!vfs_list(path, &list, &cnt)) return 0;
    u64 t = 0;
    for (int i = 0; i < cnt; i++) {
        char p[VFS_PATH_MAX];
        if (vfs_join(p, sizeof(p), path, list[i].name)) t += list[i].is_dir ? tree_size(p, depth + 1) : list[i].size;
    }
    free(list);
    return t;
}

static bool add_tree(zctx *z, const char *path, const char *name, int depth)
{
    bool is_dir;
    u64 size = 0;
    if (!vfs_stat(path, &is_dir, &size)) { snprintf(z->err, z->en, "%s is gone.", path); return false; }
    if (!is_dir) return add_file(z, path, name, size);
    if (depth > 32) { snprintf(z->err, z->en, "The folders go too deep."); return false; }
    char dname[VFS_PATH_MAX];
    snprintf(dname, sizeof(dname), "%s/", name);
    zent *e = new_entry(z, dname);
    if (!e || !local_header(z, e)) { if (!e) snprintf(z->err, z->en, "Out of memory."); return false; }
    vfs_entry *list;
    int cnt;
    if (!vfs_list(path, &list, &cnt)) { snprintf(z->err, z->en, "Couldn't read the folder %s.", path); return false; }
    bool ok = true;
    for (int i = 0; i < cnt && ok; i++) {
        char p[VFS_PATH_MAX], n[VFS_PATH_MAX];
        if (!vfs_join(p, sizeof(p), path, list[i].name) || snprintf(n, sizeof(n), "%s/%s", name, list[i].name) >= (int)sizeof(n)) {
            snprintf(z->err, z->en, "A name in %s is too long.", path);
            ok = false;
        } else {
            ok = add_tree(z, p, n, depth + 1);
        }
    }
    free(list);
    return ok;
}

static bool central(zctx *z)
{
    u64 cd = z->pos;
    for (int i = 0; i < z->n; i++) {
        zent *e = &z->e[i];
        u8 h[46];
        size_t nl = strlen(e->name);
        bool dir = nl && e->name[nl - 1] == '/';
        put32(h, 0x02014b50);
        put16(h + 4, 20);
        put16(h + 6, 20);
        put16(h + 8, 0x0800);
        put16(h + 10, e->method);
        put16(h + 12, z->dtime);
        put16(h + 14, z->ddate);
        put32(h + 16, e->crc);
        put32(h + 20, e->csize);
        put32(h + 24, e->usize);
        put16(h + 28, (u16)nl);
        put16(h + 30, 0);
        put16(h + 32, 0);
        put16(h + 34, 0);
        put16(h + 36, 0);
        put32(h + 38, dir ? 0x10 : 0);
        put32(h + 42, e->off);
        if (!out(z, h, 46) || !out(z, e->name, nl)) return false;
    }
    if (z->pos > LIMIT || z->n > 0xFFFF) { snprintf(z->err, z->en, "Too big or too many files for a zip without ZIP64."); return false; }
    u8 end[22];
    put32(end, 0x06054b50);
    put16(end + 4, 0);
    put16(end + 6, 0);
    put16(end + 8, (u16)z->n);
    put16(end + 10, (u16)z->n);
    put32(end + 12, (u32)(z->pos - cd));
    put32(end + 16, (u32)cd);
    put16(end + 20, 0);
    return out(z, end, 22);
}

bool zip_create(char **paths, int n, const char *zip_path, char *err, size_t en)
{
    zctx z;
    memset(&z, 0, sizeof(z));
    z.err = err;
    z.en = en;
    err[0] = '\0';
    time_t now = time(NULL);
    struct tm tmv;
    if (now > 315532800 && localtime_r(&now, &tmv)) {       /* DOS dates start in 1980 */
        z.dtime = (u16)(tmv.tm_hour << 11 | tmv.tm_min << 5 | tmv.tm_sec / 2);
        z.ddate = (u16)((tmv.tm_year - 80) << 9 | (tmv.tm_mon + 1) << 5 | tmv.tm_mday);
    } else {
        z.ddate = 1 << 5 | 1;
    }
    z.in = memalign(32, BUF);
    z.zb = memalign(32, BUF);
    bool ok = z.in && z.zb && vfs_open_write(&z.out, zip_path);
    if (!ok && !err[0]) snprintf(err, en, z.in && z.zb ? "Couldn't create %s." : "Out of memory.", zip_path);
    bool opened = ok;
    ui_busy_begin("Zipping...");
    for (int i = 0; ok && i < n; i++) z.total += tree_size(paths[i], 0);
    for (int i = 0; ok && i < n; i++) ok = add_tree(&z, paths[i], vfs_basename(paths[i]), 0);
    ok = ok && central(&z);
    ui_busy_end();
    if (opened) {
        ok = vfs_close_checked(&z.out) && ok;
        if (!ok) vfs_remove(zip_path, false);
    }
    if (z.cancelled) snprintf(err, en, "Stopped: nothing was saved.");
    for (int i = 0; i < z.n; i++) free(z.e[i].name);
    free(z.e);
    free(z.in);
    free(z.zb);
    return ok;
}

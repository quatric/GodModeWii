/*
 * Save data -> data.bin. See databin.h.
 *
 * Layout and signature as Dolphin's WiiSave export writes them (Source/Core/Core/HW/WiiSave.cpp,
 * GPL-2.0+); the code is GodModeWii's own. The AP signature is wad2bin's (w2b_ap_sign).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/isfs.h>

#include "databin.h"
#include "vfs.h"
#include "aes.h"
#include "sha1.h"
#include "md5.h"
#include "archive.h"

#define HDR_SIZE   0xF0C0u
#define BNR_MAX    0xF0A0u
#define BNR_MIN    0x72A0u
#define BNR_SZ     0x60A0u
#define ICON_SZ    0x1200u
#define BK_SIZE    0x80u
#define FHDR_SIZE  0x80u
#define CERTS_SIZE 0x3C0u                /* Bk header + signature + device and AP certificates */
#define FILE_MAGIC 0x03ADF17Eu
#define NAME_MAX_  0x40
#define CHUNK      0x10000u
#define MAX_FILES  2048

static const u8 MD5_BLANKER[16] = { 0x0E, 0x65, 0x37, 0x81, 0x99, 0xBE, 0x45, 0x17,
                                    0xAB, 0x06, 0xEC, 0x22, 0x45, 0x1A, 0x57, 0x93 };

static u32 a64(u32 v) { return (v + 63) & ~63u; }
static void wb32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void wb64(u8 *p, u64 v) { wb32(p, (u32)(v >> 32)), wb32(p + 4, (u32)v); }

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

void databin_dir(u64 tid, char *out, size_t n)
{
    char id[5];
    w2b_tid_ascii(tid, id);
    snprintf(out, n, "private/wii/title/%s", id);
}

typedef struct {
    char rel[NAME_MAX_ + 1];            /* below the data folder */
    bool dir;
    u32 size;
    u8 mode;
} item_t;

typedef struct {
    item_t *it;
    int n, cap;
    u32 bytes;                          /* the files part: headers and padded contents */
    bool too_long;
} list_t;

/* owner / group / other permissions, as data.bin keeps them (2 bits each) */
static u8 nand_mode(const char *path)
{
    const char *np = strncmp(path, "nand:", 5) ? NULL : path + 5;
    u32 uid;
    u16 gid;
    u8 attr, op, gp, ot;
    if (np && ISFS_GetAttr(np, &uid, &gid, &attr, &op, &gp, &ot) >= 0)
        return (u8)((op & 3) << 4 | (gp & 3) << 2 | (ot & 3));
    return 0x3C;                                        /* read / write for the owner and group */
}

static bool scan(list_t *l, const char *root, const char *rel)
{
    char dir[VFS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s%s%s", root, rel[0] ? "/" : "", rel);
    vfs_entry *e;
    int cnt;
    if (!vfs_list(dir, &e, &cnt)) return false;
    bool ok = true;
    for (int i = 0; ok && i < cnt; i++) {
        if (!rel[0] && !strcmp(e[i].name, "banner.bin")) continue;
        char r[VFS_PATH_MAX];
        snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", e[i].name);
        if (strlen(r) > NAME_MAX_) {
            l->too_long = true;
            ok = false;
            break;
        }
        if (l->n == l->cap) {
            int nc = l->cap ? l->cap * 2 : 32;
            item_t *ni = nc <= MAX_FILES ? realloc(l->it, (size_t)nc * sizeof(item_t)) : NULL;
            if (!ni) { ok = false; break; }
            l->it = ni;
            l->cap = nc;
        }
        item_t *it = &l->it[l->n++];
        memset(it, 0, sizeof(*it));
        snprintf(it->rel, sizeof(it->rel), "%s", r);
        it->dir = e[i].is_dir;
        it->size = e[i].is_dir ? 0 : (u32)e[i].size;
        char full[VFS_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", root, r);
        it->mode = nand_mode(full);
        l->bytes += FHDR_SIZE + (it->dir ? 0 : a64(it->size));
        if (it->dir) ok = scan(l, root, r);
    }
    free(e);
    return ok;
}

typedef struct {
    vfs_file f;
    u8 *buf;                            /* CHUNK + 64, 32-byte aligned */
    sha1_t sum;                         /* of everything after the header */
    bool hashing, failed;
} out_t;

static void put(out_t *o, const void *p, u32 n)
{
    if (o->hashing) sha1_update(&o->sum, p, n);
    while (n && !o->failed) {
        u32 k = n > CHUNK ? CHUNK : n;
        if (p != o->buf) memcpy(o->buf, p, k);
        if (vfs_write(&o->f, o->buf, k) != (s32)k) o->failed = true;
        p = (const u8 *)p + k;
        n -= k;
    }
}

static u8 *read_all(const char *path, u32 *len, u32 max)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    u8 *buf = NULL;
    if (f.size <= max) {
        u32 n = (u32)f.size, got = 0;
        buf = memalign(32, (n + 63) & ~31u);
        while (buf && got < n) {
            s32 r = vfs_read(&f, buf + got, n - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        if (buf && got != n) { free(buf); buf = NULL; }
        if (buf) *len = n;
    }
    vfs_close(&f);
    return buf;
}

int databin_export(u64 tid, const char *data_dir, const char *out_path, const w2b_keys_t *k, char *err, size_t en)
{
    int ret = -1;
    list_t l = { 0 };
    u8 *hdr = NULL, *bn = NULL, *rbuf = NULL;
    bool opened = false;
    out_t o;
    memset(&o, 0, sizeof(o));
    char path[VFS_PATH_MAX + 64];

    /* the banner is what the Wii shows in Data Management: a save without one can't be a data.bin */
    snprintf(path, sizeof(path), "%s/banner.bin", data_dir);
    u32 blen = 0;
    bn = read_all(path, &blen, BNR_MAX);
    if (!bn) {
        seterr(err, en, "No banner.bin, so the Wii couldn't show it (not a copyable save)");
        goto out;
    }
    if (blen < BNR_MIN || blen > BNR_MAX || (blen - BNR_SZ) % ICON_SZ) {
        seterr(err, en, "banner.bin has an odd size (%u bytes)", (unsigned)blen);
        goto out;
    }
    if (!scan(&l, data_dir, "")) {
        seterr(err, en, l.too_long ? "A file name is longer than data.bin allows (64 characters)"
                                   : "Couldn't list the save's files");
        goto out;
    }

    hdr = memalign(32, HDR_SIZE);
    o.buf = memalign(32, CHUNK + 64);
    rbuf = memalign(32, CHUNK + 64);
    if (!hdr || !o.buf || !rbuf) {
        seterr(err, en, "Out of memory");
        goto out;
    }

    /* the header: title ID, banner size, its permissions, MD5 (taken with the blanker in its place), banner */
    memset(hdr, 0, HDR_SIZE);
    wb64(hdr, tid);
    wb32(hdr + 8, blen);
    hdr[12] = nand_mode(path);
    memcpy(hdr + 14, MD5_BLANKER, 16);
    memcpy(hdr + 0x20, bn, blen);
    hdr[0x20 + 7] &= ~1;                                /* clear the "no copy" flag, as Dolphin does */
    u8 h[16];
    md5(hdr, HDR_SIZE, h);
    memcpy(hdr + 14, h, 16);
    aes128_enc_t sd;
    u8 iv[16];
    aes128_init_enc(&sd, ARC_SD_KEY);
    memcpy(iv, ARC_SD_IV, 16);
    aes128_cbc_encrypt(&sd, iv, hdr, hdr, HDR_SIZE);

    if (!vfs_open_write(&o.f, out_path)) {
        seterr(err, en, "Can't create %s (is the drive writable?)", out_path);
        goto out;
    }
    opened = true;
    vfs_unbuffered(&o.f);
    put(&o, hdr, HDR_SIZE);

    u8 bk[BK_SIZE];
    memset(bk, 0, sizeof(bk));
    wb32(bk, 0x70);
    wb32(bk + 4, 0x426B0001);
    wb32(bk + 8, k->console_id);
    wb32(bk + 0x0C, (u32)l.n);
    wb32(bk + 0x10, l.bytes);
    wb32(bk + 0x1C, l.bytes + CERTS_SIZE);
    wb64(bk + 0x60, tid);
    sha1_init(&o.sum);
    o.hashing = true;
    put(&o, bk, sizeof(bk));

    for (int i = 0; i < l.n && !o.failed; i++) {
        const item_t *it = &l.it[i];
        u8 fh[FHDR_SIZE];
        memset(fh, 0, sizeof(fh));
        wb32(fh, FILE_MAGIC);
        wb32(fh + 4, it->size);
        fh[8] = it->mode;
        fh[10] = it->dir ? 2 : 1;
        memcpy(fh + 11, it->rel, strlen(it->rel));
        memcpy(fh + 0x50, ARC_SD_IV, 16);               /* each file's IV, kept in its header */
        put(&o, fh, sizeof(fh));
        if (it->dir) continue;

        vfs_file f;
        snprintf(path, sizeof(path), "%s/%s", data_dir, it->rel);
        if (!vfs_open_read(&f, path)) {
            seterr(err, en, "Can't read %s", it->rel);
            goto out;
        }
        vfs_unbuffered(&f);
        memcpy(iv, fh + 0x50, 16);
        u32 left = it->size;
        bool rd = true;
        while (left) {
            u32 n = left > CHUNK ? CHUNK : left, got = 0;
            while (got < n) {
                s32 r = vfs_read(&f, rbuf + got, n - got);
                if (r <= 0) break;
                got += (u32)r;
            }
            if (got != n) { rd = false; break; }
            left -= n;
            u32 p = left ? n : a64(n);                  /* the last piece is padded to 64 bytes */
            memset(rbuf + n, 0, p - n);
            aes128_cbc_encrypt(&sd, iv, rbuf, rbuf, p);
            put(&o, rbuf, p);
        }
        vfs_close(&f);
        if (!rd) {
            seterr(err, en, "Can't read %s", it->rel);
            goto out;
        }
    }
    memset(&sd, 0, sizeof(sd));

    /* signature: AP key over SHA-1(SHA-1(Bk header .. last file)), "/Sii", device and AP certificates */
    o.hashing = false;
    u8 d1[20], d2[20], sig[0x40], ap[0x180];
    sha1_final(&o.sum, d1);
    sha1_t c;
    sha1_init(&c);
    sha1_update(&c, d1, 20);
    sha1_final(&c, d2);
    memset(sig, 0, sizeof(sig));
    if (!w2b_ap_sign(k, d2, sig, sig + 30, ap)) {
        seterr(err, en, "The console's key doesn't match its device certificate");
        goto out;
    }
    wb32(sig + 0x3C, 0x2F536969);
    put(&o, sig, sizeof(sig));
    put(&o, k->device_cert, 0x180);
    put(&o, ap, sizeof(ap));
    if (o.failed) {
        seterr(err, en, "Couldn't write the data.bin (is the drive full?)");
        goto out;
    }
    opened = false;
    if (!vfs_close_checked(&o.f)) {
        vfs_remove(out_path, false);
        seterr(err, en, "Couldn't finish the data.bin (is the drive full?)");
        goto out;
    }
    ret = 0;

out:
    if (opened) {
        vfs_close(&o.f);
        vfs_remove(out_path, false);
    }
    free(l.it);
    free(hdr);
    free(bn);
    free(o.buf);
    free(rbuf);
    return ret;
}

#ifdef GMW_AUTOTEST
#include "ecc233.h"
/* Test builds: "@databin <title id> <out>" with a fixed test console (ID 0BADC0DE), logged. */
void databin_autotest(const char *tid_s, const char *outp)
{
    w2b_keys_t k;
    memset(&k, 0, sizeof(k));
    k.console_id = 0x0BADC0DE;
    for (int i = 0; i < 30; i++) k.ng_priv[i] = (u8)(0x11 * (i + 1));
    k.ng_priv[0] = 0x01;
    wb32(k.device_cert, 0x00010002);
    snprintf((char *)k.device_cert + 0x80, 0x40, "Root-CA00000001-MS00000002");
    wb32(k.device_cert + 0xC0, 2);
    snprintf((char *)k.device_cert + 0xC4, 0x40, "NG%08x", (unsigned)k.console_id);
    wb32(k.device_cert + 0x104, k.console_id);
    ecc233_public_key(k.ng_priv, k.device_cert + 0x108);
    u64 tid = strtoull(tid_s, NULL, 16);
    char dir[96], err[160] = "";
    snprintf(dir, sizeof(dir), "nand:/title/%08x/%08x/data", (unsigned)(tid >> 32), (unsigned)tid);
    int rc = databin_export(tid, dir, outp, &k, err, sizeof(err));
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "databin %s -> %s: %s %s\n", dir, outp, rc ? "FAILED" : "ok", err);
        fclose(lg);
    }
}
#endif

/* ---------------------------------------------------------------- import */

#include "ui.h"
#include "risk.h"
#include "wadinstall.h"

static bool read_at(vfs_file *f, u64 off, u8 *buf, u32 n)
{
    if (!vfs_seek(f, off)) return false;
    u32 got = 0;
    while (got < n) {
        s32 r = vfs_read(f, buf + got, n - got);
        if (r <= 0) return false;
        got += (u32)r;
    }
    return true;
}

bool databin_is_file(const char *path)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    bool ok = false;
    u8 *bk = memalign(32, BK_SIZE);
    if (bk && f.size >= HDR_SIZE + BK_SIZE && read_at(&f, HDR_SIZE, bk, BK_SIZE))
        ok = be32(bk) == 0x70 && be32(bk + 4) == 0x426B0001 && be32(bk + 0x10) + CERTS_SIZE == be32(bk + 0x1C) &&
             (u64)HDR_SIZE + BK_SIZE + be32(bk + 0x10) <= f.size;
    free(bk);
    vfs_close(&f);
    return ok;
}

/* Everything inside the NAND folder `dir` goes, the folder stays. */
static bool empty_dir(const char *dir)
{
    vfs_entry *e;
    int cnt;
    if (!vfs_list(dir, &e, &cnt)) return false;
    bool ok = true;
    for (int i = 0; i < cnt; i++) {
        char p[VFS_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", dir, e[i].name);
        if (e[i].is_dir && !empty_dir(p)) ok = false;
        if (!vfs_remove(p, e[i].is_dir)) ok = false;
    }
    free(e);
    return ok;
}

static void set_owner(const char *path, u32 uid, u16 gid, u8 mode)
{
    ISFS_SetAttr(path + 5, uid, gid, 0, (mode >> 4) & 3, (mode >> 2) & 3, mode & 3);   /* past "nand:" */
}

typedef struct {
    u64 tid;
    u32 banner_size;
    u8 banner_mode;
    u32 nfiles, files_size;
    char name[64];                      /* the banner's title, for the question */
} dbinfo_t;

static bool parse_header(vfs_file *f, u8 *hdr, dbinfo_t *d, char *err, size_t en)
{
    if (!read_at(f, 0, hdr, HDR_SIZE)) {
        seterr(err, en, "Can't read the data.bin");
        return false;
    }
    aes128_t c;
    aes128_init_dec(&c, ARC_SD_KEY);
    aes128_cbc_decrypt(&c, ARC_SD_IV, hdr, hdr, HDR_SIZE);
    u8 md[16], h[16];
    memcpy(md, hdr + 14, 16);
    memcpy(hdr + 14, MD5_BLANKER, 16);
    md5(hdr, HDR_SIZE, h);
    if (memcmp(md, h, 16)) {
        seterr(err, en, "The data.bin's header is damaged (its MD5 doesn't match)");
        return false;
    }
    d->tid = be64(hdr);
    d->banner_size = be32(hdr + 8);
    d->banner_mode = hdr[12];
    if (d->banner_size < BNR_MIN || d->banner_size > BNR_MAX || (d->banner_size - BNR_SZ) % ICON_SZ) {
        seterr(err, en, "The data.bin's banner has an odd size");
        return false;
    }
    /* the title, UTF-16 at 0x20 of the banner, as ASCII */
    int k = 0;
    for (int i = 0; i < 32 && k < (int)sizeof(d->name) - 1; i++) {
        u16 ch = (u16)(hdr[0x20 + 0x20 + i * 2] << 8 | hdr[0x20 + 0x21 + i * 2]);
        if (!ch) break;
        d->name[k++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
    }
    d->name[k] = 0;
    u8 *bk = memalign(32, BK_SIZE);
    bool ok = bk && read_at(f, HDR_SIZE, bk, BK_SIZE) && be32(bk) == 0x70 && be32(bk + 4) == 0x426B0001;
    if (ok) {
        d->nfiles = be32(bk + 0x0C);
        d->files_size = be32(bk + 0x10);
        ok = d->nfiles <= MAX_FILES && (u64)HDR_SIZE + BK_SIZE + d->files_size <= f->size;
    }
    free(bk);
    if (!ok) seterr(err, en, "The data.bin's file list is damaged");
    return ok;
}

/* Writes the banner and every file under `data` (emptied first). */
static bool import_files(vfs_file *f, const u8 *hdr, const dbinfo_t *d, const char *data, u32 uid, u16 gid,
                         char *err, size_t en)
{
    u8 *buf = memalign(32, CHUNK + 64);
    if (!buf) {
        seterr(err, en, "Out of memory");
        return false;
    }
    bool ok = false;
    char path[VFS_PATH_MAX + 80];
    vfs_file o;
    if (!empty_dir(data)) {
        seterr(err, en, "Couldn't clear the old save from the NAND");
        goto out;
    }
    snprintf(path, sizeof(path), "%s/banner.bin", data);
    memcpy(buf, hdr + 0x20, d->banner_size);
    if (!vfs_open_write(&o, path)) {
        seterr(err, en, "Couldn't create banner.bin on the NAND");
        goto out;
    }
    bool w = vfs_write(&o, buf, d->banner_size) == (s32)d->banner_size;
    if (!vfs_close_checked(&o) || !w) {
        seterr(err, en, "Couldn't write banner.bin on the NAND");
        goto out;
    }
    set_owner(path, uid, gid, d->banner_mode ? d->banner_mode : 0x3C);

    aes128_t c;
    aes128_init_dec(&c, ARC_SD_KEY);
    u64 pos = HDR_SIZE + BK_SIZE;
    for (u32 i = 0; i < d->nfiles; i++) {
        u8 fh[FHDR_SIZE] __attribute__((aligned(32)));
        if (!read_at(f, pos, fh, FHDR_SIZE) || be32(fh) != FILE_MAGIC) {
            seterr(err, en, "The data.bin's file list is damaged (entry %u)", (unsigned)i);
            goto out;
        }
        pos += FHDR_SIZE;
        char name[NAME_MAX_ + 1];
        memcpy(name, fh + 11, NAME_MAX_);
        name[NAME_MAX_] = 0;
        if (!name[0] || name[0] == '/' || strstr(name, "..") || (fh[10] != 1 && fh[10] != 2)) {
            seterr(err, en, "The data.bin has an entry it shouldn't (%s)", name);
            goto out;
        }
        snprintf(path, sizeof(path), "%s/%s", data, name);
        u8 mode = fh[8] ? fh[8] : 0x3C;
        if (fh[10] == 2) {
            bool isd;
            if (!vfs_mkdir(path) && !(vfs_stat(path, &isd, NULL) && isd)) {
                seterr(err, en, "Couldn't create the folder %s on the NAND", name);
                goto out;
            }
            set_owner(path, uid, gid, mode);
            continue;
        }
        u32 size = be32(fh + 4);
        u8 iv[16];
        memcpy(iv, fh + 0x50, 16);
        if (!vfs_open_write(&o, path)) {
            seterr(err, en, "Couldn't create %s on the NAND", name);
            goto out;
        }
        u32 left = size;
        bool good = true;
        while (left && good) {
            u32 n = left > CHUNK ? CHUNK : left, p = (n + 15) & ~15u;
            good = read_at(f, pos, buf, p);
            if (!good) break;
            u8 next[16];
            memcpy(next, buf + p - 16, 16);
            aes128_cbc_decrypt(&c, iv, buf, buf, p);
            memcpy(iv, next, 16);
            good = vfs_write(&o, buf, n) == (s32)n;
            pos += p;
            left -= n;
        }
        pos = pos - ((size + 15) & ~15u) + a64(size);   /* each file is padded to 64 bytes */
        if (!vfs_close_checked(&o) || !good) {
            seterr(err, en, "Couldn't copy %s to the NAND (read error, or the NAND is full)", name);
            goto out;
        }
        set_owner(path, uid, gid, mode);
    }
    ok = true;
out:
    free(buf);
    return ok;
}

void databin_import(const char *path)
{
    char err[200] = "";
    vfs_file f;
    u8 *hdr = memalign(32, HDR_SIZE);
    dbinfo_t d;
    memset(&d, 0, sizeof(d));
    if (!hdr || !vfs_open_read(&f, path)) {
        free(hdr);
        ui_message("IMPORT SAVE", C_RED, "Can't open the data.bin.");
        return;
    }
    vfs_unbuffered(&f);
    if (!parse_header(&f, hdr, &d, err, sizeof(err))) goto fail;

    char data[96];
    snprintf(data, sizeof(data), "nand:/title/%08x/%08x/data", (unsigned)(d.tid >> 32), (unsigned)d.tid);
    u32 uid;
    u16 gid;
    u8 attr, op, gp, ot;
    if (ISFS_GetAttr(data + 5, &uid, &gid, &attr, &op, &gp, &ot) < 0) {
        seterr(err, sizeof(err),
               "Title %08x-%08x has no save folder on this Wii yet. Install it, or start the game once, then "
               "import the save.", (unsigned)(d.tid >> 32), (unsigned)d.tid);
        goto fail;
    }

    char id[5], q[600];
    w2b_tid_ascii(d.tid, id);
    snprintf(q, sizeof(q),
             "Copy this save to the NAND?\n\n%s%s%s%08x-%08x (%s), %u file(s) and folder(s).\n\n"
             "The save that is there now is replaced. It is copied to GodModeWii/save-backups/ on the SD card or "
             "a USB drive first.",
             d.name[0] ? "\"" : "", d.name, d.name[0] ? "\"\n" : "", (unsigned)(d.tid >> 32), (unsigned)d.tid, id,
             (unsigned)d.nfiles);
    if (!ui_confirm("IMPORT SAVE", C_YELLOW, q)) goto done;
    if (!risk_ensure_unlocked()) goto done;

    char where[VFS_PATH_MAX] = "";
    u32 before = 0;
    ui_splash("Backing up the save that is there...");
    int b = wad_backup_save(d.tid, where, sizeof(where), &before);
    if (b < 0) {
        seterr(err, sizeof(err), "Couldn't back up the save that is there (no writable SD card or USB drive?), so "
               "nothing was changed.");
        goto fail;
    }
    ui_splash("Copying the save to the NAND...");
    if (!import_files(&f, hdr, &d, data, uid, gid, err, sizeof(err))) {
        char e2[400];
        snprintf(e2, sizeof(e2), "%s%s%s", err, b > 0 ? "\n\nThe old save is in " : "", b > 0 ? where : "");
        snprintf(err, sizeof(err), "%s", e2);
        goto fail;
    }
    char msg[VFS_PATH_MAX + 200];
    snprintf(msg, sizeof(msg), "The save of %08x-%08x is on the NAND.%s%s", (unsigned)(d.tid >> 32), (unsigned)d.tid,
             b > 0 ? "\n\nThe one it replaced is in " : "", b > 0 ? where : "");
    ui_message("SAVE IMPORTED", C_GREEN, msg);
    goto done;
fail:
    ui_message("IMPORT SAVE FAILED", C_RED, err);
done:
    vfs_close(&f);
    free(hdr);
}

#ifdef GMW_AUTOTEST
/* Test builds: "@dbimport <data.bin>", answering yes, logged. */
void databin_import_autotest(const char *path)
{
    char err[200] = "";
    vfs_file f;
    u8 *hdr = memalign(32, HDR_SIZE);
    dbinfo_t d;
    memset(&d, 0, sizeof(d));
    int rc = -1;
    if (hdr && vfs_open_read(&f, path)) {
        if (parse_header(&f, hdr, &d, err, sizeof(err))) {
            char data[96];
            snprintf(data, sizeof(data), "nand:/title/%08x/%08x/data", (unsigned)(d.tid >> 32), (unsigned)d.tid);
            u32 uid;
            u16 gid;
            u8 attr, op, gp, ot;
            if (ISFS_GetAttr(data + 5, &uid, &gid, &attr, &op, &gp, &ot) < 0)
                snprintf(err, sizeof(err), "no save folder");
            else if (import_files(&f, hdr, &d, data, uid, gid, err, sizeof(err)))
                rc = 0;
        }
        vfs_close(&f);
    }
    free(hdr);
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "dbimport %s: %s %s (%s, %u files)\n", path, rc ? "FAILED" : "ok", err, d.name, (unsigned)d.nfiles);
        fclose(lg);
    }
}
#endif

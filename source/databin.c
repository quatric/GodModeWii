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

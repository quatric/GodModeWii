/*
 * Installed title / content.bin -> installable WAD. See waddump.h.
 *
 * What goes into the WAD follows Yet Another BlueDump MOD (YABDM) by DarkMatterCore
 * (https://github.com/DarkMatterCore/bluedump-mod, GPL-3.0): a 0x20 byte "Is" header, the CA / CP / XS
 * certificates, the ticket (0x2A4 bytes), the TMD, each content AES-128-CBC encrypted with the title key
 * (IV = its index) and padded to 64 bytes, then the first content as it is on the NAND as the footer.
 * Shared contents are found by their SHA-1 in /shared1/content.map, a System Menu with Priiloader is
 * dumped from Priiloader's copies of the original (title_or.tmd, 1xxxxxxx.app), and every content is
 * checked against the TMD's hash before the file is kept. The code is GodModeWii's own.
 *
 * Unlike YABDM there are no choices: the ticket is always fakesigned and the TMD never is. A DLC with
 * pieces that were never downloaded becomes a WAD of the pieces that are there, with the whole TMD
 * (the way dlc-wad-splitter's "small" WADs and wad2bin's partial DLC WADs are), and a DLC can be cut
 * into several such WADs of a few contents each, content 0 in every one, as dlc-wad-splitter does
 * (https://github.com/DarkMatterCore/dlc-wad-splitter).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "waddump.h"
#include "vfs.h"
#include "ui.h"
#include "aes.h"
#include "sha1.h"
#include "archive.h"
#include "input.h"

#define CHUNK 0x10000u
#define TID_HI(t) ((u32)((t) >> 32))
#define TID_LO(t) ((u32)(t))

#define CERTS_LEN   0xA00u
#define TIK_LEN     0x2A4u
#define TIK_ECDH    0x180
#define TIK_KEY     0x1BF
#define TIK_CONSOLE 0x1D8
#define TIK_TID     0x1DC
#define TIK_CKIDX   0x1F1
#define TIK_PADDING 0x262
#define TIK_BODY    0x140
#define TMD_TID     0x18C
#define TMD_VERSION 0x1DC
#define TMD_NCONT   0x1DE
#define TMD_RECORDS 0x1E4
#define REC_LEN     0x24
#define FILE_MAX    (4u * 1024 * 1024)     /* tickets, TMDs, cert.sys, content.map */

static u32 a64(u32 v) { return (v + 63) & ~63u; }
static void wb16(u8 *p, u16 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
static void wb32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- sources */

static bool hex8(const char *s)
{
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* ".../title/0001000a/48414241" -> the part before "/title" ("nand:", "sd:/nand.bin|") and the title ID */
static bool title_dir(const char *path, char *nand, size_t nn, u64 *tid)
{
    size_t len = strlen(path);
    while (len && path[len - 1] == '/') len--;
    if (len < 24) return false;
    const char *t = path + len - 24;               /* "/title/" + 8 + "/" + 8 = 24 */
    if (strncasecmp(t, "/title/", 7) || !hex8(t + 7) || t[15] != '/' || !hex8(t + 16)) return false;
    if (nand) {
        size_t pre = (size_t)(t - path);
        if (pre + 1 > nn) return false;
        memcpy(nand, path, pre);
        nand[pre] = 0;
    }
    if (tid) *tid = (u64)strtoul(t + 7, NULL, 16) << 32 | (u32)strtoul(t + 16, NULL, 16);
    return true;
}

static bool is_content_bin(const char *path)
{
    return !strcasecmp(vfs_basename(path), "content.bin") && !vfs_is_archive(path);
}

bool waddump_is_source(const char *path, u64 *tid)
{
    if (tid) *tid = 0;
    if (title_dir(path, NULL, 0, tid)) {
        bool d;
        return vfs_stat(path, &d, NULL) && d;
    }
    return is_content_bin(path);
}

void waddump_name(u64 tid, u16 version, char *out, size_t n)
{
    char id[5];
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        id[i] = (char)(TID_LO(tid) >> (24 - 8 * i));
        if (!isalnum((unsigned char)id[i])) ok = false;
    }
    id[4] = 0;
    if (ok)
        snprintf(out, n, "%08x-%08x-%s-v%u.wad", (unsigned)TID_HI(tid), (unsigned)TID_LO(tid), id, version);
    else
        snprintf(out, n, "%08x-%08x-v%u.wad", (unsigned)TID_HI(tid), (unsigned)TID_LO(tid), version);
}

/* ---------------------------------------------------------------- reading */

static u8 *read_all(const char *path, u32 *len, u32 max)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    u8 *buf = NULL;
    if (f.size && f.size <= max) {
        u32 n = (u32)f.size;
        buf = memalign(32, (n + 63) & ~31u);
        u32 got = 0;
        while (buf && got < n) {
            s32 r = vfs_read(&f, buf + got, n - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        if (buf && got != n) {
            free(buf);
            buf = NULL;
        }
        if (buf) *len = n;
    }
    vfs_close(&f);
    return buf;
}

static bool exists_file(const char *path)
{
    bool d;
    return vfs_stat(path, &d, NULL) && !d;
}

/* sig type -> where a signed blob's body starts */
static u32 sig_len(u32 type)
{
    switch (type) {
    case 0x00010000: return 0x240;
    case 0x00010001: return 0x140;
    case 0x00010002: return 0x80;
    default: return 0;
    }
}

/* The CA, CP and XS certificates out of the NAND's /sys/cert.sys (stored there as XS, CA, CP), in the
 * order WADs carry them. */
static bool make_certs(const u8 *sys, u32 len, u8 out[CERTS_LEN])
{
    static const char *const want[3] = { "CA00000001", "CP00000004", "XS00000003" };
    u32 pos = 0, at = 0;
    for (int w = 0; w < 3; w++) {
        bool found = false;
        for (u32 off = 0; off + 0x100 <= len;) {
            u32 sl = sig_len(be32(sys + off));
            if (!sl || off + sl + 0x88 > len) break;
            u32 kl;
            switch (be32(sys + off + sl + 0x40)) {
            case 0: kl = 0x238; break;
            case 1: kl = 0x138; break;
            case 2: kl = 0x78; break;
            default: kl = 0; break;
            }
            u32 sz = kl ? sl + 0x88 + kl : 0;
            if (!sz || off + sz > len) break;
            if (!strncmp((const char *)sys + off + sl + 0x44, want[w], 0x40)) {
                if (at + sz > CERTS_LEN) return false;
                memcpy(out + at, sys + off, sz);
                at += sz;
                found = true;
                break;
            }
            off += sz;
        }
        if (!found) return false;
        pos++;
    }
    return at == CERTS_LEN && pos == 3;
}

/* Common key index 0, the console's own data gone, then fakesigned (the signing bug: a zero signature
 * and a body whose SHA-1 starts with 00). The title key comes back in `key`. */
static bool fix_ticket(u8 *tik, u8 key[16], char *err, size_t en)
{
    int idx = tik[TIK_CKIDX];
    u8 ck[16], iv[16];
    if (!arc_common_key(idx, ck)) {
        seterr(err, en, "The ticket uses common key %d, which isn't known", idx);
        return false;
    }
    memset(iv, 0, 16);
    memcpy(iv, tik + TIK_TID, 8);
    aes128_t d;
    aes128_init_dec(&d, ck);
    aes128_cbc_decrypt(&d, iv, tik + TIK_KEY, key, 16);
    memset(&d, 0, sizeof(d));
    if (idx != 0) {                     /* Korean / vWii: encrypt the title key again with the Wii common key */
        aes128_enc_t e;
        arc_common_key(0, ck);
        aes128_init_enc(&e, ck);
        aes128_cbc_encrypt(&e, iv, key, tik + TIK_KEY, 16);
        memset(&e, 0, sizeof(e));
        tik[TIK_CKIDX] = 0;
    }
    memset(ck, 0, sizeof(ck));

    memset(tik + TIK_ECDH, 0, 0x3C);
    memset(tik + TIK_CONSOLE, 0, 4);
    memset(tik + 4, 0, TIK_BODY - 4);
    for (u32 fill = 0; fill < 0x10000; fill++) {
        wb16(tik + TIK_PADDING, (u16)fill);
        u8 h[20];
        sha1_t s;
        sha1_init(&s);
        sha1_update(&s, tik + TIK_BODY, TIK_LEN - TIK_BODY);
        sha1_final(&s, h);
        if (!h[0]) return true;
    }
    seterr(err, en, "Couldn't fakesign the ticket");
    return false;
}

/* ---------------------------------------------------------------- the dump */

typedef struct {
    char nand[VFS_PATH_MAX];            /* the NAND's root, without its slash ("nand:") */
    char cbin[VFS_PATH_MAX];            /* a content.bin's inside ("sd:/.../content.bin|"), or "" */
    char tdir[VFS_PATH_MAX];            /* <nand>/title/<hi>/<lo> */
    bool priiloader;                    /* a System Menu dumped from Priiloader's copies */
    u8 *map;                            /* /shared1/content.map */
    u32 map_n;
} dump_t;

/* Where content `rec` is on the source (plain bytes). */
static bool locate(const dump_t *d, const u8 *rec, char *path, size_t n)
{
    u32 cid = be32(rec);
    if (d->cbin[0]) {
        snprintf(path, n, "%s/%08x.app", d->cbin, (unsigned)cid);
        if (exists_file(path)) return true;
    }
    if (d->priiloader) {
        snprintf(path, n, "%s/content/%08x.app", d->tdir, (unsigned)(0x10000000u | cid));
        if (exists_file(path)) return true;
    }
    if (!(be16(rec + 6) & 0x8000)) {
        snprintf(path, n, "%s/content/%08x.app", d->tdir, (unsigned)cid);
        if (exists_file(path)) return true;
    }
    for (u32 i = 0; d->map && i < d->map_n; i++)
        if (!memcmp(d->map + i * 28 + 8, rec + 0x10, 20)) {
            snprintf(path, n, "%s/shared1/%.8s.app", d->nand, (const char *)d->map + i * 28);
            if (exists_file(path)) return true;
        }
    return false;
}

typedef struct {
    vfs_file *out;
    u8 *buf;
    waddump_progress_fn progress;
    void *ctx;
    u64 done, total;
    char what[48];
} writer_t;

static bool put(writer_t *w, const void *p, u32 n)
{
    while (n) {
        u32 k = n > CHUNK ? CHUNK : n;
        if (p != w->buf) memcpy(w->buf, p, k);
        if (vfs_write(w->out, w->buf, k) != (s32)k) return false;
        p = (const u8 *)p + k;
        n -= k;
    }
    return true;
}

static bool zeros(writer_t *w, u32 n)
{
    if (!n) return true;
    memset(w->buf, 0, n);
    return vfs_write(w->out, w->buf, n) == (s32)n;
}

/* Copies `size` bytes of `path`, encrypted with `key` (IV = index) when key isn't NULL, checks the
 * SHA-1 against `hash` (when not NULL) and pads the output to 64 bytes. -1 I/O, -2 hash, -3 canceled. */
static int stream(writer_t *w, const char *path, u64 size, const aes128_enc_t *key, u16 index, const u8 *hash)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return -1;
    if (f.size < size) {
        vfs_close(&f);
        return -1;
    }
    vfs_unbuffered(&f);
    u8 iv[16] = { 0 };
    wb16(iv, index);
    sha1_t s;
    sha1_init(&s);
    u64 left = size;
    int rc = 0;
    while (left) {
        u32 k = left > CHUNK ? CHUNK : (u32)left;
        u32 got = 0;
        while (got < k) {
            s32 r = vfs_read(&f, w->buf + got, k - got);
            if (r <= 0) break;
            got += (u32)r;
        }
        if (got != k) { rc = -1; break; }
        sha1_update(&s, w->buf, k);
        left -= k;
        u32 out = k;
        if (key) {
            out = (k + 15) & ~15u;
            memset(w->buf + k, 0, out - k);
            aes128_cbc_encrypt(key, iv, w->buf, w->buf, out);
        }
        if (vfs_write(w->out, w->buf, out) != (s32)out) { rc = -1; break; }
        w->done += out;
        if (w->progress && !w->progress(w->ctx, w->what, w->done, w->total)) { rc = -3; break; }
    }
    vfs_close(&f);
    if (rc) return rc;
    u32 written = key ? (u32)((size + 15) & ~15ull) : (u32)size;
    if (!zeros(w, a64(written) - written)) return -1;
    w->done += a64(written) - written;
    if (hash) {
        u8 h[20];
        sha1_final(&s, h);
        if (memcmp(h, hash, 20)) return -2;
    }
    return 0;
}

int waddump_build(const char *src, const char *outpath, char *err, size_t en, waddump_progress_fn progress,
                  void *ctx)
{
    return waddump_build_part(src, outpath, 0, 0, NULL, err, en, progress, ctx);
}

int waddump_build_part(const char *src, const char *outpath, int part, int per, waddump_info_t *info, char *err,
                       size_t en, waddump_progress_fn progress, void *ctx)
{
    u8 *sel = NULL;
    dump_t d;
    memset(&d, 0, sizeof(d));
    u64 tid = 0;
    char path[VFS_PATH_MAX + 64];
    u8 *tmd = NULL, *tik = NULL, *sys = NULL, *wbuf = NULL;
    u32 tmd_sz = 0, tik_sz = 0, sys_sz = 0;
    int ret = -1;
    bool opened = false;
    vfs_file out;
    u8 key[16];
    aes128_enc_t enc;
    memset(&enc, 0, sizeof(enc));
    err[0] = 0;

    /* --- where everything is */
    if (title_dir(src, d.nand, sizeof(d.nand), &tid)) {
        size_t l = strlen(src);
        while (l && src[l - 1] == '/') l--;
        snprintf(d.tdir, sizeof(d.tdir), "%.*s", (int)l, src);
        if (tid == 0x0000000100000001ull) {
            seterr(err, en, "boot2 isn't kept as files on the NAND, so it can't be backed up as a WAD");
            goto out;
        }
        if (tid == 0x0000000100000002ull) {
            snprintf(path, sizeof(path), "%s/content/title_or.tmd", d.tdir);
            if (exists_file(path)) d.priiloader = true;
        }
        if (!d.priiloader) snprintf(path, sizeof(path), "%s/content/title.tmd", d.tdir);
        tmd = read_all(path, &tmd_sz, FILE_MAX);
        if (!tmd) {
            seterr(err, en, "No TMD in %s/content (not installed, or not readable)", d.tdir);
            goto out;
        }
    } else if (is_content_bin(src)) {
        snprintf(d.cbin, sizeof(d.cbin), "%s|", src);
        snprintf(d.nand, sizeof(d.nand), "nand:");
        snprintf(path, sizeof(path), "%s/", d.cbin);
        vfs_entry *list;
        int cnt;
        if (!vfs_list(path, &list, &cnt)) {
            seterr(err, en, "Can't open the content.bin");
            goto out;
        }
        bool encrypted = false;
        for (int i = 0; i < cnt; i++) {
            if (arc_name_has_ext(list[i].name, ".app.enc")) encrypted = true;
            if (!tmd && arc_name_has_ext(list[i].name, ".tmd")) {
                snprintf(path, sizeof(path), "%s/%s", d.cbin, list[i].name);
                tmd = read_all(path, &tmd_sz, FILE_MAX);
            }
        }
        free(list);
        if (encrypted) {
            seterr(err, en, "This content.bin was made by another Wii: put that Wii's BootMii keys.bin (or otp.bin / "
                   "nand.bin) next to it or at the root of the drive");
            goto out;
        }
        if (!tmd) {
            seterr(err, en, "No TMD in the content.bin");
            goto out;
        }
    } else {
        seterr(err, en, "Not a title folder or a content.bin");
        goto out;
    }

    /* --- the TMD, kept as it is */
    if (tmd_sz < TMD_RECORDS || be32(tmd) != 0x00010001) {
        seterr(err, en, "The TMD isn't valid");
        goto out;
    }
    u16 ncont = be16(tmd + TMD_NCONT);
    u32 tmd_len = TMD_RECORDS + (u32)ncont * REC_LEN;
    if (!ncont || tmd_len > tmd_sz) {
        seterr(err, en, "The TMD isn't valid");
        goto out;
    }
    u64 ttid = be64(tmd + TMD_TID);
    if (tid && ttid != tid) {
        seterr(err, en, "The TMD is for %08x-%08x, not this title", (unsigned)TID_HI(ttid), (unsigned)TID_LO(ttid));
        goto out;
    }
    tid = ttid;
    if (!d.tdir[0])
        snprintf(d.tdir, sizeof(d.tdir), "%s/title/%08x/%08x", d.nand, (unsigned)TID_HI(tid), (unsigned)TID_LO(tid));

    /* --- the ticket, fakesigned */
    snprintf(path, sizeof(path), "%s/ticket/%08x/%08x.tik", d.nand, (unsigned)TID_HI(tid), (unsigned)TID_LO(tid));
    tik = read_all(path, &tik_sz, FILE_MAX);
    if (!tik && d.cbin[0]) {
        /* a content.bin of a title that isn't on this NAND: its ticket from beside it, or a tickets folder */
        char dir[VFS_PATH_MAX], root[16];
        int dev = vfs_dev_of(src);
        if (!vfs_parent(src, dir, sizeof(dir))) dir[0] = 0;
        snprintf(root, sizeof(root), "%s", dev >= 0 ? vfs_dev_root(dev) : "sd:/");
        const char *const fmts[] = { "%s/%08x%08x.tik", "%s/%08x-%08x.tik", "%s/%08x.tik", "%s/ticket.tik",
                                     "%sGodModeWii/tickets/%08x-%08x.tik", "%sYABDM/Tickets/%08x-%08x.tik" };
        for (int i = 0; i < 6 && !tik; i++) {
            const char *base = i < 4 ? dir : root;
            if (!base[0]) continue;
            if (i == 2) snprintf(path, sizeof(path), fmts[i], base, (unsigned)TID_LO(tid));
            else if (i == 3) snprintf(path, sizeof(path), fmts[i], base);
            else snprintf(path, sizeof(path), fmts[i], base, (unsigned)TID_HI(tid), (unsigned)TID_LO(tid));
            tik = read_all(path, &tik_sz, FILE_MAX);
            if (tik && (tik_sz < TIK_LEN || be64(tik + TIK_TID) != tid)) {
                free(tik);
                tik = NULL;
            }
        }
    }
    if (!tik || tik_sz < TIK_LEN || be32(tik) != 0x00010001) {
        if (d.cbin[0])
            seterr(err, en, "No ticket for %08x-%08x: not on this NAND, nor as %08x-%08x.tik next to the content.bin "
                   "or in GodModeWii/tickets/", (unsigned)TID_HI(tid), (unsigned)TID_LO(tid), (unsigned)TID_HI(tid),
                   (unsigned)TID_LO(tid));
        else
            seterr(err, en, "No ticket for %08x-%08x on the NAND", (unsigned)TID_HI(tid), (unsigned)TID_LO(tid));
        goto out;
    }
    if (be64(tik + TIK_TID) != tid) {
        seterr(err, en, "The ticket is for another title");
        goto out;
    }
    if (!fix_ticket(tik, key, err, en)) goto out;
    aes128_init_enc(&enc, key);
    memset(key, 0, sizeof(key));

    /* --- certificates */
    snprintf(path, sizeof(path), "%s/sys/cert.sys", d.nand);
    sys = read_all(path, &sys_sz, FILE_MAX);
    u8 *certs = memalign(32, CERTS_LEN);
    if (!sys || !certs || !make_certs(sys, sys_sz, certs)) {
        free(certs);
        seterr(err, en, "Can't read the certificates (/sys/cert.sys)");
        goto out;
    }

    /* --- every content has to be there, except the pieces of a DLC that were never downloaded */
    snprintf(path, sizeof(path), "%s/shared1/content.map", d.nand);
    d.map = read_all(path, &d.map_n, FILE_MAX);
    d.map_n = d.map ? d.map_n / 28 : 0;
    bool dlc = (tid >> 32) == 0x00010005;
    sel = calloc(ncont, 1);
    if (!sel) {
        free(certs);
        seterr(err, en, "Out of memory");
        goto out;
    }
    int have = 0;
    for (u16 i = 0; i < ncont; i++) {
        const u8 *rec = tmd + TMD_RECORDS + (u32)i * REC_LEN;
        if (locate(&d, rec, path, sizeof(path))) {
            sel[i] = 1;
            have++;
            continue;
        }
        if (!dlc || !(be16(rec + 6) & 0x4000)) {
            seterr(err, en, "Content %08x isn't on the NAND", (unsigned)be32(rec));
            free(certs);
            goto out;
        }
    }
    if (info) {
        info->ncont = ncont;
        info->present = have;
        info->dlc = dlc;
    }
    if (per > 0) {                      /* split: content 0, then the part'th `per` of the others there are */
        if (!dlc || !sel[0]) {
            seterr(err, en, dlc ? "Content 0 of this DLC isn't on the NAND" : "Only a DLC can be split");
            free(certs);
            goto out;
        }
        int nth = 0, kept = 0, from = (part - 1) * per;
        for (u16 i = 1; i < ncont; i++) {
            if (!sel[i]) continue;
            bool in = nth >= from && nth < from + per;
            nth++;
            if (!in) sel[i] = 0;
            else kept++;
        }
        if (!kept) {                    /* past the last part */
            free(certs);
            ret = 1;
            goto out;
        }
    }
    u64 data_len = 0;
    u32 footer_len = 0;
    int nsel = 0;
    char foot[VFS_PATH_MAX + 64] = "";
    for (u16 i = 0; i < ncont; i++) {
        if (!sel[i]) continue;
        const u8 *rec = tmd + TMD_RECORDS + (u32)i * REC_LEN;
        u64 size = be64(rec + 8);
        locate(&d, rec, path, sizeof(path));
        data_len += a64((u32)((size + 15) & ~15ull));
        nsel++;
        if (i == 0 && size < 0x10000000ull) {
            footer_len = (u32)size;
            snprintf(foot, sizeof(foot), "%s", path);
        }
    }
    if (info) info->written = nsel;

    /* --- write it */
    wbuf = memalign(32, CHUNK + 64);
    if (!wbuf || !vfs_open_write(&out, outpath)) {
        free(certs);
        seterr(err, en, "Can't create %s (is the drive writable?)", outpath);
        goto out;
    }
    opened = true;
    vfs_unbuffered(&out);
    writer_t w = { &out, wbuf, progress, ctx, 0, data_len + a64(footer_len), "" };

    u8 hdr[0x40];
    memset(hdr, 0, sizeof(hdr));
    wb32(hdr, 0x20);
    wb16(hdr + 4, 0x4973);              /* "Is" */
    wb32(hdr + 8, CERTS_LEN);
    wb32(hdr + 0x10, TIK_LEN);
    wb32(hdr + 0x14, tmd_len);
    wb32(hdr + 0x18, (u32)data_len);
    wb32(hdr + 0x1C, footer_len);
    bool ok = put(&w, hdr, sizeof(hdr)) && put(&w, certs, CERTS_LEN) && put(&w, tik, TIK_LEN) &&
              zeros(&w, a64(TIK_LEN) - TIK_LEN) && put(&w, tmd, tmd_len) && zeros(&w, a64(tmd_len) - tmd_len);
    free(certs);
    if (!ok) {
        seterr(err, en, "Couldn't write the WAD (is the drive full?)");
        goto out;
    }
    for (u16 i = 0; i < ncont; i++) {
        if (!sel[i]) continue;
        const u8 *rec = tmd + TMD_RECORDS + (u32)i * REC_LEN;
        locate(&d, rec, path, sizeof(path));
        snprintf(w.what, sizeof(w.what), "Content %u of %u (%08x)", i + 1, ncont, (unsigned)be32(rec));
        int r = stream(&w, path, be64(rec + 8), &enc, be16(rec + 4), rec + 0x10);
        if (r == -3) {
            seterr(err, en, "Canceled");
            goto out;
        }
        if (r == -2) {
            seterr(err, en, "Content %08x doesn't match its hash in the TMD (damaged?)", (unsigned)be32(rec));
            goto out;
        }
        if (r) {
            seterr(err, en, "Couldn't copy content %08x (read error, or the drive is full)", (unsigned)be32(rec));
            goto out;
        }
    }
    if (footer_len) {
        snprintf(w.what, sizeof(w.what), "Footer");
        int r = stream(&w, foot, footer_len, NULL, 0, NULL);
        if (r) {
            seterr(err, en, r == -3 ? "Canceled" : "Couldn't write the footer");
            goto out;
        }
    }
    opened = false;
    if (!vfs_close_checked(&out)) {
        seterr(err, en, "Couldn't finish writing the WAD (is the drive full?)");
        vfs_remove(outpath, false);
        goto out;
    }
    ret = 0;

out:
    if (opened) {
        vfs_close(&out);
        vfs_remove(outpath, false);
    }
    memset(&enc, 0, sizeof(enc));
    memset(key, 0, sizeof(key));
    if (tik) memset(tik, 0, tik_sz);
    free(tik);
    free(tmd);
    free(sys);
    free(d.map);
    free(wbuf);
    free(sel);
    return ret;
}

/* ---------------------------------------------------------------- the action */

static void progress_box(const char *name, const char *what, u64 done, u64 total)
{
    ui_splash(name);
    int r = ui_rows() / 2 + 2, cols = ui_cols();
    ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(what)) / 2, "", what);
    int pct = total ? (int)(done * 100 / total) : 0;
    ui_bar(r + 1, (cols - 50) / 2, 50, pct, C_GREEN);
}

static bool ui_progress(void *ctx, const char *what, u64 done, u64 total)
{
    static u64 last;
    if (input_cancel_held()) return false;
    u64 now = ticks_to_millisecs(gettime());
    if (now - last < 200 && done != total) return true;
    last = now;
    char w[96];
    snprintf(w, sizeof(w), "%s...  (hold B to cancel)", what);
    progress_box(ctx, w, done, total);
    return true;
}

static bool mkdirs(const char *dir)
{
    bool is_dir;
    if (vfs_stat(dir, &is_dir, NULL)) return is_dir;
    char parent[VFS_PATH_MAX];
    if (!vfs_parent(dir, parent, sizeof(parent))) return false;
    if (!vfs_is_root(parent) && !mkdirs(parent)) return false;
    return vfs_mkdir(dir) || (vfs_stat(dir, &is_dir, NULL) && is_dir);
}

/* The SD card or a USB drive to write to; -1 when canceled or there is none. */
static int pick_target(void)
{
    const char *items[1 + USB_SLOTS];
    int devs[1 + USB_SLOTS], n = 0;
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (!g_devs[d].mounted || vfs_is_readonly(g_devs[d].root)) continue;
        items[n] = g_devs[d].name;
        devs[n++] = d;
    }
    if (!n) {
        ui_message("BACK UP AS WAD", C_RED, "There is no writable SD card or USB drive to put the WAD on.");
        return -1;
    }
    if (n == 1) return devs[0];
    int r = ui_menu("PUT THE WADS ON", items, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

/* The title ID and version a source will give, for its file name. */
static bool peek(const char *src, u64 *tid, u16 *ver)
{
    char p[VFS_PATH_MAX + 64];
    u8 *tmd = NULL;
    u32 sz = 0;
    if (title_dir(src, NULL, 0, tid)) {
        size_t l = strlen(src);
        while (l && src[l - 1] == '/') l--;
        snprintf(p, sizeof(p), "%.*s/content/title_or.tmd", (int)l, src);
        if (*tid != 0x0000000100000002ull || !(tmd = read_all(p, &sz, FILE_MAX))) {
            snprintf(p, sizeof(p), "%.*s/content/title.tmd", (int)l, src);
            tmd = read_all(p, &sz, FILE_MAX);
        }
    } else {
        snprintf(p, sizeof(p), "%s|/", src);
        vfs_entry *list;
        int cnt;
        if (vfs_list(p, &list, &cnt)) {
            for (int i = 0; i < cnt && !tmd; i++)
                if (arc_name_has_ext(list[i].name, ".tmd")) {
                    snprintf(p, sizeof(p), "%s|/%s", src, list[i].name);
                    tmd = read_all(p, &sz, FILE_MAX);
                }
            free(list);
        }
    }
    bool ok = tmd && sz >= TMD_RECORDS;
    if (ok) {
        *tid = be64(tmd + TMD_TID);
        *ver = be16(tmd + TMD_VERSION);
    }
    free(tmd);
    return ok;
}

int waddump_paths(char **paths, int n)
{
    int srcs = 0;
    for (int i = 0; i < n; i++)
        if (waddump_is_source(paths[i], NULL)) srcs++;
    if (!srcs) {
        ui_message("BACK UP AS WAD", C_YELLOW,
                   "Pick a title folder (nand:/title/<type>/<title>, also inside a BootMii nand.bin) or a "
                   "content.bin from the SD card.");
        return 0;
    }
    char q[640];
    snprintf(q, sizeof(q),
             "Back up %d title%s as installable WAD%s?\n\n"
             "Made the way YABDM makes them: the contents are encrypted again with the title key and checked "
             "against the TMD. The ticket is fakesigned and its console ID removed, so the WAD installs on any "
             "Wii whose IOS has the signing bug; the TMD is kept as Nintendo signed it. The source is only read.\n\n"
             "They go in GodModeWii/wad-dump/ on the drive you choose next.",
             srcs, srcs == 1 ? "" : "s", srcs == 1 ? "" : "s");
    if (!ui_confirm("BACK UP AS WAD", C_CYAN, q)) return 0;

    /* DLC: one WAD, or split into small ones (dlc-wad-splitter) */
    int dlcs = 0, per = 0;
    for (int i = 0; i < n; i++) {
        u64 t = 0;
        if (waddump_is_source(paths[i], &t) && (t >> 32) == 0x00010005) dlcs++;
    }
    if (dlcs) {
        static const int sizes[] = { 0, 1, 5, 10, 20, 50 };
        const char *items[] = { "One WAD (every part that was downloaded)", "Split: 1 content per WAD",
                                "Split: 5 contents per WAD", "Split: 10 contents per WAD", "Split: 20 contents per WAD",
                                "Split: 50 contents per WAD" };
        int r = ui_menu(dlcs == 1 ? "BACK UP THE DLC AS" : "BACK UP THE DLCS AS", items, NULL, 6, 0);
        if (r < 0) return 0;
        per = sizes[r];
    }
    int dev = pick_target();
    if (dev < 0) return 0;
    char dir[VFS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%sGodModeWii/wad-dump", g_devs[dev].root);
    if (!mkdirs(dir)) {
        ui_message("BACK UP AS WAD", C_RED, "Could not create GodModeWii/wad-dump. Is the drive writable?");
        return 0;
    }

    int ok = 0, done = 0;
    char report[1400] = "", last[2 * VFS_PATH_MAX + 320] = "";
    size_t rl = 0;
    for (int i = 0; i < n; i++) {
        if (!waddump_is_source(paths[i], NULL)) continue;
        char name[64] = "", outp[VFS_PATH_MAX + 96], err[200] = "", shown[VFS_PATH_MAX], note[96] = "";
        u64 tid = 0;
        u16 ver = 0;
        vfs_shown_path(paths[i], shown, sizeof(shown));
        int rc = -1;
        if (!peek(paths[i], &tid, &ver)) {
            snprintf(err, sizeof(err), "No readable TMD");
        } else {
            waddump_name(tid, ver, name, sizeof(name));
            snprintf(outp, sizeof(outp), "%s/%s", dir, name);
            progress_box(name, "Reading the ticket and TMD...", 0, 1);
            waddump_info_t inf = { 0, 0, 0, false };
            if (per && (tid >> 32) == 0x00010005) {
                /* <name>_split_1.wad, _2 ... until the contents run out */
                char stem[64];
                snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(name) - 4), name);
                int parts = 0;
                for (int part = 1;; part++) {
                    snprintf(outp, sizeof(outp), "%s/%s_split_%d.wad", dir, stem, part);
                    rc = waddump_build_part(paths[i], outp, part, per, &inf, err, sizeof(err), ui_progress, name);
                    if (rc) break;
                    parts++;
                }
                if (rc == 1) rc = parts ? 0 : -1;
                if (!rc) {
                    snprintf(outp, sizeof(outp), "%s/%s_split_1.wad ... _%d.wad", dir, stem, parts);
                    snprintf(note, sizeof(note), "%d WADs, %d of %d contents", parts, inf.present, inf.ncont);
                } else if (!err[0])
                    snprintf(err, sizeof(err), "Nothing to split: only content 0 is on the NAND");
            } else {
                rc = waddump_build_part(paths[i], outp, 0, 0, &inf, err, sizeof(err), ui_progress, name);
                if (!rc && inf.present < inf.ncont)
                    snprintf(note, sizeof(note), "%d of %d contents, the rest were never downloaded", inf.present,
                             inf.ncont);
            }
        }
        done++;
        if (!rc) ok++;
        if (rc)
            snprintf(last, sizeof(last), "%s: %s", shown, err);
        else
            snprintf(last, sizeof(last), "%s\n-> %s%s%s", shown, outp, note[0] ? "\n\n" : "", note);
        if (rl < sizeof(report) - 1)
            rl += snprintf(report + rl, sizeof(report) - rl, "%s %s: %s\n", rc ? "FAIL" : "OK  ",
                           name[0] ? name : vfs_basename(paths[i]), rc ? err : note[0] ? note : "done");
        if (rl >= sizeof(report)) rl = sizeof(report) - 1;
    }
    vfs_update_space(dev);
    if (done == 1) {
        ui_message(ok ? "WAD MADE" : "BACK UP AS WAD FAILED", ok ? C_GREEN : C_RED, last);
    } else {
        char title[48];
        snprintf(title, sizeof(title), "MADE %d OF %d WADS", ok, done);
        ui_message(title, ok == done ? C_GREEN : C_YELLOW, report);
    }
    return ok;
}

#ifdef GMW_AUTOTEST
/* Test builds: "@waddump <source> <out.wad>", logged to sd:/gmwtest.log. */
void waddump_autotest(const char *src, const char *outp)
{
    char err[200] = "";
    int rc = waddump_build(src, outp, err, sizeof(err), NULL, NULL);
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "waddump %s -> %s: %s %s\n", src, outp, rc ? "FAILED" : "ok", err);
        fclose(lg);
    }
}

void waddump_part_autotest(const char *src, const char *outp, int part, int per)
{
    char err[200] = "";
    waddump_info_t inf = { 0, 0, 0, false };
    int rc = waddump_build_part(src, outp, part, per, &inf, err, sizeof(err), NULL, NULL);
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "waddumppart %s -> %s part %d/%d: rc=%d %d of %d present, %d written %s\n", src, outp, part, per, rc,
                inf.present, inf.ncont, inf.written, err);
        fclose(lg);
    }
}
#endif

/*
 * WAD installer. See wadinstall.h.
 *
 * The install sequence (ticket, TMD, contents through ES), the ticket fix-up
 * for vWii-key WADs and the brick checks are ported from YAWM ModMii Edition
 * (https://github.com/modmii/YAWM-ModMii-Edition, GPL-2.0) by OverjoY,
 * blackb0x, scooby74029, Peter0x44 and XFlak, itself based on Some YAWMM Mod,
 * YAWMM and WAD Manager (Waninkoko).
 *
 * Differences that keep save data safe:
 *   - there is no uninstall: ES_DeleteTitle (which wipes /title/../data) is
 *     never called
 *   - before a title that is already on the NAND is installed over, its data
 *     folder is copied to <SD or USB>:/GodModeWii/save-backups/
 *   - after the install the data folder is counted again and a difference is
 *     reported, with the backup's location
 *   - the System Menu is not installed here (that needs YAWM ModMii
 *     Edition's Priiloader handling), nor is boot2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <time.h>
#include <gccore.h>
#include <ogc/es.h>
#include <ogc/isfs.h>
#include <ogc/lwp_watchdog.h>

#include "wadinstall.h"
#include "vfs.h"
#include "ui.h"
#include "ops.h"
#include "risk.h"
#include "aes.h"
#include "sha1.h"
#include "seeprom.h"
#include "archive.h"
#include "ftype.h"
#include "runtimeiospatch.h"
#include "wad2bin.h"
#include "otp.h"
#include "input.h"

#define TID(hi, lo)   (((u64)(hi) << 32) | (u32)(lo))
#define TID_HI(t)     ((u32)((t) >> 32))
#define TID_LO(t)     ((u32)(t))

#define CHUNK 0x4000                    /* ES content data block, as in YAWM */
#define META_MAX (1024 * 1024)

/* Offsets inside a signed ticket / TMD with an RSA-2048 signature. */
#define W_TIK_KEY      0x1BF
#define W_TIK_TID      0x1DC
#define W_TIK_CKIDX    0x1F1
#define W_TIK_PADDING  0x262
#define W_TIK_BODY     0x140
#define W_TIK_BODY_LEN 0x164
#define W_TMD_VWII     0x183
#define W_TMD_SYSVER   0x184
#define W_TMD_TID      0x18C
#define W_TMD_VERSION  0x1DC
#define W_TMD_NCONT    0x1DE
#define W_TMD_CONTENTS 0x1E4

/* ---------------------------------------------------------------- helpers */

static const char *wad_strerror(int ec)
{
    switch (ec) {
    case 0:     return "Success";
    case -106:  return "Not found";
    case -996:  return "Read error";
    case -999:  return "Blocked to protect the console";
    case -1010: return "Wii system memory full";
    case -1017: return "Invalid argument";
    case -1022: return "Content hash mismatch";
    case -1035: return "Newer version already installed";
    case -1036: return "Needed IOS missing";
    case -2011: return "Signature check failed (no trucha patch? launch from the Homebrew Channel)";
    case ES_EINVAL: return "Invalid WAD";
    default:    return "Unknown error";
    }
}

static bool read_at(vfs_file *f, u64 off, void *buf, u32 len)
{
    if (!vfs_seek(f, off))
        return false;
    u8 *p = buf;
    while (len) {
        s32 r = vfs_read(f, p, len);
        if (r <= 0)
            return false;
        p += r;
        len -= r;
    }
    return true;
}

static void *read_blob(vfs_file *f, u64 off, u32 len)
{
    if (!len || len > META_MAX)
        return NULL;
    void *b = memalign(32, (len + 31) & ~31u);
    if (b && !read_at(f, off, b, len)) {
        free(b);
        b = NULL;
    }
    return b;
}

static u32 round64(u32 v) { return (v + 63) & ~63u; }

/* Signed TMD of an installed title, or NULL. */
static u8 *stored_tmd(u64 tid, u32 *len)
{
    u32 sz = 0;
    if (ES_GetStoredTMDSize(tid, &sz) < 0 || !sz || sz > META_MAX)
        return NULL;
    u8 *t = memalign(32, (sz + 31) & ~31u);
    if (t && ES_GetStoredTMD(tid, (signed_blob *)t, sz) < 0) {
        free(t);
        return NULL;
    }
    if (len) *len = sz;
    return t;
}

static bool title_installed(u64 tid)
{
    u32 sz = 0;
    return ES_GetStoredTMDSize(tid, &sz) >= 0 && sz;
}

/* The IOS an installed title runs on (0 if it isn't installed). */
static u64 title_ios(u64 tid)
{
    u32 len;
    u8 *t = stored_tmd(tid, &len);
    if (!t)
        return 0;
    u64 ios = be64(t + W_TMD_SYSVER);
    free(t);
    return ios;
}

static bool tmd_is_stub_ios(const u8 *tmd)
{
    const u8 *c = tmd + W_TMD_CONTENTS;
    return be64(tmd + W_TMD_SYSVER) >> 32 == 0 && be16(tmd + W_TMD_NCONT) == 3 &&
           be16(c + 0x06) == 0x0001 && be16(c + 0x24 + 0x06) == 0x8001 &&
           be16(c + 0x48 + 0x06) == 0x8001;
}

/* YAWM's stub detection for an installed IOS. */
static bool ios_is_stub(u8 ios)
{
    u32 boot2 = 0;
    ES_GetBoot2Version(&boot2);
    if (boot2 >= 5 && (ios == 202 || ios == 222 || ios == 223 || ios == 224))
        return true;
    u32 len;
    u8 *t = stored_tmd(TID(1, ios), &len);
    if (!t)
        return true;                    /* missing counts as unusable */
    u8 low = (u8)be16(t + W_TMD_VERSION);
    bool stub = false;
    if (boot2 >= 5 && (ios == 249 || ios == 250) && low < 18)
        stub = true;
    else if ((ios == 202 || ios == 222 || ios == 223 || ios == 224) && low < 4)
        stub = true;
    else if (low == 0)
        stub = tmd_is_stub_ios(t);
    free(t);
    return stub;
}

/* Korean common key in the SEEPROM (offset 0x74): checked as in YAWM. */
static bool has_korean_key(void)
{
    u8 *ee = memalign(32, SEEPROM_SIZE);
    if (!ee || !seeprom_read(ee)) {
        free(ee);
        return false;
    }
    u8 data[16] = { 0x56, 0x52, 0x6f, 0x63, 0xa1, 0x2c, 0xd1, 0x32,
                    0x07, 0x99, 0x82, 0x3b, 0x1b, 0x08, 0x17, 0xd0 };
    u8 iv[16] = { 0 };
    aes128_t c;
    aes128_init_dec(&c, ee + 0x74);
    aes128_cbc_decrypt(&c, iv, data, data, 16);
    memset(&c, 0, sizeof(c));
    free(ee);
    return !memcmp(data, "thepikachugamer", 16);
}

/* Fakesigning: zero the RSA-2048 signature, then find a padding value that
 * makes the SHA-1 of the ticket body start with 0 (the trucha bug). */
static bool fakesign_ticket(u8 *tik)
{
    memset(tik + 4, 0, 0x100);
    for (u32 fill = 0; fill < 0x10000; fill++) {
        tik[W_TIK_PADDING] = (u8)(fill >> 8);
        tik[W_TIK_PADDING + 1] = (u8)fill;
        sha1_t s;
        u8 h[20];
        sha1_init(&s);
        sha1_update(&s, tik + W_TIK_BODY, W_TIK_BODY_LEN);
        sha1_final(&s, h);
        if (!h[0])
            return true;
    }
    return false;
}

/* YAWM's __Wad_FixTicket: a ticket for common key 2 (vWii) is re-encrypted
 * with the Wii common key and fakesigned. Returns true for a vWii title. */
static bool fix_ticket(u8 *tik)
{
    u8 idx = tik[W_TIK_CKIDX];
    if (idx <= 1)
        return false;
    bool vwii = idx == 2;
    tik[W_TIK_CKIDX] = 0;
    if (vwii) {
        u8 vk[16], wk[16], iv[16], key[16];
        if (arc_common_key(2, vk) && arc_common_key(0, wk)) {
            memset(iv, 0, 16);
            memcpy(iv, tik + W_TIK_TID, 8);
            aes128_t c;
            aes128_init_dec(&c, vk);
            aes128_cbc_decrypt(&c, iv, tik + W_TIK_KEY, key, 16);
            for (int i = 0; i < 16; i++)            /* CBC encrypt of a single block */
                key[i] ^= iv[i];
            aes128_ecb_encrypt(wk, key, tik + W_TIK_KEY);
            memset(&c, 0, sizeof(c));
            memset(key, 0, 16);
            memset(vk, 0, 16);
            memset(wk, 0, 16);
        }
    }
    fakesign_ticket(tik);
    return vwii;
}

/* The cIOS build tag at the start of content index 0, decrypted with the
 * title key. True when it's a cIOS based on IOS60 (safe with the Korean key). */
static bool cios_base60(vfs_file *f, const u8 *tik, const u8 *tmd, u64 data_off)
{
    u8 ck[16], tk[16], iv[16];
    int idx = tik[W_TIK_CKIDX];
    if (!arc_common_key(idx, ck))
        return false;
    memset(iv, 0, 16);
    memcpy(iv, tik + W_TIK_TID, 8);
    aes128_t c;
    aes128_init_dec(&c, ck);
    aes128_cbc_decrypt(&c, iv, tik + W_TIK_KEY, tk, 16);
    aes128_init_dec(&c, tk);

    u64 off = data_off;
    int n = be16(tmd + W_TMD_NCONT);
    for (int i = 0; i < n; i++) {
        const u8 *r = tmd + W_TMD_CONTENTS + i * 0x24;
        if (be16(r + 4) == 0) break;
        off += round64((u32)be64(r + 8));
    }
    u8 *tag = memalign(32, 0x40);
    bool ok = false;
    if (tag && read_at(f, off, tag, 0x40)) {
        memset(iv, 0, 16);
        aes128_cbc_decrypt(&c, iv, tag, tag, 0x40);
        ok = be32(tag) == 0x1EE7C105 && be32(tag + 4) == 1 && be32(tag + 12) == 60;
    }
    free(tag);
    memset(&c, 0, sizeof(c));
    memset(tk, 0, 16);
    memset(ck, 0, 16);
    return ok;
}

/* /shared1/content.map: 8-char name + SHA-1 per shared content. */
static u8 *load_content_map(u32 *count)
{
    *count = 0;
    s32 fd = ISFS_Open("/shared1/content.map", ISFS_OPEN_READ);
    if (fd < 0)
        return NULL;
    static fstats st ATTRIBUTE_ALIGN(32);
    u8 *buf = NULL;
    if (ISFS_GetFileStats(fd, &st) >= 0 && st.file_length && st.file_length % 28 == 0 &&
        st.file_length < META_MAX) {
        buf = memalign(32, (st.file_length + 31) & ~31u);
        if (buf && ISFS_Read(fd, buf, st.file_length) != (s32)st.file_length) {
            free(buf);
            buf = NULL;
        }
        if (buf) *count = st.file_length / 28;
    }
    ISFS_Close(fd);
    return buf;
}

static bool shared_present(const u8 *rec, const u8 *map, u32 count)
{
    if (!map || !(be16(rec + 6) & 0x8000))
        return false;
    for (u32 i = 0; i < count; i++)
        if (!memcmp(map + i * 28 + 8, rec + 0x10, 20))
            return true;
    return false;
}

/* ---------------------------------------------------------------- save data */

static void count_tree(const char *path, u32 *files, u64 *bytes)
{
    u32 d = 0;
    *files = 0;
    *bytes = 0;
    ops_tree_info(path, bytes, files, &d);
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

static int backup_device(void)
{
    if (g_devs[DEV_SD].mounted && !vfs_is_readonly("sd:/"))
        return DEV_SD;
    for (int d = DEV_USB0; d <= DEV_USB7; d++)
        if (g_devs[d].mounted && !vfs_is_readonly(g_devs[d].root))
            return d;
    return -1;
}

/* Copies nand:/title/hi/lo/data to <drive>:/GodModeWii/save-backups/<tid>-<time>/data.
 * Returns 1 when backed up, 0 when there is nothing to back up, -1 on failure. */
static int backup_save(u64 tid, char *where, size_t wsz, u32 *files_before)
{
    char data[96];
    snprintf(data, sizeof(data), "nand:/title/%08x/%08x/data", TID_HI(tid), TID_LO(tid));
    *files_before = 0;
    bool is_dir;
    if (!vfs_stat(data, &is_dir, NULL) || !is_dir)
        return 0;
    u64 bytes;
    count_tree(data, files_before, &bytes);
    if (!*files_before)
        return 0;

    int dev = backup_device();
    if (dev < 0)
        return -1;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(where, wsz, "%sGodModeWii/save-backups/%08x-%08x-%04d%02d%02d-%02d%02d%02d",
             g_devs[dev].root, TID_HI(tid), TID_LO(tid), tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (!mkdirs(where))
        return -1;
    char *src = data;
    if (!ops_paste(&src, 1, false, where))
        return -1;
    char copy[VFS_PATH_MAX];
    snprintf(copy, sizeof(copy), "%s/data", where);
    u32 got;
    count_tree(copy, &got, &bytes);
    return got == *files_before ? 1 : -1;
}

/* ---------------------------------------------------------------- install */

typedef struct {
    u32 hdr_len, certs_len, crl_len, tik_len, tmd_len, data_len;
} wadhdr;

static bool patched;

static void progress(const char *name, const char *what, u64 done, u64 total)
{
    ui_splash(name);
    int r = ui_rows() / 2 + 2, cols = ui_cols();
    ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(what)) / 2, "", what);
    int pct = total ? (int)(done * 100 / total) : 0;
    ui_bar(r + 1, (cols - 50) / 2, 50, pct, C_GREEN);
}

/* One WAD. Returns 0 on success or an ES / YAWM style error code; `msg` gets
 * a human-readable explanation for refusals. */
/* sd_mode: only the ticket and the TMD (the contents are in a content.bin on the SD card), the way a
 * channel moved to the SD card is left on the NAND. */
static int install_one(const char *path, char *msg, size_t msz, char *note, size_t nsz, bool sd_mode)
{
    const char *name = vfs_basename(path);
    msg[0] = note[0] = '\0';
    vfs_file f;
    if (!vfs_open_read(&f, path)) {
        snprintf(msg, msz, "Can't open the file.");
        return -996;
    }
    vfs_unbuffered(&f);

    int ret = -996;
    u8 *hb = NULL, *certs = NULL, *crl = NULL, *tik = NULL, *tmd = NULL, *buf = NULL, *map = NULL;
    bool started = false;
    progress(name, "Reading WAD...", 0, 1);

    hb = read_blob(&f, 0, 0x20);
    if (!hb) goto out;
    wadhdr h = { be32(hb),        be32(hb + 8),    be32(hb + 0xC),
                 be32(hb + 0x10), be32(hb + 0x14), be32(hb + 0x18) };
    if (h.hdr_len != 0x20 || be16(hb + 4) != 0x4973 || be16(hb + 6) != 0) {
        snprintf(msg, msz,
                 "This isn't an installable WAD (only \"Is\" type WADs are; boot2 \"ib\" WADs are not).");
        ret = ES_EINVAL;
        goto out;
    }

    u64 off = round64(h.hdr_len);
    certs = read_blob(&f, off, h.certs_len);
    off += round64(h.certs_len);
    if (h.crl_len) {
        crl = read_blob(&f, off, h.crl_len);
        off += round64(h.crl_len);
    }
    tik = read_blob(&f, off, h.tik_len);
    off += round64(h.tik_len);
    tmd = read_blob(&f, off, h.tmd_len);
    off += round64(h.tmd_len);

    u64 data_off = off;
    if (!certs || !tik || !tmd || (h.crl_len && !crl) || h.tik_len < 0x2A4 ||
        h.tmd_len < W_TMD_CONTENTS || be32(tik) != 0x00010001 || be32(tmd) != 0x00010001) {
        snprintf(msg, msz, "The WAD is damaged or uses an unsupported signature type.");
        ret = ES_EINVAL;
        goto out;
    }

    int ncont = be16(tmd + W_TMD_NCONT);
    if (W_TMD_CONTENTS + (u32)ncont * 0x24 > h.tmd_len) {
        ret = ES_EINVAL;
        goto out;
    }

    u64 tid = be64(tik + W_TIK_TID);
    u64 tmd_tid = be64(tmd + W_TMD_TID);
    u64 sysver = be64(tmd + W_TMD_SYSVER);
    u16 version = be16(tmd + W_TMD_VERSION);
    bool vwii_flag = tmd[W_TMD_VWII] != 0;
    if (tid != tmd_tid) {
        snprintf(msg, msz, "The ticket and the TMD are for different titles.");
        ret = ES_EINVAL;
        goto out;
    }

    /* ---- brick checks (YAWM ModMii Edition) ---- */
    if (sd_mode && TID_HI(tid) == 1) {
        snprintf(msg, msz, "System titles (IOS, System Menu, BC, MIOS) can't run from the SD card.");
        ret = -999;
        goto out;
    }
    if (tid == TID(1, 1)) {
        snprintf(msg, msz, "This is boot2. GodModeWii won't install it.");
        ret = -999;
        goto out;
    }
    if (tid == TID(1, 2)) {
        snprintf(msg, msz,
                 "This is a System Menu WAD. Install it with YAWM ModMii Edition, which checks the region and keeps Priiloader.");
        ret = -999;
        goto out;
    }

    bool vwii_title = fix_ticket(tik) || vwii_flag;
    if (sysver >> 32 == 0) {                                /* an IOS */
        if (vwii_title != g_is_vwii) {
            snprintf(msg, msz,
                     g_is_vwii ?
                         "This is a Wii IOS. Installing it on vWii could brick the console." :
                         "This is a vWii IOS. Installing it on a Wii could brick the console.");
            ret = -999;
            goto out;
        }

        u64 sm_ios = title_ios(TID(1, 2));
        if (tid == sm_ios) {
            if (tmd_is_stub_ios(tmd)) {
                snprintf(msg, msz, "This would stub the System Menu's IOS and brick the console.");
                ret = -999;
                goto out;
            }
            if (!g_is_vwii && (tid == TID(1, 70) || tid == TID(1, 80)) &&
                !cios_base60(&f, tik, tmd, data_off) && has_korean_key()) {
                snprintf(msg, msz,
                         "Installing this System Menu IOS will brick a Wii with the Korean key. Remove it with KoreanKii first.");
                ret = -999;
                goto out;
            }
        }

        static const u32 eula_rgn[2] = { 0x48414B00, 0x48414C00 };
        static const char regions[4] = { 'E', 'P', 'J', 'K' };
        for (int k = 0; k < 2; k++)
            for (int r = 0; r < 4; r++)
                if (tid == title_ios(TID(0x10008, eula_rgn[k] | (u8)regions[r])) &&
                    tmd_is_stub_ios(tmd)) {
                    snprintf(msg, msz, "This would stub the IOS the %s needs.",
                             k ? "region select" : "EULA");
                    ret = -999;
                    goto out;
                }

        static const u32 hbc[5] = { 0x48415858, 0x4A4F4449, 0xAF1BF516, 0x4C554C5A, 0x4F484243 };
        for (int k = 0; k < 5; k++)
            if (tid == title_ios(TID(0x10001, hbc[k])) && tmd_is_stub_ios(tmd)) {
                snprintf(msg, msz, "This would stub the Homebrew Channel's IOS.");
                ret = -999;
                goto out;
            }
    } else if (TID_HI(sysver) == 1 && TID_LO(sysver) < 256 && ios_is_stub((u8)TID_LO(sysver))) {
        snprintf(msg, msz,
                 "This title needs IOS%u, which is a stub or missing. Install IOS%u first.",
                 (unsigned)TID_LO(sysver), (unsigned)TID_LO(sysver));
        ret = -1036;
        goto out;
    }

    /* ---- protect the save data ---- */
    char backup[VFS_PATH_MAX] = "";
    u32 files_before = 0;
    bool had_title = title_installed(tid);
    if (had_title && TID_HI(tid) != 1 && !sd_mode) {
        int b = backup_save(tid, backup, sizeof(backup), &files_before);
        if (b < 0) {
            char q[300];
            snprintf(q, sizeof(q),
                     "%s\n\nThe save data of this title could not be backed up%s. Installing never deletes "
                     "save data, but without a backup there is nothing to fall back on.\n\nInstall anyway?",
                     name, backup_device() < 0 ? " (no writable SD card or USB drive)" : "");
            if (!ui_confirm("NO SAVE BACKUP", C_YELLOW, q)) {
                snprintf(msg, msz, "Canceled: the save data could not be backed up.");
                ret = -998;
                goto out;
            }
            backup[0] = '\0';
        }
    }

    /* ---- install ---- */
    if (!patched && AHBPROT_DISABLED && !g_is_dolphin) {
        progress(name, "Patching IOS...", 0, 1);
        IosPatch_WADInstall(g_is_vwii, false);
        patched = true;
    }

    u64 total = 0, done = 0;
    for (int i = 0; i < ncont; i++)
        total += round64((u32)be64(tmd + W_TMD_CONTENTS + i * 0x24 + 8));

    progress(name, "Installing ticket...", 0, total);
    ret = ES_AddTicket((signed_blob *)tik, h.tik_len, (signed_blob *)certs, h.certs_len,
                       (signed_blob *)crl, h.crl_len);
    if (ret < 0) goto out;
    if (sd_mode) {
        /* the TMD, with no contents: a title on the SD card. A title already on the NAND keeps
         * what it has. */
        if (had_title) {
            snprintf(msg, msz, "Ticket installed; %08x-%08x is also installed on the NAND, which is left as it is.",
                     TID_HI(tid), TID_LO(tid));
        } else {
            progress(name, "Installing the TMD...", 0, 1);
            s32 r2 = ES_AddTitleStart((signed_blob *)tmd, h.tmd_len, (signed_blob *)certs, h.certs_len,
                                      (signed_blob *)crl, h.crl_len);
            if (r2 >= 0) r2 = ES_AddTitleFinish();
            if (r2 < 0) {
                ES_AddTitleCancel();
                snprintf(msg, msz, "Ticket installed. The TMD wasn't kept (%d); the System Menu takes it from content.bin.",
                         (int)r2);
            } else
                snprintf(msg, msz, "Ticket and TMD installed.");
        }
        ret = 0;
        goto out;
    }
    progress(name, "Installing title...", 0, total);
    ret = ES_AddTitleStart((signed_blob *)tmd, h.tmd_len, (signed_blob *)certs, h.certs_len,
                           (signed_blob *)crl, h.crl_len);
    if (ret < 0) goto out;
    started = true;

    u32 map_n = 0;
    map = load_content_map(&map_n);
    buf = memalign(32, CHUNK);
    if (!buf) {
        ret = -996;
        goto out;
    }

    u64 pos = data_off;
    for (int i = 0; i < ncont; i++) {
        const u8 *rec = tmd + W_TMD_CONTENTS + i * 0x24;
        u32 cid = be32(rec);
        u32 len = round64((u32)be64(rec + 8));
        if (shared_present(rec, map, map_n)) {
            pos += len;
            done += len;
            continue;
        }

        char what[64];
        snprintf(what, sizeof(what), "Installing content %d of %d (%08x)...", i + 1, ncont,
                 (unsigned)cid);
        progress(name, what, done, total);
        s32 cfd = ES_AddContentStart(tid, cid);
        if (cfd < 0) {
            ret = cfd;
            goto out;
        }
        for (u32 idx = 0; idx < len;) {
            u32 n = len - idx > CHUNK ? CHUNK : len - idx;
            if (!read_at(&f, pos, buf, n)) {
                ES_AddContentFinish(cfd);
                ret = -996;
                goto out;
            }
            ret = ES_AddContentData(cfd, buf, n);
            if (ret < 0) {
                ES_AddContentFinish(cfd);
                goto out;
            }
            idx += n;
            pos += n;
            done += n;
            if ((idx & 0x3FFFF) == 0)
                progress(name, what, done, total);
        }
        ret = ES_AddContentFinish(cfd);
        if (ret < 0) goto out;
    }

    progress(name, "Finishing installation...", total, total);
    ret = ES_AddTitleFinish();
    if (ret < 0) goto out;
    started = false;
    ret = 0;

    /* ---- check the save data is still there ---- */
    if (files_before) {
        char data[96];
        u32 after;
        u64 bytes;
        snprintf(data, sizeof(data), "nand:/title/%08x/%08x/data", TID_HI(tid), TID_LO(tid));
        count_tree(data, &after, &bytes);
        if (after != files_before)
            snprintf(note, nsz, "Warning: the save data had %u file(s) before and %u now.%s%s",
                     (unsigned)files_before, (unsigned)after, backup[0] ? " The backup is in " : "",
                     backup);
        else if (backup[0])
            snprintf(note, nsz, "Save data kept (%u file(s)). A backup is in %s",
                     (unsigned)files_before, backup);
    }
    snprintf(msg, msz, "Installed %08x-%08x v%u.", TID_HI(tid), TID_LO(tid), (unsigned)version);

out:
    if (started)
        ES_AddTitleCancel();
    if (ret < 0 && !msg[0])
        snprintf(msg, msz, "%s (%d).", wad_strerror(ret), ret);
    free(hb);
    free(certs);
    free(crl);
    free(tik);
    free(tmd);
    free(buf);
    free(map);
    vfs_close(&f);
    return ret;
}

int wad_install_paths(char **paths, int n)
{
    int wads = 0;
    for (int i = 0; i < n; i++)
        if (ftype_detect(paths[i]) == FT_WAD)
            wads++;
    if (!wads) {
        ui_message("INSTALL WAD", C_YELLOW, "Select one or more .wad files to install.");
        return 0;
    }
    char q[400];
    if (wads == 1) {
        const char *one = NULL;
        for (int i = 0; i < n && !one; i++)
            if (ftype_detect(paths[i]) == FT_WAD) one = paths[i];
        snprintf(q, sizeof(q),
                 "Install \"%s\" to the NAND?\n\nSave data is kept, and backed up to the SD card first when "
                 "the title is already installed. Don't turn the console off while installing.",
                 vfs_basename(one));
    } else {
        snprintf(q, sizeof(q),
                 "Install %d WADs to the NAND?\n\nSave data is kept, and backed up to the SD card first when "
                 "a title is already installed. Don't turn the console off while installing.",
                 wads);
    }
    if (!ui_confirm("INSTALL WAD", C_YELLOW, q))
        return 0;
    if (!risk_ensure_unlocked())
        return 0;

    int ok = 0, done = 0;
    char report[1200] = "";
    size_t rl = 0;
    for (int i = 0; i < n; i++) {
        if (ftype_detect(paths[i]) != FT_WAD)
            continue;
        char msg[200], note[300];
        int r = install_one(paths[i], msg, sizeof(msg), note, sizeof(note), false);
        done++;
        if (r == 0) ok++;
        if (wads == 1) {
            char text[600];
            snprintf(text, sizeof(text), "%s\n\n%s%s%s", vfs_basename(paths[i]), msg,
                     note[0] ? "\n\n" : "", note);
            ui_message(r == 0 ? "INSTALLED" : "INSTALL FAILED",
                       r == 0 ? (note[0] == 'W' ? C_YELLOW : C_GREEN) : C_RED, text);
            return ok;
        }
        if (rl < sizeof(report) - 1)
            rl += snprintf(report + rl, sizeof(report) - rl, "%s %s: %s%s%s\n",
                           r == 0 ? "OK  " : "FAIL", vfs_basename(paths[i]), msg,
                           note[0] == 'W' ? " " : "", note[0] == 'W' ? note : "");
        if (rl >= sizeof(report)) rl = sizeof(report) - 1;
    }
    char title[48];
    snprintf(title, sizeof(title), "INSTALLED %d OF %d", ok, done);
    ui_message(title, ok == done ? C_GREEN : C_YELLOW, report);
    return ok;
}

/* ---------------------------------------------------------------- to the SD card (wad2bin) */

bool wad_console_keys(w2b_keys_t *k, char *why, size_t wn)
{
    memset(k, 0, sizeof(*k));
    u8 otp[OTP_SIZE] ATTRIBUTE_ALIGN(32);
    if (!AHBPROT_DISABLED || !otp_read_all(otp)) {
        snprintf(why, wn, "Can't read this console's keys: start GodModeWii from the Homebrew Channel (HW_AHBPROT off).");
        return false;
    }
    const otp_t *o = (const otp_t *)otp;
    k->console_id = be32(o->ng_id);
    memcpy(k->ng_priv, o->ng_priv, sizeof(k->ng_priv));
    memcpy(k->prng_key, o->rng_key, sizeof(k->prng_key));
    memset(otp, 0, sizeof(otp));
    u8 *cert = memalign(32, 0x200);
    bool ok = cert && ES_GetDeviceCert(cert) >= 0;
    if (ok) memcpy(k->device_cert, cert, sizeof(k->device_cert));
    free(cert);
    if (!ok) {
        memset(k, 0, sizeof(*k));
        snprintf(why, wn, "Can't read this console's device certificate.");
    }
    return ok;
}

/* the WAD, read through the vfs (which wants 32-byte aligned buffers) */
static int32_t vsrc_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    vfs_file *f = ctx;
    static u8 *bounce;
    if (!bounce && !(bounce = memalign(32, CHUNK))) return -1;
    uint32_t done = 0;
    if (!vfs_seek(f, off)) return -1;
    while (done < len) {
        uint32_t n = len - done > CHUNK ? CHUNK : len - done;
        s32 r = vfs_read(f, bounce, n);
        if (r <= 0) break;
        memcpy((u8 *)buf + done, bounce, (size_t)r);
        done += (uint32_t)r;
        if ((uint32_t)r < n) break;
    }
    return (int32_t)done;
}
static void vsrc_close(void *ctx) { (void)ctx; }

static bool vio_mkdirs(void *ctx, const char *dir) { (void)ctx; return mkdirs(dir); }
static void *vio_open(void *ctx, const char *path)
{
    (void)ctx;
    vfs_file *f = calloc(1, sizeof(*f));
    if (f && !vfs_open_write(f, path)) {
        free(f);
        f = NULL;
    }
    if (f) vfs_unbuffered(f);
    return f;
}
static bool vio_write(void *ctx, void *fh, const void *p, uint32_t n)
{
    static u8 *wb;
    if (!wb && !(wb = memalign(32, CHUNK))) return false;
    (void)ctx;
    for (uint32_t done = 0; done < n;) {
        uint32_t k = n - done > CHUNK ? CHUNK : n - done;
        memcpy(wb, (const u8 *)p + done, k);
        if (vfs_write(fh, wb, k) != (s32)k) return false;
        done += k;
    }
    return true;
}
static bool vio_close(void *ctx, void *fh)
{
    (void)ctx;
    bool ok = vfs_close_checked(fh);
    free(fh);
    return ok;
}
static void vio_remove(void *ctx, const char *path) { (void)ctx; vfs_remove(path, false); }
static bool vio_progress(void *ctx, const char *what, uint64_t done, uint64_t total)
{
    static u64 last;
    const char *name = ctx;
    if (input_cancel_held()) return false;
    u64 now = ticks_to_millisecs(gettime());
    if (now - last < 200 && done != total) return true;
    last = now;
    char w[80];
    snprintf(w, sizeof(w), "%s for the SD card...  (hold B to cancel)", what);
    progress(name, w, done, total);
    return true;
}

int wad_install_sd(char **paths, int n)
{
    int wads = 0, dlcs = 0;
    for (int i = 0; i < n; i++)
        if (ftype_detect(paths[i]) == FT_WAD) {
            wads++;
            vfs_file f;
            if (vfs_open_read(&f, paths[i])) {
                src_t s = { vsrc_read, vsrc_close, &f, f.size };
                uint64_t tid;
                if (w2b_peek(&s, &tid) && (tid >> 32) == 0x00010005) dlcs++;
                vfs_close(&f);
            }
        }
    if (!wads) {
        ui_message("INSTALL TO SD", C_YELLOW, "Select one or more .wad files to install.");
        return 0;
    }
    if (!g_devs[DEV_SD].mounted || vfs_is_readonly(vfs_dev_root(DEV_SD))) {
        ui_message("INSTALL TO SD", C_RED, "Insert a writable SD card: titles on the SD card only run from the SD card.");
        return 0;
    }
    char why[160];
    w2b_keys_t keys;
    if (!wad_console_keys(&keys, why, sizeof(why))) {
        ui_message("INSTALL TO SD", C_RED, why);
        return 0;
    }

    char q[700];
    snprintf(q, sizeof(q),
             "Install %s%s%s to the SD card?\n\n"
             "Converted the way wad2bin does it, with this console's keys: a channel becomes "
             "private/wii/title/<ID>/content.bin and runs from the SD card menu (System Menu 4.0 or later); "
             "DLC becomes private/wii/data/<ID>/<index>.bin. The files only work on this Wii.\n\n"
             "Its ticket and TMD are then put on the NAND (asks for the Konami code). A WAD whose ticket "
             "isn't for this console needs a System Menu IOS with the signing bug (or a cIOS for DLC).",
             wads == 1 ? "\"" : "", wads == 1 ? vfs_basename(paths[0]) : "these WADs", wads == 1 ? "\"" : "");
    if (wads > 1) {
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d WADs", wads);
        char *p = strstr(q, "these WADs");
        if (p) {
            char rest[700];
            snprintf(rest, sizeof(rest), "%s", p + strlen("these WADs"));
            snprintf(p, sizeof(q) - (size_t)(p - q), "%s%s", cnt, rest);
        }
    }
    if (!ui_confirm("INSTALL TO SD", C_YELLOW, q)) {
        memset(&keys, 0, sizeof(keys));
        return 0;
    }
    w2b_opts_t opt = { false, 0 };
    if (dlcs) {
        const char *items[] = { "This console's key (newer games, or any game through a cIOS)",
                                "Null key (older games through the Disc Channel, like Rock Band 2)" };
        int r = ui_menu("ENCRYPT THE DLC WITH", items, NULL, 2, 0);
        if (r < 0) {
            memset(&keys, 0, sizeof(keys));
            return 0;
        }
        opt.dlc_null_key = r == 1;
    }

    int ok = 0, done = 0;
    bool unlocked = false;
    char report[1400] = "";
    size_t rl = 0;
    for (int i = 0; i < n; i++) {
        if (ftype_detect(paths[i]) != FT_WAD) continue;
        const char *name = vfs_basename(paths[i]);
        char line[400];
        w2b_result_t res;
        int rc = -1;
        vfs_file f;
        if (vfs_open_read(&f, paths[i])) {
            src_t s = { vsrc_read, vsrc_close, &f, f.size };
            w2b_io_t io = { (void *)name, vio_mkdirs, vio_open, vio_write, vio_close, vio_remove, vio_progress };
            progress(name, "Converting for the SD card...", 0, 1);
            rc = w2b_convert(&s, vfs_dev_root(DEV_SD), &keys, &opt, &io, &res);
            vfs_close(&f);
        } else
            snprintf(res.err, sizeof(res.err), "Can't open the file");
        done++;
        if (rc) {
            snprintf(line, sizeof(line), "%s: %s", name, res.err);
        } else {
            char msg[200] = "", note[300] = "";
            if (!unlocked) unlocked = risk_ensure_unlocked();
            int r2 = unlocked ? install_one(paths[i], msg, sizeof(msg), note, sizeof(note), true) : -998;
            if (!unlocked) snprintf(msg, sizeof(msg), "Ticket not installed (NAND locked). Install %s with a WAD manager.",
                                    res.bogus[0] ? vfs_basename(res.bogus) : "its ticket");
            if (r2 == 0) ok++;
            snprintf(line, sizeof(line), "%s: %s %s. %s", name, res.dlc ? "DLC written to" : "Written to",
                     res.path, msg);
        }
        if (wads == 1) {
            memset(&keys, 0, sizeof(keys));
            vfs_update_space(DEV_SD);
            ui_message(rc ? "INSTALL TO SD FAILED" : "INSTALLED TO SD", rc ? C_RED : C_GREEN, line);
            return ok;
        }
        if (rl < sizeof(report) - 1) rl += snprintf(report + rl, sizeof(report) - rl, "%s %s\n", rc ? "FAIL" : "OK  ", line);
        if (rl >= sizeof(report)) rl = sizeof(report) - 1;
    }
    memset(&keys, 0, sizeof(keys));
    vfs_update_space(DEV_SD);
    char title[48];
    snprintf(title, sizeof(title), "INSTALLED %d OF %d TO SD", ok, done);
    ui_message(title, ok == done ? C_GREEN : C_YELLOW, report);
    return ok;
}

#ifdef GMW_AUTOTEST
/* Test builds: convert `wad` with the keys in `keyfile` (console ID, NG key, PRNG key, device
 * certificate) and log the outcome, with no questions asked. */
void wad2bin_autotest(const char *wad, const char *keyfile, bool null_key)
{
    w2b_keys_t k;
    FILE *kf = fopen(keyfile, "rb"), *lg;
    u8 raw[4 + 30 + 16 + 0x180];
    bool have = kf && fread(raw, 1, sizeof(raw), kf) == sizeof(raw);
    if (kf) fclose(kf);
    w2b_result_t res;
    int rc = -1;
    memset(&res, 0, sizeof(res));
    if (have) {
        k.console_id = be32(raw);
        memcpy(k.ng_priv, raw + 4, 30);
        memcpy(k.prng_key, raw + 34, 16);
        memcpy(k.device_cert, raw + 50, 0x180);
        vfs_file f;
        if (vfs_open_read(&f, wad)) {
            src_t s = { vsrc_read, vsrc_close, &f, f.size };
            w2b_io_t io = { (void *)"test", vio_mkdirs, vio_open, vio_write, vio_close, vio_remove, NULL };
            w2b_opts_t opt = { null_key, 0 };
            rc = w2b_convert(&s, "sd:/", &k, &opt, &io, &res);
            vfs_close(&f);
        }
    }
    if ((lg = fopen("sd:/gmwtest.log", "a"))) {
        fprintf(lg, "wad2bin %s: %s %s files=%d bogus=%s %s\n", wad, rc ? "FAILED" : "ok", res.path, res.files,
                res.bogus, res.err);
        fclose(lg);
    }
}
#endif

/*
 * HBC Reborn (github.com/Monsterray/hbc-reborn): installs or updates the Homebrew Channel from that
 * project's latest release. See hbcreborn.h.
 *
 * The release has two fakesigned channel WADs: -LULZ (00010001-4C554C5A, the Homebrew Channel's ID since
 * 1.1.0) and -OHBC (its own channel). The installed Homebrew Channel is replaced in place, under its own
 * title ID: LULZ and OHBC from their WAD; the older IDs (JODI for 1.0.x, HAXX for the first beta) from the
 * LULZ WAD given that ID, its title key encrypted again for it and both ticket and TMD fakesigned.
 * Each WAD is checked against the release's SHA256SUMS before it goes anywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogc/es.h>
#include <ogc/lwp_watchdog.h>

#include "hbcreborn.h"
#include "wiiupdate.h"
#include "wadinstall.h"
#include "ftp.h"
#include "ui.h"
#include "vfs.h"
#include "input.h"
#include "aes.h"
#include "sha1.h"
#include "archive.h"

#define API_URL  "https://api.github.com/repos/Monsterray/hbc-reborn/releases/latest"
#define WAD_MAX  (16 * 1024 * 1024)

typedef struct {
    u32 id;             /* low half of the title ID; the high half is 00010001 */
    const char *what;
} hbc_id;

static const hbc_id IDS[] = {
    { 0x4C554C5A, "the Homebrew Channel (LULZ)" },
    { 0x4F484243, "HBC Reborn's own channel (OHBC)" },
    { 0x4A4F4449, "the Homebrew Channel 1.0.x (JODI)" },
    { 0x48415858, "the Homebrew Channel beta (HAXX)" },
};
#define NIDS ((int)(sizeof(IDS) / sizeof(IDS[0])))

static u64 tid_of(u32 lo) { return 0x0001000100000000ull | lo; }

static u32 up64(u32 v) { return (v + 63) & ~63u; }

/* The installed version of a title, or -1 when it isn't installed. */
static int installed_version(u32 lo)
{
    u32 size = 0;
    if (ES_GetStoredTMDSize(tid_of(lo), &size) < 0 || !size) return -1;
    u8 *tmd = memalign(32, (size + 31) & ~31u);
    if (!tmd) return -1;
    int v = ES_GetStoredTMD(tid_of(lo), (signed_blob *)tmd, size) < 0 ? -1 : be16(tmd + 0x1DC);
    free(tmd);
    return v;
}

/* A string value after "key": in GitHub's JSON, from p on; copied to out. NULL when there is none. */
static const char *json_str(const char *p, const char *key, char *out, size_t n)
{
    char k[40];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *s = strstr(p, k);
    if (!s) return NULL;
    s = strchr(s + strlen(k), ':');
    if (!s) return NULL;
    while (*++s == ' ') {}
    if (*s != '"') return NULL;
    size_t i = 0;
    for (s++; *s && *s != '"' && i + 1 < n; s++) out[i++] = *s;
    out[i] = '\0';
    return *s == '"' ? s + 1 : NULL;
}

/* The download URL of the asset whose name ends with `suffix` ("-LULZ.wad", "SHA256SUMS"). */
static bool asset_url(const char *json, const char *suffix, char *url, size_t n, char *name, size_t nn)
{
    for (const char *p = json; (p = json_str(p, "browser_download_url", url, n));) {
        const char *slash = strrchr(url, '/');
        size_t ul = strlen(url), sl = strlen(suffix);
        if (slash && ul >= sl && !strcmp(url + ul - sl, suffix)) {
            snprintf(name, nn, "%s", slash + 1);
            return true;
        }
    }
    return false;
}

typedef struct {
    const char *what;
    u64 last;
} prog_t;

static int progress(void *ctx, long long done, long long total)
{
    prog_t *pr = ctx;
    u64 t = ticks_to_millisecs(gettime());
    if (t - pr->last >= 200) {
        pr->last = t;
        char l1[96], l2[64];
        snprintf(l1, sizeof(l1), "%s", pr->what);
        if (total > 0)
            snprintf(l2, sizeof(l2), "%lld of %lld KB   (hold B to stop)", done / 1024, total / 1024);
        else
            snprintf(l2, sizeof(l2), "%lld KB   (hold B to stop)", done / 1024);
        ui_splash("HBC REBORN");
        int r = ui_rows() / 2 + 1, cols = ui_cols();
        ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(l1)) / 2, "", l1);
        ui_field(r + 1, 0, cols - 1, "%*s%s", (cols - (int)strlen(l2)) / 2, "", l2);
        ui_bar(r + 2, (cols - 50) / 2, 50, total > 0 ? (int)(done * 100 / total) : 0, C_GREEN);
    }
    return input_cancel_held();
}

/* The hex SHA-256 SHA256SUMS lists for `name`. */
static bool sum_for(const char *sums, const char *name, u8 out[32])
{
    for (const char *p = sums; *p;) {
        const char *end = p + strcspn(p, "\r\n");
        const char *sp = memchr(p, ' ', (size_t)(end - p));
        if (sp && sp - p == 64) {
            const char *nm = sp;
            while (nm < end && (*nm == ' ' || *nm == '*')) nm++;
            if ((size_t)(end - nm) == strlen(name) && !memcmp(nm, name, strlen(name))) {
                for (int i = 0; i < 32; i++) {
                    unsigned v;
                    if (sscanf(p + i * 2, "%2x", &v) != 1) return false;
                    out[i] = (u8)v;
                }
                return true;
            }
        }
        p = end + strspn(end, "\r\n");
    }
    return false;
}

/* The trucha bug: a zeroed signature and a body whose SHA-1 starts with 0, by trying values in `field`. */
static bool fakesign(u8 *sig_start, const u8 *body, u32 len, u8 *field)
{
    memset(sig_start + 4, 0, 0x100);
    for (u32 v = 0; v < 0x10000; v++) {
        field[0] = (u8)(v >> 8);
        field[1] = (u8)v;
        u8 h[20];
        sha1_t s;
        sha1_init(&s);
        sha1_update(&s, body, len);
        sha1_final(&s, h);
        if (!h[0]) return true;
    }
    return false;
}

/* Gives the WAD in `w` the title 00010001-<lo>: the TMD's and ticket's title ID, the title key encrypted
 * for it (its IV is the title ID), then both fakesigned. */
static bool retitle(u8 *w, u32 len, u32 lo)
{
    if (len < 0x40 || be32(w) != 0x20) return false;
    u32 certs = be32(w + 8), crl = be32(w + 12), tik_len = be32(w + 16), tmd_len = be32(w + 20);
    u32 tik_off = up64(0x20) + up64(certs) + up64(crl), tmd_off = tik_off + up64(tik_len);
    if (tik_len < 0x2A4 || tmd_len < 0x1E4 || (u64)tmd_off + tmd_len > len) return false;
    u8 *tik = w + tik_off, *tmd = w + tmd_off;
    if (be32(tik) != 0x10001 || be32(tmd) != 0x10001) return false;   /* RSA-2048 signatures */

    u8 ck[16], key[16], iv[16];
    if (!arc_common_key(tik[0x1F1], ck)) return false;
    memset(iv, 0, 16);
    memcpy(iv, tik + 0x1DC, 8);
    aes128_t d;
    aes128_init_dec(&d, ck);
    aes128_cbc_decrypt(&d, iv, tik + 0x1BF, key, 16);
    u8 newid[8] = { 0x00, 0x01, 0x00, 0x01, (u8)(lo >> 24), (u8)(lo >> 16), (u8)(lo >> 8), (u8)lo };
    memcpy(tik + 0x1DC, newid, 8);
    memcpy(tmd + 0x18C, newid, 8);
    memset(iv, 0, 16);
    memcpy(iv, newid, 8);
    aes128_enc_t e;
    aes128_init_enc(&e, ck);
    aes128_cbc_encrypt(&e, iv, key, tik + 0x1BF, 16);
    memset(&d, 0, sizeof(d));
    memset(&e, 0, sizeof(e));
    memset(ck, 0, 16);
    memset(key, 0, 16);
    return fakesign(tik, tik + 0x140, 0x164, tik + 0x262) &&
           fakesign(tmd, tmd + 0x140, tmd_len - 0x140, tmd + 0x1E2);
}

static int pick_drive(void)
{
    static const int order[] = { DEV_SD, DEV_USB0, DEV_USB1, DEV_USB2, DEV_USB3, DEV_USB4, DEV_USB5, DEV_USB6,
                                 DEV_USB7, DEV_GCA, DEV_GCB };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (vfs_dev_writable(order[i])) return order[i];
    return -1;
}

static bool g_test;     /* autotest: no questions (target given), results to sd:/gmwtest.log */
static int g_test_target;

static void tlog(const char *what, const char *msg)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "hbcreborn %s: %s\n", what, msg);
        fclose(lg);
    }
}

static void fail(const char *msg)
{
    if (g_test) {
        tlog("FAIL", msg);
        return;
    }
    ui_alert_window(true);
    ui_message("HBC REBORN", C_RED, msg);
    ui_alert_window(false);
}

void hbcreborn_install(void)
{
    /* which Homebrew Channels are on the NAND */
    int ver[NIDS], ninst = 0;
    for (int i = 0; i < NIDS; i++)
        ninst += (ver[i] = installed_version(IDS[i].id)) >= 0;

    int dev = pick_drive();
    if (dev < 0) {
        fail("The WAD is downloaded to the SD card or a USB drive first, and there is no writable one.");
        return;
    }

    ui_splash("Connecting...");
    if (!ftp_net_start()) return;
    WIIUPDATE_Wait(15000);              /* GodModeWii's own update check shares the HTTPS code */

    ui_splash("Asking GitHub for HBC Reborn's latest release...");
    char *json = NULL;
    if (WIIUPDATE_Get(API_URL, "application/vnd.github+json", NULL, &json, NULL, 512 * 1024, NULL, NULL) < 0) {
        fail("Couldn't get HBC Reborn's latest release from GitHub (api.github.com). Is the network up?");
        return;
    }
    char tag[32] = "", lulz_url[400], lulz_name[96], ohbc_url[400], ohbc_name[96], sums_url[400], sums_name[64];
    json_str(json, "tag_name", tag, sizeof(tag));
    bool have_lulz = asset_url(json, "-LULZ.wad", lulz_url, sizeof(lulz_url), lulz_name, sizeof(lulz_name));
    bool have_ohbc = asset_url(json, "-OHBC.wad", ohbc_url, sizeof(ohbc_url), ohbc_name, sizeof(ohbc_name));
    bool have_sums = asset_url(json, "SHA256SUMS", sums_url, sizeof(sums_url), sums_name, sizeof(sums_name));
    free(json);
    if (!have_lulz || !have_sums) {
        char m[200];
        snprintf(m, sizeof(m), "HBC Reborn's release %s has no %s, so it can't be installed from here.",
                 tag[0] ? tag : "(latest)", have_sums ? "LULZ channel WAD" : "SHA256SUMS");
        fail(m);
        return;
    }

    /* where it goes: the installed one, in place; asked when there are several or none */
    int target = -1;
    if (g_test) {
        target = g_test_target;
    } else if (ninst == 1) {
        for (int i = 0; i < NIDS; i++)
            if (ver[i] >= 0) target = i;
    } else {
        const char *items[NIDS];
        char labels[NIDS][96];
        int map[NIDS], n = 0;
        for (int i = 0; i < NIDS; i++) {
            if (ninst ? ver[i] < 0 : i > 1) continue;
            if (i == 1 && !have_ohbc) continue;
            if (ver[i] >= 0)
                snprintf(labels[n], sizeof(labels[n]), "Update %s, installed v%d", IDS[i].what, ver[i]);
            else
                snprintf(labels[n], sizeof(labels[n]), i == 0 ? "As the Homebrew Channel (LULZ)" :
                                                                "As its own channel (OHBC)");
            items[n] = labels[n];
            map[n++] = i;
        }
        ui_clear();
        int r = ui_menu(ninst ? "WHICH HOMEBREW CHANNEL?" : "INSTALL HBC REBORN", items, NULL, n, 0);
        if (r < 0) return;
        target = map[r];
    }
    if (target == 1 && !have_ohbc) {
        fail("This release has no OHBC WAD.");
        return;
    }

    char now[48] = "";
    if (ver[target] >= 0) snprintf(now, sizeof(now), "\nInstalled now: title version %d.", ver[target]);
    char q[700];
    snprintf(q, sizeof(q),
             "Install HBC Reborn %s as %s (00010001-%08X)?%s\n\nThe channel WAD is downloaded from GitHub, checked "
             "against the release's SHA256SUMS and installed %s. It is fakesigned, which "
             "GodModeWii's patched IOS accepts.\n\nHave a BootMii NAND backup and Priiloader first, as HBC Reborn asks.",
             tag, IDS[target].what, (unsigned)IDS[target].id, now,
             ver[target] < 0 ? "as a new channel" : target > 1 ? "in place, given this channel's title ID (its saved settings stay)" : "in place (its saved settings stay)");
    if (g_test) tlog("ask", q);
    else if (!ui_confirm("HBC REBORN", C_YELLOW, q)) return;

    /* SHA256SUMS, then the WAD, into memory */
    char *sums = NULL;
    prog_t pr = { "Downloading SHA256SUMS", 0 };
    if (WIIUPDATE_Get(sums_url, NULL, NULL, &sums, NULL, 64 * 1024, progress, &pr) < 0) {
        fail("Couldn't download SHA256SUMS from the release.");
        return;
    }
    const char *url = target == 1 ? ohbc_url : lulz_url, *name = target == 1 ? ohbc_name : lulz_name;
    u8 want[32], got[32];
    bool listed = sum_for(sums, name, want);
    free(sums);
    if (!listed) {
        fail("The release's SHA256SUMS doesn't list the channel WAD, so it wasn't installed.");
        return;
    }
    char *wad = NULL;
    size_t len = 0;
    char what[128];
    snprintf(what, sizeof(what), "Downloading %s", name);
    pr.what = what;
    pr.last = 0;
    if (WIIUPDATE_Get(url, NULL, NULL, &wad, &len, WAD_MAX, progress, &pr) < 0) {
        fail(input_cancel_held() ? "Stopped. Nothing was installed." : "Couldn't download the channel WAD.");
        return;
    }
    WIIUPDATE_Sha256(wad, len, got);
    if (memcmp(got, want, 32)) {
        free(wad);
        fail("The downloaded WAD doesn't match the release's SHA256SUMS, so it wasn't installed.");
        return;
    }
    char lo[5] = { (char)(IDS[target].id >> 24), (char)(IDS[target].id >> 16), (char)(IDS[target].id >> 8),
                   (char)IDS[target].id, 0 };
    if (target > 1 && !retitle((u8 *)wad, (u32)len, IDS[target].id)) {
        free(wad);
        fail("Couldn't give the WAD this channel's title ID (an unexpected WAD layout, or no common key).");
        return;
    }

    /* to <drive>/GodModeWii/hbc-reborn/, then the usual install */
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], file[128];
    snprintf(dir, sizeof(dir), "%sGodModeWii", g_devs[dev].root);
    if (!vfs_exists(dir)) vfs_mkdir(dir);
    snprintf(dir, sizeof(dir), "%sGodModeWii/hbc-reborn", g_devs[dev].root);
    if (!vfs_exists(dir)) vfs_mkdir(dir);
    snprintf(file, sizeof(file), "hbc-reborn-%s-%s.wad", tag[0] == 'v' ? tag + 1 : tag, lo);
    vfs_file f;
    bool good = vfs_join(path, sizeof(path), dir, file) && vfs_open_write(&f, path);
    if (good) {
        good = vfs_write(&f, wad, (u32)len) == (s32)len;
        good = vfs_close_checked(&f) && good;
    }
    free(wad);
    if (!good) {
        char m[VFS_PATH_MAX + 64];
        snprintf(m, sizeof(m), "Couldn't save the WAD as %s.", path);
        fail(m);
        return;
    }
    char msg[600];
    int r;
#ifdef GMW_AUTOTEST
    if (g_test) {
        extern void wadinstall_autotest(const char *wad);
        wadinstall_autotest(path);
        int v = installed_version(IDS[target].id);
        snprintf(msg, sizeof(msg), "%s installed, title version %d", path, v);
        tlog("done", msg);
        return;
    }
#endif
    r = wad_install_one(path, msg, sizeof(msg));
    char text[VFS_PATH_MAX + 700];
    snprintf(text, sizeof(text), "%s\n\n%s%s", file, msg,
             r == 0 ? "\n\nThe WAD stays in GodModeWii/hbc-reborn/." : "");
    if (r == 0)
        ui_message("HBC REBORN INSTALLED", C_GREEN, text);
    else
        fail(text);
}

#ifdef GMW_AUTOTEST
/* "@hbcreborn <LULZ|OHBC|JODI|HAXX>": that target, no questions; then each installed HBC logged. */
void hbcreborn_autotest(const char *id)
{
    g_test = true;
    g_test_target = 0;
    for (int i = 0; i < NIDS; i++)
        if ((u32)be32((const u8 *)id) == IDS[i].id) g_test_target = i;
    hbcreborn_install();
    g_test = false;
    for (int i = 0; i < NIDS; i++) {
        char m[80];
        snprintf(m, sizeof(m), "%08X version %d", (unsigned)IDS[i].id, installed_version(IDS[i].id));
        tlog("nand", m);
    }
}
#endif

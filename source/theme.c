/*
 * Wii Menu theme installer. See theme.h.
 *
 * The System Menu identification, the theme checks, the vWii 4:3 database patch and the
 * write-to-/tmp-then-rename install follow Naim2000's csm-installer
 * (https://github.com/Naim2000/csm-installer); the 43DB layout is DarkMatterCore's
 * ww-43db-patcher's. Added here: the theme is read back from /tmp and hash checked before the old
 * archive is touched, the result is checked again after the rename, and a backup of whatever is
 * installed is required (and verified) before anything is written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <gccore.h>
#include <ogc/es.h>
#include <ogc/isfs.h>
#include <network.h>
#include <poll.h>

#include "theme.h"
#include "mym.h"
#include "vfs.h"
#include "arcfs.h"
#include "ui.h"
#include "risk.h"
#include "sha1.h"
#include "archive.h"
#include "aes.h"
#include "ftp.h"

#define SM_TID       0x0000000100000002ULL
#define CONTENT_FMT  "/title/00000001/00000002/content/%08x.app"
#define TMP_FMT      "/tmp/%08x.app"
#define CHUNK        0x10000
#define THEME_MIN    (512u * 1024)
#define THEME_MAX    (32u * 1024 * 1024)
#define MYM_FILE_MAX (16u * 1024 * 1024)

static u8 bounce[CHUNK] ATTRIBUTE_ALIGN(32);

/* ---------------------------------------------------------------- the System Menu */

typedef struct {
    u16 rev;                    /* title version: 513 = 4.3U */
    char region;                /* J U E K */
    char major;                 /* '1' .. '4' */
    char platform;              /* 'W' Wii, 'V' vWii, 'M' Wii mini */
    bool priiloader;
    bool missing;               /* the archive's file isn't on the NAND (a failed install) */
    u32 cid;                    /* the layout archive */
    u16 index;                  /* its content index (the IV when it is downloaded) */
    u32 size;
    u8 hash[20];
} sysmenu_t;

static char sm_region(u16 rev)
{
    switch (rev & 0x1F) {
    case 0: return 'J';
    case 1: return 'U';
    case 2: return 'E';
    case 6: return 'K';
    }
    return 0;
}

static char sm_major(u16 rev)
{
    switch (rev >> 5) {
    case 0x01: case 0x02: return '1';
    case 0x03: case 0x04: case 0x05: case 0x06: return '2';
    case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: return '3';
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
    case 0x11: case 0x13:       /* vWii */
    case 0x90:                  /* Wii mini */
        return '4';
    }
    return 0;
}

static void sm_name(const sysmenu_t *sm, char *out, size_t n)
{
    const char *minor = "x";
    switch (sm->rev >> 5) {
    case 0x0D: minor = "0"; break;
    case 0x0E: minor = "1"; break;
    case 0x0F: minor = "2"; break;
    case 0x10: case 0x11: case 0x13: case 0x90: minor = "3"; break;
    }
    snprintf(out, n, "%c.%s%c%s (v%u)", sm->major, minor, sm->region,
             sm->platform == 'V' ? " vWii" : sm->platform == 'M' ? " Wii mini" : "", sm->rev);
}

static bool nand_exists(const char *path)
{
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0) return false;
    ISFS_Close(fd);
    return true;
}

static bool sysmenu_probe(sysmenu_t *sm, char *why, size_t wn)
{
    memset(sm, 0, sizeof(*sm));
    u32 sz = 0;
    if (ES_GetStoredTMDSize(SM_TID, &sz) < 0 || sz < sizeof(sig_rsa2048) + sizeof(tmd) || sz > MAX_SIGNED_TMD_SIZE) {
        snprintf(why, wn, "Couldn't read the System Menu's TMD.");
        return false;
    }
    signed_blob *b = memalign(32, (sz + 31) & ~31u);
    if (!b || ES_GetStoredTMD(SM_TID, b, sz) < 0) {
        free(b);
        snprintf(why, wn, "Couldn't read the System Menu's TMD.");
        return false;
    }
    bool ok = false;
    tmd *t = SIGNATURE_PAYLOAD(b);
    u32 sigsz = SIGNATURE_SIZE(b);
    if (!sigsz || sigsz + sizeof(tmd) + (u32)t->num_contents * sizeof(tmd_content) > sz) {
        snprintf(why, wn, "The System Menu's TMD is damaged.");
        goto out;
    }
    sm->rev = t->title_version;
    if (sm->rev > 0x2000) {
        snprintf(why, wn, "Unknown System Menu version %u: not a stock System Menu.", sm->rev);
        goto out;
    }
    if (!(sm->region = sm_region(sm->rev)) || !(sm->major = sm_major(sm->rev))) {
        snprintf(why, wn, "Unknown System Menu version %u (region or version not recognized).", sm->rev);
        goto out;
    }
    sm->platform = t->vwii_title ? 'V' : (sm->rev & 0xFFE0) == 0x1200 ? 'M' : 'W';

    const tmd_content *gone = NULL;
    int ngone = 0;
    for (int i = 0; i < t->num_contents; i++) {
        const tmd_content *c = &t->contents[i];
        if (c->type & 0x8000)
            continue;
        char path[64];
        snprintf(path, sizeof(path), CONTENT_FMT, c->cid);
        if (c->index == t->boot_index) {
            /* Priiloader moves the System Menu's boot content to 1xxxxxxx.app (as its installer
             * checks it) */
            path[strlen("/title/00000001/00000002/content/")] = '1';
            sm->priiloader = nand_exists(path);
            continue;
        }
        s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
        if (fd < 0) {
            gone = c;
            ngone++;
            continue;
        }
        s32 r = ISFS_Read(fd, bounce, 32);
        ISFS_Close(fd);
        if (r >= 4 && be32(bounce) == 0x55AA382Du) {
            sm->cid = c->cid;
            sm->index = c->index;
            sm->size = (u32)c->size;
            memcpy(sm->hash, c->hash, 20);
        }
    }
    if (!sm->cid && ngone == 1 && gone->size >= THEME_MIN && gone->size <= THEME_MAX) {
        /* the one content that isn't there: the archive, removed by an install that failed half way */
        sm->cid = gone->cid;
        sm->index = gone->index;
        sm->size = (u32)gone->size;
        memcpy(sm->hash, gone->hash, 20);
        sm->missing = true;
    }
    if (!sm->cid) {
        snprintf(why, wn, "Couldn't find the System Menu's layout archive on the NAND.");
        goto out;
    }
    if (sm->size < THEME_MIN || sm->size > THEME_MAX) {
        snprintf(why, wn, "The System Menu's layout archive has an unexpected size (%u bytes).", sm->size);
        goto out;
    }
    ok = true;
out:
    free(b);
    return ok;
}

/* ---------------------------------------------------------------- dialogs */

#ifdef GMW_AUTOTEST
/* Test builds: "@theme install|convert <path>" in sd:/gmwtest.txt runs these with every question
 * answered yes and every message written to sd:/gmwtest.log. */
static bool g_auto;
static void tlog(const char *title, const char *text)
{
    FILE *f = fopen("sd:/gmwtest.log", "a");
    if (f) {
        fprintf(f, "[theme] %s: %s\n", title, text);
        fclose(f);
    }
}
static void msg(const char *title, int color, const char *text)
{
    if (g_auto) tlog(title, text);
    else ui_message(title, color, text);
}
static bool ask(const char *title, int color, const char *text)
{
    if (!g_auto) return ui_confirm(title, color, text);
    tlog(title, "(answered yes)");
    return true;
}
static bool unlocked(void) { return g_auto || risk_ensure_unlocked(); }
#else
#define msg ui_message
#define ask ui_confirm
#define unlocked risk_ensure_unlocked
#endif

/* ---------------------------------------------------------------- progress */

static const char *g_title = "WII MENU THEME";

static void progress(const char *what, u64 done, u64 total)
{
    ui_splash(g_title);
    int r = ui_rows() / 2 + 2, cols = ui_cols();
    ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(what)) / 2, "", what);
    int pct = total ? (int)(done * 100 / total) : 0;
    ui_bar(r + 1, (cols - 50) / 2, 50, pct, C_GREEN);
}

static void mym_progress(void *ctx, const char *what, int done, int total)
{
    (void)ctx;
    char line[96];
    snprintf(line, sizeof(line), "Building the theme: %.60s", what);
    progress(line, done, total);
}

/* ---------------------------------------------------------------- NAND files */

static void sha_buf(const u8 *p, u32 n, u8 out[20])
{
    sha1_t c;
    sha1_init(&c);
    sha1_update(&c, p, n);
    sha1_final(&c, out);
}

/* SHA-1 and size of a NAND file. */
static bool nand_sha(const char *path, u32 *len, u8 out[20], const char *what)
{
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0) return false;
    static fstats st ATTRIBUTE_ALIGN(32);
    if (ISFS_GetFileStats(fd, &st) < 0) {
        ISFS_Close(fd);
        return false;
    }
    sha1_t c;
    sha1_init(&c);
    u32 got = 0;
    while (got < st.file_length) {
        u32 want = st.file_length - got > CHUNK ? CHUNK : st.file_length - got;
        s32 r = ISFS_Read(fd, bounce, want);
        if (r <= 0) break;
        sha1_update(&c, bounce, r);
        got += r;
        if (what) progress(what, got, st.file_length);
    }
    ISFS_Close(fd);
    if (got != st.file_length) return false;
    sha1_final(&c, out);
    *len = got;
    return true;
}

static u8 *nand_read_all(const char *path, u32 max, u32 *len)
{
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0) return NULL;
    static fstats st ATTRIBUTE_ALIGN(32);
    u8 *buf = NULL;
    if (ISFS_GetFileStats(fd, &st) < 0 || !st.file_length || st.file_length > max ||
        !(buf = memalign(32, (st.file_length + 31) & ~31u))) {
        ISFS_Close(fd);
        return NULL;
    }
    u32 got = 0;
    while (got < st.file_length) {
        s32 r = ISFS_Read(fd, buf + got, st.file_length - got > CHUNK ? CHUNK : st.file_length - got);
        if (r <= 0) break;
        got += r;
        progress("Reading the System Menu archive", got, st.file_length);
    }
    ISFS_Close(fd);
    if (got != st.file_length) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/* Creates `path` with the given permissions and writes `buf` to it, then reads it back: true only
 * when the file on the NAND has exactly these bytes. */
static bool nand_write_checked(const char *path, const u8 *buf, u32 len, const u8 sha[20], u8 attr, u8 op,
                               u8 gp, u8 ot)
{
    ISFS_Delete(path);
    if (ISFS_CreateFile(path, attr, op, gp, ot) < 0) return false;
    s32 fd = ISFS_Open(path, ISFS_OPEN_WRITE);
    if (fd < 0) return false;
    u32 done = 0;
    while (done < len) {
        u32 n = len - done > CHUNK ? CHUNK : len - done;
        memcpy(bounce, buf + done, n);
        s32 r = ISFS_Write(fd, bounce, n);
        if (r != (s32)n) break;
        done += n;
        progress("Writing the theme to the NAND", done, len);
    }
    if (ISFS_Close(fd) < 0 || done != len) return false;
    u32 back = 0;
    u8 got[20];
    return nand_sha(path, &back, got, "Checking what was written") && back == len && !memcmp(got, sha, 20);
}

/* ---------------------------------------------------------------- SD / USB files */

static int backup_device(void)
{
    if (g_devs[DEV_SD].mounted && !vfs_is_readonly("sd:/"))
        return DEV_SD;
    for (int d = DEV_USB0; d <= DEV_USB7; d++)
        if (g_devs[d].mounted && !vfs_is_readonly(g_devs[d].root))
            return d;
    return -1;
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

static u8 *file_read_all(const char *path, u32 max, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    u8 *buf = NULL;
    if (!f.size || f.size > max || !(buf = memalign(32, ((u32)f.size + 31) & ~31u))) {
        vfs_close(&f);
        return NULL;
    }
    u32 got = 0, size = (u32)f.size;
    while (got < size) {
        s32 r = vfs_read(&f, bounce, size - got > CHUNK ? CHUNK : size - got);
        if (r <= 0) break;
        memcpy(buf + got, bounce, r);
        got += r;
    }
    vfs_close(&f);
    if (got != size) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

static bool file_sha(const char *path, u32 *len, u8 out[20])
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    sha1_t c;
    sha1_init(&c);
    u64 got = 0;
    while (got < f.size) {
        s32 r = vfs_read(&f, bounce, f.size - got > CHUNK ? CHUNK : (u32)(f.size - got));
        if (r <= 0) break;
        sha1_update(&c, bounce, r);
        got += r;
    }
    bool ok = got == f.size;
    vfs_close(&f);
    sha1_final(&c, out);
    *len = (u32)got;
    return ok;
}

/* Writes `buf` to `path` and reads it back. */
static bool file_write_checked(const char *path, const u8 *buf, u32 len)
{
    char dir[VFS_PATH_MAX];
    if (!vfs_parent(path, dir, sizeof(dir)) || !mkdirs(dir)) return false;
    vfs_file f;
    if (!vfs_open_write(&f, path)) return false;
    u32 done = 0;
    while (done < len) {
        u32 n = len - done > CHUNK ? CHUNK : len - done;
        memcpy(bounce, buf + done, n);
        if (vfs_write(&f, bounce, n) != (s32)n) break;
        done += n;
        progress("Saving to the SD card", done, len);
    }
    if (!vfs_close_checked(&f) || done != len) return false;
    u8 want[20], got[20];
    u32 back;
    sha_buf(buf, len, want);
    return file_sha(path, &back, got) && back == len && !memcmp(want, got, 20);
}

/* Copies the NAND file to `path` and checks the copy against `sha`. */
static bool nand_copy_checked(const char *nand, const char *path, u32 len, const u8 sha[20])
{
    u32 got_len;
    u8 *buf = nand_read_all(nand, THEME_MAX, &got_len);
    if (!buf) return false;
    u8 got[20];
    sha_buf(buf, got_len, got);
    bool ok = got_len == len && !memcmp(got, sha, 20) && file_write_checked(path, buf, got_len);
    free(buf);
    return ok;
}

/* ---------------------------------------------------------------- NUS download (csm-installer's) */

#define NUS_HOST "nus.cdn.shop.wii.com"
#define NET_WAIT_MS 15000

/* Plain HTTP GET (NUS has no HTTPS) of up to `cap` bytes into `buf`; the body must be exactly
 * `want` bytes long. */
static bool http_get(const char *host, const char *path, u8 *buf, u32 want, char *why, size_t wn)
{
    struct hostent *he = net_gethostbyname(host);
    if (!he || he->h_length != 4 || !he->h_addr_list || !he->h_addr_list[0]) {
        snprintf(why, wn, "Couldn't look up %s.", host);
        return false;
    }
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        snprintf(why, wn, "Couldn't open a network socket (%d).", s);
        return false;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    memcpy(&a.sin_addr, he->h_addr_list[0], 4);
    bool ok = false;
    if (net_connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        snprintf(why, wn, "Couldn't connect to %s.", host);
        goto out;
    }
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: wii libnup/1.0\r\nConnection: close\r\n\r\n",
                      path, host);
    for (int sent = 0; sent < rl;) {
        s32 r = net_send(s, req + sent, rl - sent, 0);
        if (r <= 0) {
            snprintf(why, wn, "Couldn't send the request to %s.", host);
            goto out;
        }
        sent += r;
    }

    char head[2048];
    u32 hl = 0, got = 0;
    bool in_body = false;
    long long length = -1;
    for (;;) {
        struct pollsd sd = { s, POLLIN, 0 };
        if (net_poll(&sd, 1, NET_WAIT_MS) <= 0) {
            snprintf(why, wn, "%s stopped answering.", host);
            goto out;
        }
        s32 r;
        if (!in_body) {
            r = net_recv(s, head + hl, sizeof(head) - 1 - hl, 0);
            if (r <= 0) {
                snprintf(why, wn, "%s closed the connection.", host);
                goto out;
            }
            hl += r;
            head[hl] = '\0';
            char *end = strstr(head, "\r\n\r\n");
            if (!end) {
                if (hl >= sizeof(head) - 1) {
                    snprintf(why, wn, "Bad answer from %s.", host);
                    goto out;
                }
                continue;
            }
            int status = 0;
            if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
                snprintf(why, wn, "%s answered with HTTP %d.", host, status);
                goto out;
            }
            for (char *l = strstr(head, "\r\n"); l && l < end; l = strstr(l + 2, "\r\n"))
                if (!strncasecmp(l + 2, "content-length:", 15)) length = strtoll(l + 17, NULL, 10);
            if (length >= 0 && length != want) {
                snprintf(why, wn, "%s sent %lld bytes; %u were expected.", host, length, want);
                goto out;
            }
            in_body = true;
            u32 extra = hl - (u32)(end + 4 - head);
            if (extra > want) extra = want;
            memcpy(buf, end + 4, extra);
            got = extra;
        } else {
            r = net_recv(s, buf + got, want - got > CHUNK ? CHUNK : want - got, 0);
            if (r < 0) {
                snprintf(why, wn, "The download from %s broke off.", host);
                goto out;
            }
            if (r == 0) break;
            got += r;
        }
        progress("Downloading the original Wii Menu from Nintendo", got, want);
        if (got >= want) break;
    }
    ok = got == want;
    if (!ok) snprintf(why, wn, "The download from %s ended early (%u of %u bytes).", host, got, want);
out:
    net_close(s);
    return ok;
}

/* The stock archive, as csm-installer's DownloadOriginalTheme gets it: the encrypted content from
 * NUS, decrypted with the title key in the System Menu's ticket, and hash checked against the TMD. */
static u8 *nus_download(const sysmenu_t *sm, u32 *len, char *why, size_t wn)
{
    u32 tl;
    u8 *tik = nand_read_all("/ticket/00000001/00000002.tik", 0x4000, &tl);
    if (!tik || tl < 0x2A4 || be32(tik) != 0x10001) {
        free(tik);
        snprintf(why, wn, "Couldn't read the System Menu's ticket, which the download is decrypted with.");
        return NULL;
    }
    u8 ck[16], tk[16], iv[16];
    int idx = tik[0x1F1] > 2 ? 0 : tik[0x1F1];
    if (!arc_common_key(idx, ck)) {
        free(tik);
        snprintf(why, wn, "The System Menu's ticket uses an unknown common key.");
        return NULL;
    }
    aes128_t c;
    aes128_init_dec(&c, ck);
    memset(iv, 0, 16);
    memcpy(iv, tik + 0x1DC, 8);
    aes128_cbc_decrypt(&c, iv, tik + 0x1BF, tk, 16);
    free(tik);

    char name[48], q[300];
    sm_name(sm, name, sizeof(name));
    snprintf(q, sizeof(q),
             "The original Wii Menu archive is needed to build the theme, and there is no copy on the "
             "console or the SD card.\n\nDownload it (%u KiB, System Menu %s) from Nintendo's update "
             "server?", sm->size / 1024, name);
    if (!ask(g_title, C_CYAN, q) || !ftp_net_start()) {
        snprintf(why, wn, "The original Wii Menu archive wasn't downloaded.");
        return NULL;
    }
    u32 enc = (sm->size + 15) & ~15u;
    u8 *buf = memalign(32, enc);
    if (!buf) {
        snprintf(why, wn, "Out of memory.");
        return NULL;
    }
    char path[96];
    snprintf(path, sizeof(path), "/ccs/download/%016llx/%08x",
             sm->platform == 'V' ? 0x0000000700000002ULL : SM_TID, sm->cid);
    if (!http_get(NUS_HOST, path, buf, enc, why, wn)) {
        free(buf);
        return NULL;
    }
    progress("Decrypting the original Wii Menu", 0, 1);
    aes128_init_dec(&c, tk);
    memset(iv, 0, 16);
    iv[0] = sm->index >> 8;
    iv[1] = sm->index;
    aes128_cbc_decrypt(&c, iv, buf, buf, enc);
    memset(tk, 0, 16);
    memset(&c, 0, sizeof(c));
    u8 sha[20];
    sha_buf(buf, sm->size, sha);
    if (memcmp(sha, sm->hash, 20)) {
        free(buf);
        snprintf(why, wn, "The downloaded Wii Menu archive doesn't match the TMD's hash.");
        return NULL;
    }
    *len = sm->size;
    return buf;
}

/* ---------------------------------------------------------------- the theme */

static void original_path(const sysmenu_t *sm, int dev, char *out, size_t n)
{
    snprintf(out, n, "%sGodModeWii/themes/original/%08x-v%u.csm", g_devs[dev].root, sm->cid, sm->rev);
}

/* The stock archive: from the NAND when it is still stock, else a saved copy (GodModeWii's, or
 * csm-installer's sd:/themes/original_backup/), hash checked against the TMD. */
static u8 *load_stock(const sysmenu_t *sm, u32 *len, char *why, size_t wn)
{
    char path[VFS_PATH_MAX];
    snprintf(path, sizeof(path), CONTENT_FMT, sm->cid);
    u8 sha[20];
    u8 *b = nand_read_all(path, THEME_MAX, len);
    if (b) {
        sha_buf(b, *len, sha);
        if (*len == sm->size && !memcmp(sha, sm->hash, 20)) return b;
        free(b);
    }
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (!g_devs[d].mounted) continue;
        for (int k = 0; k < 2; k++) {
            if (k == 0) original_path(sm, d, path, sizeof(path));
            else snprintf(path, sizeof(path), "%sthemes/original_backup/%08x-v%u.app", g_devs[d].root, sm->cid, sm->rev);
            b = file_read_all(path, THEME_MAX, len);
            if (!b) continue;
            sha_buf(b, *len, sha);
            if (*len == sm->size && !memcmp(sha, sm->hash, 20)) return b;
            free(b);
        }
    }
    /* not on the console: download it, and keep it for next time */
    char nus_why[200];
    b = nus_download(sm, len, nus_why, sizeof(nus_why));
    if (b) {
        int dev = backup_device();
        if (dev >= 0) {
            original_path(sm, dev, path, sizeof(path));
            file_write_checked(path, b, *len);
        }
        return b;
    }
    snprintf(why, wn,
             "A custom theme is installed and there is no copy of the original Wii Menu archive to build "
             "the mym on.\n\n%s\n\nOr put the original %08x.app of System Menu v%u at\n"
             "sd:/GodModeWii/themes/original/%08x-v%u.csm",
             nus_why, sm->cid, sm->rev, sm->cid, sm->rev);
    return NULL;
}

typedef struct {
    char **norm, **full;
    int n;
} mymidx;

static void idx_free(mymidx *ix)
{
    for (int i = 0; i < ix->n; i++) {
        free(ix->norm[i]);
        free(ix->full[i]);
    }
    free(ix->norm);
    free(ix->full);
    memset(ix, 0, sizeof(*ix));
}

static void idx_walk(mymidx *ix, const char *dir, const char *rel, int depth)
{
    vfs_entry *ents = NULL;
    int cnt = 0;
    if (depth > 12 || !vfs_list(dir, &ents, &cnt)) return;
    for (int i = 0; i < cnt; i++) {
        char full[VFS_PATH_MAX], r[VFS_PATH_MAX];
        if (!vfs_join(full, sizeof(full), dir, ents[i].name)) continue;
        snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", ents[i].name);
        if (ents[i].is_dir) {
            idx_walk(ix, full, r, depth + 1);
            continue;
        }
        char **nn = realloc(ix->norm, sizeof(char *) * (ix->n + 1));
        if (!nn) break;
        ix->norm = nn;
        char **nf = realloc(ix->full, sizeof(char *) * (ix->n + 1));
        if (!nf) break;
        ix->full = nf;
        for (char *c = r; *c; c++) *c = *c == '\\' ? '/' : tolower((unsigned char)*c);
        ix->norm[ix->n] = strdup(r);
        ix->full[ix->n] = strdup(full);
        if (ix->norm[ix->n] && ix->full[ix->n]) ix->n++;
    }
    free(ents);
}

static bool mym_fetch(void *ctx, const char *member, uint8_t **data, uint32_t *len)
{
    mymidx *ix = ctx;
    for (int i = 0; i < ix->n; i++) {
        const char *m = ix->norm[i];
        while (*m == '/') m++;
        if (!strcmp(m, member)) {
            u8 *b = file_read_all(ix->full[i], MYM_FILE_MAX, len);
            if (!b) return false;
            *data = b;
            return true;
        }
    }
    return false;
}

static bool mym_ok_for(const sysmenu_t *sm, char *why, size_t wn)
{
    if (sm->platform == 'W' && (sm->rev >> 5) == 0x10) return true;
    char name[48];
    sm_name(sm, name, sizeof(name));
    snprintf(why, wn,
             "mym themes are built here for System Menu 4.3 on a Wii only; this console runs %s.\n\n"
             "Use a csm made for this exact menu instead.", name);
    return false;
}

static u8 *build_mym(const char *path, const sysmenu_t *sm, u32 *len, mym_result *res, char *why, size_t wn)
{
    u32 slen;
    u8 *stock = load_stock(sm, &slen, why, wn);
    if (!stock) return NULL;
    char root[VFS_PATH_MAX + 4];
    snprintf(root, sizeof(root), "%s|/", path);
    mymidx ix = { 0 };
    idx_walk(&ix, root, "", 0);
    u8 *out = NULL;
    if (!ix.n) {
        snprintf(why, wn, "Couldn't open the mym (it should be a zip archive).");
    } else {
        out = mym_build(stock, slen, mym_fetch, &ix, mym_progress, NULL, len, res, why, wn);
    }
    idx_free(&ix);
    free(stock);
    arcfs_flush();
    return out;
}

/* Where a csm built from `mym` is saved: <drive>GodModeWii/themes/<name>.4.3U.csm */
static bool built_path(const char *mym, const sysmenu_t *sm, char *out, size_t n)
{
    int dev = backup_device();
    if (dev < 0) return false;
    char base[VFS_NAME_MAX];
    snprintf(base, sizeof(base), "%s", vfs_basename(mym));
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(out, n, "%sGodModeWii/themes/%.180s.4.3%c.csm", g_devs[dev].root, base, sm->region);
    return true;
}

/* ---------------------------------------------------------------- checks (csm-installer's) */

typedef struct {
    bool stock;
    const char *signer;
    char version[24];
} csminfo;

static const char *ipl_setting_path(char region)
{
    switch (region) {
    case 'U': return "html/US2/iplsetting.ash";
    case 'E': return "html/EU2/iplsetting.ash";
    case 'J': return "html/JP2/iplsetting.ash";
    case 'K': return "html/KR2/iplsetting.ash";
    }
    return "html/XXX/iplsetting.ash";
}

static const char *main_sel_path(char region)
{
    switch (region) {
    case 'U': return "FINAL/US2/main.sel";
    case 'E': return "FINAL/EU2/main.sel";
    case 'J': return "FINAL/JP2/main.sel";
    case 'K': return "FINAL/KR2/main.sel";
    }
    return "FINAL/XXX/main.sel";
}

/* The System Menu ELF path in main.sel says which menu the theme was made from. */
static void elf_version(const char *path, char *base, char *major, char *minor, char *region)
{
    static const char s1[] = "\\System", s2[] = "\\Compat_irdrepo\\ipl\\Compat",
                      s3[] = "\\home\\neceCheck\\WiiMenu\\ipl\\bin\\RVL\\Final_";
    const char *p;
    *base = *major = *minor = *region = '?';
    if ((p = strstr(path, s1))) {
        *base = 'W';
        p += strlen(s1);
    } else if ((p = strstr(path, s2))) {
        *base = 'V';
        p += strlen(s2);
    } else if ((p = strstr(path, s3))) {
        p += strlen(s3);
        *base = 'M';
        *major = '4';
        *minor = '3';
        *region = *p;
        return;
    } else {
        return;
    }
    sscanf(p, "%c_%c\\ipl\\bin\\RVL\\Final_%c", major, minor, region);
}

static bool csm_check(const u8 *buf, u32 len, const sysmenu_t *sm, csminfo *ci, char *why, size_t wn)
{
    memset(ci, 0, sizeof(*ci));
    if (len < THEME_MIN || len > THEME_MAX) {
        snprintf(why, wn, "The file is %u bytes: too small or too big to be a Wii Menu theme.", len);
        return false;
    }
    u8arc u;
    if (!u8_parse(&u, buf, len)) {
        snprintf(why, wn, "This isn't a valid U8 archive, so it isn't a theme (or it is damaged).");
        return false;
    }
    bool ok = false;
    static const char *const need[] = { "www.arc", "layout/common/health.ash", "sound/IplSound.brsar" };
    for (int i = 0; i < 3; i++)
        if (u8_find(&u, need[i], true) < 0) {
            snprintf(why, wn, "/%s is missing: this is a U8 archive, but not a Wii Menu theme.", need[i]);
            goto out;
        }
    if (u8_find(&u, ipl_setting_path(sm->region), true) < 0) {
        snprintf(why, wn, "/%s is missing: this theme isn't for a %c region Wii Menu.",
                 ipl_setting_path(sm->region), sm->region);
        goto out;
    }
    int si = u8_find(&u, main_sel_path(sm->region), true);
    if (si < 0) {
        snprintf(why, wn, "/%s is missing: this theme isn't for this Wii Menu.", main_sel_path(sm->region));
        goto out;
    }
    const u8 *sel = u.n[si].data;
    u32 sel_len = u.n[si].len;
    if (sel_len < 0x120) {
        snprintf(why, wn, "main.sel is damaged.");
        goto out;
    }
    u32 po = be32(sel + 0x10);
    char elf[256] = "";
    if (po < sel_len) {
        u32 n = sel_len - po < sizeof(elf) - 1 ? sel_len - po : sizeof(elf) - 1;
        memcpy(elf, sel + po, n);
        elf[n] = '\0';
    }
    char base, major, minor, region;
    elf_version(elf, &base, &major, &minor, &region);
    snprintf(ci->version, sizeof(ci->version), "%c.%c%c%s", major, minor, region,
             base == 'V' ? " vWii" : base == 'M' ? " Wii mini" : base == 'W' ? "" : " ?");
    if (major != sm->major || base != sm->platform) {
        char name[48];
        sm_name(sm, name, sizeof(name));
        snprintf(why, wn, "Incompatible theme!\n\nTheme made for: %s\nThis console:   %s", ci->version, name);
        goto out;
    }
    if (region != '?' && strchr("JUEK", region) && region != sm->region) {
        char name[48];
        sm_name(sm, name, sizeof(name));
        snprintf(why, wn, "This theme is for the %c region; this console's Wii Menu is %s.", region, name);
        goto out;
    }

    u8 sha[20];
    sha_buf(buf, len, sha);
    ci->stock = len == sm->size && !memcmp(sha, sm->hash, 20);
    char s100[16], sf0[48];
    memcpy(s100, sel + 0x100, sizeof(s100) - 1);
    s100[sizeof(s100) - 1] = '\0';
    memcpy(sf0, sel + 0xF0, sizeof(sf0) - 1);
    sf0[sizeof(sf0) - 1] = '\0';
    if (!strcasecmp(s100, "Wii_Themer") || strstr(sf0, "wiithemer"))
        ci->signer = "wii-themer";
    else if (!strcmp(sf0, "ModMii_________\xa9______XFlak"))
        ci->signer = "ModMii";
    ok = true;
out:
    u8_free(&u);
    return ok;
}

/* vWii: drops the WiiConnect24 channels (HAJ*, HAP*) from the WiiWare 4:3 database, as
 * csm-installer does by default. Returns how many entries went. */
static int patch_43db(u8 *buf, u32 len)
{
    u8arc u;
    if (!u8_parse(&u, buf, len)) return 0;
    int idx = u8_find(&u, "titlelist/wwdb.bin", true);
    int filtered = 0;
    if (idx >= 0) {
        u32 off = u.n[idx].a, size = u.n[idx].len;
        u8 *db = buf + off;
        u32 count = size >= 16 ? be32(db + 8) : 0;
        if (size >= 16 && !memcmp(db, "43DB", 4) && count <= (size - 16) / 4) {
            u32 kept = 0;
            for (u32 i = 0; i < count; i++) {
                u32 e = be32(db + 16 + i * 4), id3 = e >> 8;
                if (id3 == 0x48414A || id3 == 0x484150) {
                    filtered++;
                    continue;
                }
                memmove(db + 16 + kept * 4, db + 16 + i * 4, 4);
                kept++;
            }
            memset(db + 16 + kept * 4, 0, (count - kept) * 4);
            if (filtered) {
                *(u32 *)(db + 8) = kept;
                u32 root = be32(buf + 4);
                *(u32 *)(buf + root + idx * 12 + 8) = size - filtered * 4;
            }
        }
    }
    u8_free(&u);
    return filtered;
}

/* ---------------------------------------------------------------- install */

static bool preflight(sysmenu_t *sm, bool mym)
{
    char why[400];
    if (!g_is_dolphin && !g_nand_full) {
        msg(g_title, C_RED,
                   "The System Menu's files can't be written without full NAND access.\n\n"
                   "Start GodModeWii from the Homebrew Channel (HW_AHBPROT off).");
        return false;
    }
    if (!sysmenu_probe(sm, why, sizeof(why))) {
        msg(g_title, C_RED, why);
        return false;
    }
    if (!sm->priiloader && !g_is_dolphin) {
        msg(g_title, C_RED,
                   "Priiloader isn't installed.\n\nA broken theme stops the Wii Menu from starting, and "
                   "Priiloader is what lets you start homebrew and put the old theme back. Install "
                   "Priiloader first.");
        return false;
    }
    if (mym && !mym_ok_for(sm, why, sizeof(why))) {
        msg(g_title, C_RED, why);
        return false;
    }
    if (backup_device() < 0) {
        msg(g_title, C_RED,
                   "Insert a writable SD card or USB drive: the current theme is saved there before "
                   "anything is installed.");
        return false;
    }
    return true;
}

bool theme_is_mym_name(const char *name) { return arc_name_has_ext(name, ".mym"); }
bool theme_is_name(const char *name) { return arc_name_has_ext(name, ".csm") || theme_is_mym_name(name); }

static void show_skipped(const mym_result *res)
{
    if (!res->skipped) return;
    char text[1200];
    snprintf(text, sizeof(text), "%d change(s) applied, %d skipped (not in this menu, or not supported):\n\n%s",
             res->applied, res->skipped, res->report);
    msg("MYM: SKIPPED ENTRIES", C_YELLOW, text);
}

void theme_convert(const char *path)
{
    g_title = "BUILD CSM FROM MYM";
    sysmenu_t sm;
    char why[600], out[VFS_PATH_MAX];
    if (!sysmenu_probe(&sm, why, sizeof(why))) {
        msg(g_title, C_RED, why);
        return;
    }
    if (!mym_ok_for(&sm, why, sizeof(why))) {
        msg(g_title, C_RED, why);
        return;
    }
    if (!built_path(path, &sm, out, sizeof(out))) {
        msg(g_title, C_RED, "Insert a writable SD card or USB drive to save the csm on.");
        return;
    }
    mym_result res;
    u32 len;
    u8 *csm = build_mym(path, &sm, &len, &res, why, sizeof(why));
    if (!csm) {
        msg(g_title, C_RED, why);
        return;
    }
    csminfo ci;
    bool valid = csm_check(csm, len, &sm, &ci, why, sizeof(why));
    bool saved = valid && file_write_checked(out, csm, len);
    free(csm);
    if (!valid) {
        char text[800];
        snprintf(text, sizeof(text), "The csm built from this mym fails the theme checks:\n\n%s", why);
        msg(g_title, C_RED, text);
        return;
    }
    show_skipped(&res);
    char text[VFS_PATH_MAX + 128];
    char shown[VFS_PATH_MAX];
    vfs_shown_path(out, shown, sizeof(shown));
    snprintf(text, sizeof(text), saved ? "Saved the csm (%u bytes) to\n%s" : "Couldn't save the csm (%u bytes) to\n%s",
             len, shown);
    msg(g_title, saved ? C_GREEN : C_RED, text);
}

void theme_install(const char *path)
{
    bool mym = theme_is_mym_name(path);
    g_title = "INSTALL WII MENU THEME";
    sysmenu_t sm;
    if (!preflight(&sm, mym))
        return;

    char smname[48], q[900], why[600];
    sm_name(&sm, smname, sizeof(smname));
    snprintf(q, sizeof(q),
             "DANGER: this replaces the Wii Menu's layout archive on the NAND. A bad theme stops the Wii "
             "Menu from starting.\n\n"
             "Theme: %.80s\nWii Menu: %s\n\n"
             "The theme is checked first, the current theme is saved to the SD card (or a USB drive), "
             "and you are asked once more before anything is written. Keep Priiloader handy (hold RESET "
             "while turning the console on). Don't turn the console off while installing.\n\nContinue?",
             vfs_basename(path), smname);
    if (!ask(g_title, C_RED, q))
        return;
    if (!unlocked())
        return;

    /* the theme, in memory */
    u32 len = 0;
    u8 *csm = NULL;
    mym_result res;
    memset(&res, 0, sizeof(res));
    char built[VFS_PATH_MAX] = "";
    if (mym) {
        csm = build_mym(path, &sm, &len, &res, why, sizeof(why));
        if (csm && built_path(path, &sm, built, sizeof(built)) && !file_write_checked(built, csm, len))
            built[0] = '\0';
    } else {
        progress("Reading the theme", 0, 1);
        csm = file_read_all(path, THEME_MAX, &len);
        if (!csm) snprintf(why, sizeof(why), "Couldn't read the theme (or it is bigger than 32 MiB).");
    }
    if (!csm) {
        msg(g_title, C_RED, why);
        return;
    }

    csminfo ci;
    if (!csm_check(csm, len, &sm, &ci, why, sizeof(why))) {
        free(csm);
        msg("THEME REFUSED", C_RED, why);
        return;
    }
    if (mym) show_skipped(&res);

    int db = sm.platform == 'V' ? patch_43db(csm, len) : 0;
    u8 sha[20];
    sha_buf(csm, len, sha);

    char cur[64], tmp[64];
    snprintf(cur, sizeof(cur), CONTENT_FMT, sm.cid);
    snprintf(tmp, sizeof(tmp), TMP_FMT, sm.cid);
    u32 cur_len = 0;
    u8 cur_sha[20];
    memset(cur_sha, 0, sizeof(cur_sha));
    if (sm.missing) {
        /* nothing installed to back up or remove */
    } else if (!nand_sha(cur, &cur_len, cur_sha, "Reading the installed theme")) {
        free(csm);
        msg(g_title, C_RED, "Couldn't read the installed theme from the NAND. Nothing was changed.");
        return;
    }
    if (!sm.missing && cur_len == len && !memcmp(cur_sha, sha, 20)) {
        free(csm);
        msg(g_title, C_GREEN, "This theme is already installed. Nothing was changed.");
        return;
    }
    vfs_update_space(DEV_NAND);
    if (g_devs[DEV_NAND].space_known && g_devs[DEV_NAND].free < (u64)len + 1024 * 1024) {
        free(csm);
        char sz[32];
        ui_fmt_size(sz, sizeof(sz), g_devs[DEV_NAND].free);
        snprintf(why, sizeof(why),
                 "Not enough free NAND space: the theme is written next to the old one first, and only %s "
                 "is free. Nothing was changed.", sz);
        msg(g_title, C_RED, why);
        return;
    }

    /* the backup of what is installed now: required */
    bool cur_stock = !sm.missing && cur_len == sm.size && !memcmp(cur_sha, sm.hash, 20);
    int dev = backup_device();
    char backup[VFS_PATH_MAX];
    if (sm.missing) {
        snprintf(backup, sizeof(backup), "(none: the Wii Menu's archive is missing from the NAND)");
    } else if (cur_stock) {
        original_path(&sm, dev, backup, sizeof(backup));
        u32 bl;
        u8 bs[20];
        if (!(file_sha(backup, &bl, bs) && bl == cur_len && !memcmp(bs, cur_sha, 20)) &&
            !nand_copy_checked(cur, backup, cur_len, cur_sha)) {
            free(csm);
            msg(g_title, C_RED, "Couldn't save the original theme to the SD card. Nothing was changed.");
            return;
        }
    } else {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(backup, sizeof(backup), "%sGodModeWii/themes/backup/%08x-v%u-%04d%02d%02d-%02d%02d%02d.csm",
                 g_devs[dev].root, sm.cid, sm.rev, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
                 tm->tm_min, tm->tm_sec);
        if (!nand_copy_checked(cur, backup, cur_len, cur_sha)) {
            free(csm);
            msg(g_title, C_RED, "Couldn't save the installed theme to the SD card. Nothing was changed.");
            return;
        }
    }
    char shown_backup[VFS_PATH_MAX];
    if (sm.missing) snprintf(shown_backup, sizeof(shown_backup), "%s", backup);
    else vfs_shown_path(backup, shown_backup, sizeof(shown_backup));

    /* last word */
    char sz[32];
    ui_fmt_size(sz, sizeof(sz), len);
    snprintf(q, sizeof(q),
             "The theme passed the checks.\n\n"
             "Theme: %.80s (%s)\nMade for: %s%s%s%s\n%s"
             "The current theme is saved as\n%s\n\n"
             "Write it to the NAND now?",
             vfs_basename(path), sz, ci.version, ci.stock ? "  (the original Wii Menu theme)" : "",
             ci.signer ? "  signed by " : "", ci.signer ? ci.signer : "",
             db ? "vWii: WiiConnect24 channels taken out of the 4:3 database.\n\n" : "\n", shown_backup);
    if (!ask("LAST CHANCE", C_RED, q)) {
        free(csm);
        return;
    }

    /* the install: /tmp first, checked, then over the old archive */
    static u32 uid;
    static u16 gid;
    static u8 attr, op, gp, ot;
    bool have_attr = !sm.missing && ISFS_GetAttr(cur, &uid, &gid, &attr, &op, &gp, &ot) >= 0;
    if (!have_attr) {
        attr = 0;
        op = 3;
        gp = 0;
        ot = 0;
    }
    if (!nand_write_checked(tmp, csm, len, sha, attr, op, gp, ot)) {
        ISFS_Delete(tmp);
        free(csm);
        msg(g_title, C_RED,
                   "Writing the theme to the NAND's /tmp failed (or it read back wrong). The installed theme "
                   "wasn't touched.");
        return;
    }
    bool ok = false;
    const char *how = "";
    if (!sm.missing && ISFS_Delete(cur) < 0) {
        ISFS_Delete(tmp);
        how = "Couldn't remove the old archive; it is still in place. Nothing was changed.";
    } else if (ISFS_Rename(tmp, cur) >= 0) {
        u32 l2;
        u8 s2[20];
        ok = nand_sha(cur, &l2, s2, "Checking the installed theme") && l2 == len && !memcmp(s2, sha, 20);
        if (!ok) how = "The installed archive reads back wrong.";
    } else {
        /* the old one is gone and the rename failed: write it in place */
        ok = nand_write_checked(cur, csm, len, sha, attr, op, gp, ot);
        ISFS_Delete(tmp);
        if (!ok) how = "Moving the theme into place failed, and so did writing it there directly.";
    }
    if (have_attr && ok)
        ISFS_SetAttr(cur, uid, gid, attr, op, gp, ot);
    free(csm);
    arcfs_flush();
    vfs_update_space(DEV_NAND);

    char text[VFS_PATH_MAX * 2 + 400];
    char shown_built[VFS_PATH_MAX] = "";
    if (built[0]) vfs_shown_path(built, shown_built, sizeof(shown_built));
    if (ok && sm.missing) {
        msg("THEME INSTALLED", C_GREEN,
            "The theme is installed: the Wii Menu's archive, which was missing from the NAND, is back in "
            "place.");
    } else if (ok) {
        snprintf(text, sizeof(text),
                 "The theme is installed.\n\nThe previous theme is saved as\n%s%s%s\n\n"
                 "If the Wii Menu doesn't start: hold RESET while turning the console on to reach "
                 "Priiloader, start GodModeWii from the Homebrew Channel and install that file.",
                 shown_backup, built[0] ? "\nThe csm built from the mym is\n" : "", shown_built);
        msg("THEME INSTALLED", C_GREEN, text);
    } else if (!strncmp(how, "Couldn't remove", 15)) {
        msg(g_title, C_RED, how);
    } else {
        snprintf(text, sizeof(text),
                 "%s\n\nDON'T TURN THE CONSOLE OFF. Install the saved theme\n%s\nnow (A on it). If the Wii "
                 "Menu doesn't start later, use Priiloader to start the Homebrew Channel.",
                 how, shown_backup);
        msg("THEME INSTALL FAILED", C_RED, text);
    }
}

#ifdef GMW_AUTOTEST
void theme_autotest(const char *cmd, const char *path)
{
    g_auto = true;
    if (!strcmp(cmd, "install")) theme_install(path);
    else theme_convert(path);
    g_auto = false;
}
#endif

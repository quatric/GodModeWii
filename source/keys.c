/*
 * Console keys, laid out the way xyzzy-mod (DarkMatterCore) writes them so
 * its output and wad2bin's keys.txt format are understood as they are.
 * OTP / SEEPROM readers are re-implemented here; the SD key is recognised in
 * IOS memory by hash, like xyzzy-mod does. SD IV, MD5 blanker and the MAC
 * address (which need the System Menu binary / network) are not included.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <ogc/es.h>

#include "keys.h"
#include "otp.h"
#include "seeprom.h"
#include "hashes.h"
#include "runtimeiospatch.h"

#define MAX_FILES 24
#define SD_KEY_SCAN_START 0x93400000u
#define SD_KEY_SCAN_END   0x94000000u
#define DEVCERT_SIZE 0x180

typedef struct {
    char path[40];
    u8 *data;
    u32 size;
} kfile;

static kfile files[MAX_FILES];
static int nfiles;

/* SEEPROM fields */
#define SEEP_NG_KEY_ID  0x08
#define SEEP_NG_SIG     0x0C
#define SEEP_KOREAN_KEY 0x74

static void clear_all(void)
{
    for (int i = 0; i < nfiles; i++) {
        if (files[i].data) {
            memset(files[i].data, 0, files[i].size);     /* don't leave keys lying around */
            free(files[i].data);
        }
    }
    memset(files, 0, sizeof(files));
    nfiles = 0;
}

static void add_file(const char *path, const void *data, u32 size)
{
    if (nfiles >= MAX_FILES)
        return;
    u8 *copy = malloc(size ? size : 1);
    if (!copy)
        return;
    memcpy(copy, data, size);
    snprintf(files[nfiles].path, sizeof(files[nfiles].path), "%s", path);
    files[nfiles].data = copy;
    files[nfiles].size = size;
    nfiles++;
}

static bool all_zero(const u8 *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

/* One key: its own file under keys/ and a line in keys.txt. */
static void add_key(char *txt, size_t txt_sz, const char *name, const void *data, u32 size)
{
    char path[40], line[200];
    snprintf(path, sizeof(path), "keys/%s.bin", name);
    add_file(path, data, size);

    int n = snprintf(line, sizeof(line), "%-16s= ", name);
    for (u32 i = 0; i < size && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02X", ((const u8 *)data)[i]);
    snprintf(line + n, sizeof(line) - n, "\r\n");
    strncat(txt, line, txt_sz - strlen(txt) - 1);
}

/* The SD key lives in the loaded IOS image; recognise it by its hashes. */
static bool find_sd_key(u8 out[16])
{
    static const u8 sha[20] = { 0x10, 0x37, 0xD8, 0x80, 0x10, 0x2F, 0xF0, 0x21, 0xC2, 0x2B,
                                0xA8, 0xF5, 0xDF, 0x53, 0xD7, 0x98, 0xCF, 0x44, 0xDD, 0x0B };
    u8 digest[20];
    for (u32 addr = SD_KEY_SCAN_START; addr + 16 <= SD_KEY_SCAN_END; addr += 4) {
        const u8 *p = (const u8 *)addr;
        if (xxh32(p, 16, 0) != 0xF655F81B)
            continue;
        sha1_digest(p, 16, digest);
        if (!memcmp(digest, sha, 20)) {
            memcpy(out, p, 16);
            return true;
        }
    }
    return false;
}

void keys_refresh(void)
{
    vfs_dev *d = &g_devs[DEV_KEYS];
    d->mounted = false;
    clear_all();

    if (g_is_vwii)         { snprintf(d->note, sizeof(d->note), "not available in vWii"); return; }
    if (g_is_dolphin)      { snprintf(d->note, sizeof(d->note), "not available in Dolphin"); return; }
    if (!AHBPROT_DISABLED) { snprintf(d->note, sizeof(d->note), "needs Homebrew Channel (AHBPROT)"); return; }

    u8 otp[OTP_SIZE];
    if (!otp_read_all(otp)) {
        snprintf(d->note, sizeof(d->note), "OTP read failed");
        return;
    }
    const otp_t *o = (const otp_t *)otp;

    u8 seep[SEEPROM_SIZE];
    bool have_seeprom = seeprom_read(seep);

    char *txt = calloc(1, 2048);
    if (!txt)
        return;

    add_key(txt, 2048, "boot1_hash", o->boot1_hash, sizeof(o->boot1_hash));
    add_key(txt, 2048, "wii_common_key", o->common_key, sizeof(o->common_key));
    add_key(txt, 2048, "console_id", o->ng_id, sizeof(o->ng_id));
    add_key(txt, 2048, "ecc_private_key", o->ng_priv, sizeof(o->ng_priv));
    add_key(txt, 2048, "nand_hmac", o->nand_hmac, sizeof(o->nand_hmac));
    add_key(txt, 2048, "nand_aes_key", o->nand_key, sizeof(o->nand_key));
    add_key(txt, 2048, "prng_key", o->rng_key, sizeof(o->rng_key));

    if (have_seeprom) {
        if (!all_zero(seep + SEEP_NG_KEY_ID, 4))
            add_key(txt, 2048, "ng_key_id", seep + SEEP_NG_KEY_ID, 4);
        add_key(txt, 2048, "ng_signature", seep + SEEP_NG_SIG, 60);
        if (!all_zero(seep + SEEP_KOREAN_KEY, 16))
            add_key(txt, 2048, "wii_korean_key", seep + SEEP_KOREAN_KEY, 16);
    }

    u8 sd_key[16];
    if (find_sd_key(sd_key))
        add_key(txt, 2048, "sd_key", sd_key, sizeof(sd_key));

    add_file("keys.txt", txt, strlen(txt));
    free(txt);

    add_file("otp.bin", otp, OTP_SIZE);
    if (have_seeprom)
        add_file("seeprom.bin", seep, SEEPROM_SIZE);

    /* BootMii keys.bin: banner, OTP, SEEPROM, each padded as BootMii does. */
    if (have_seeprom) {
        u8 *bm = calloc(1, 0x400);
        if (bm) {
            snprintf((char *)bm, 0x100, "BackupMii v1, ConsoleID: %08x\n", (unsigned)(
                     ((u32)o->ng_id[0] << 24) | ((u32)o->ng_id[1] << 16) | ((u32)o->ng_id[2] << 8) | o->ng_id[3]));
            memcpy(bm + 0x100, otp, OTP_SIZE);
            memcpy(bm + 0x200, seep, SEEPROM_SIZE);
            add_file("bootmii_keys.bin", bm, 0x400);
            free(bm);
        }
    }

    u8 *cert = memalign(32, 0x200);
    if (cert) {
        memset(cert, 42, 0x200);
        if (ES_GetDeviceCert(cert) >= 0)
            add_file("device.cert", cert, DEVCERT_SIZE);
        free(cert);
    }

    memset(otp, 0, sizeof(otp));
    memset(seep, 0, sizeof(seep));

    char note[48];
    snprintf(note, sizeof(note), "%08X  read-only", (unsigned)(
             ((u32)o->ng_id[0] << 24) | ((u32)o->ng_id[1] << 16) | ((u32)o->ng_id[2] << 8) | o->ng_id[3]));
    snprintf(d->note, sizeof(d->note), "%s", note);
    d->mounted = nfiles > 0;
}

/* ---------------------------------------------------------------- browsing */

static bool in_dir(const char *path, const char *rel, bool *is_subdir, char *name, size_t name_sz)
{
    size_t rl = strlen(rel);
    const char *rest = path;
    if (rl) {
        if (strncasecmp(path, rel, rl) || path[rl] != '/')
            return false;
        rest = path + rl + 1;
    }
    const char *slash = strchr(rest, '/');
    *is_subdir = slash != NULL;
    size_t l = slash ? (size_t)(slash - rest) : strlen(rest);
    if (l >= name_sz)
        return false;
    memcpy(name, rest, l);
    name[l] = '\0';
    return true;
}

bool keys_list(const char *rel, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;
    if (!g_devs[DEV_KEYS].mounted)
        return false;
    if (*rel && strcasecmp(rel, "keys"))
        return false;

    vfs_entry *l = calloc(nfiles + 1, sizeof(vfs_entry));
    if (!l)
        return false;
    int n = 0;
    for (int i = 0; i < nfiles; i++) {
        bool sub;
        char name[VFS_NAME_MAX];
        if (!in_dir(files[i].path, rel, &sub, name, sizeof(name)))
            continue;
        bool dup = false;
        for (int k = 0; k < n; k++)
            if (!strcasecmp(l[k].name, name)) dup = true;
        if (dup)
            continue;
        snprintf(l[n].name, sizeof(l[n].name), "%s", name);
        l[n].is_dir = sub;
        l[n].size = sub ? 0 : files[i].size;
        n++;
    }
    /* directories first, then by name */
    for (int a = 1; a < n; a++) {
        vfs_entry t = l[a];
        int b = a;
        while (b > 0 && (l[b - 1].is_dir != t.is_dir ? !l[b - 1].is_dir : strcasecmp(l[b - 1].name, t.name) > 0)) {
            l[b] = l[b - 1];
            b--;
        }
        l[b] = t;
    }
    *out = l;
    *count = n;
    return true;
}

static int find_file(const char *rel)
{
    for (int i = 0; i < nfiles; i++)
        if (!strcasecmp(files[i].path, rel))
            return i;
    return -1;
}

bool keys_stat(const char *rel, bool *is_dir, u64 *size)
{
    if (!g_devs[DEV_KEYS].mounted)
        return false;
    if (!*rel || !strcasecmp(rel, "keys")) {
        if (is_dir) *is_dir = true;
        if (size) *size = 0;
        return true;
    }
    int i = find_file(rel);
    if (i < 0)
        return false;
    if (is_dir) *is_dir = false;
    if (size) *size = files[i].size;
    return true;
}

bool keys_open(vfs_file *f, const char *rel)
{
    int i = find_file(rel);
    if (i < 0)
        return false;
    f->size = files[i].size;
    f->base = (u64)i;
    f->pos = 0;
    return true;
}

s32 keys_read(vfs_file *f, void *buf, u32 len)
{
    if (f->pos >= f->size)
        return 0;
    if (len > f->size - f->pos)
        len = (u32)(f->size - f->pos);
    memcpy(buf, files[f->base].data + f->pos, len);
    f->pos += len;
    return (s32)len;
}

void keys_shutdown(void)
{
    clear_all();
}

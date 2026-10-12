#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <errno.h>
#include <ogc/lwp_watchdog.h>
#include <di/di.h>

#include "virt.h"
#include "seeprom.h"
#include "runtimeiospatch.h"

#define WII_MAGIC 0x5D1C9EA3u
#define GC_MAGIC  0xC2339F3Du
#define SZ_GC     1459978240ULL
#define SZ_WII_SL 4699979776ULL
#define SZ_WII_DL 8511160320ULL
#define DI_BLOCK  0x8000u

enum { V_SEEPROM = 1, V_TEXT, V_DISC };

static u8 seeprom_data[SEEPROM_SIZE];
static bool seeprom_ok;

static bool disc_ok;
static u64 disc_size;
static char disc_iso_name[16] = "disc.iso";
static char disc_text[512];
static bool di_started;
static u8 *di_block;
static u64 di_block_off = (u64)-1;

static void set_note(int dev, const char *note)
{
    snprintf(g_devs[dev].note, sizeof(g_devs[dev].note), "%s", note);
}

/* ---------------------------------------------------------------- SEEPROM */

static void refresh_seeprom(void)
{
    vfs_dev *d = &g_devs[DEV_SEEPROM];
    d->mounted = false;
    seeprom_ok = false;
    if (g_is_vwii)      { set_note(DEV_SEEPROM, "not available in vWii"); return; }
    if (g_is_dolphin)   { set_note(DEV_SEEPROM, "not available in Dolphin"); return; }
    if (!AHBPROT_DISABLED) { set_note(DEV_SEEPROM, "needs Homebrew Channel (AHBPROT)"); return; }
    if (!seeprom_read(seeprom_data)) { set_note(DEV_SEEPROM, "read failed"); return; }
    seeprom_ok = true;
    d->mounted = true;
    set_note(DEV_SEEPROM, "read-only");
}

/* ---------------------------------------------------------------- disc */

static bool disc_raw_read(u64 off, void *buf, u32 len)
{
    int ret = DI_UnencryptedRead(buf, len, (u32)(off >> 2));
    return ret == 0;
}

static void refresh_disc(void)
{
    vfs_dev *d = &g_devs[DEV_DISC];
    d->mounted = false;
    disc_ok = false;
    di_block_off = (u64)-1;

    if (g_is_vwii)         { set_note(DEV_DISC, "not available in vWii"); return; }
    if (!AHBPROT_DISABLED) { set_note(DEV_DISC, "needs Homebrew Channel (AHBPROT)"); return; }

    if (!di_started) {
        IosPatch_DIReadLimit(false);
        DI_UseCache(false);
        DI_Init();
        di_started = true;
    }
    if (di_fd < 0)       { set_note(DEV_DISC, "drive unavailable"); return; }

    u32 cover = 0;
    if (DI_GetCoverRegister(&cover) != 0 || !(cover & DVD_COVER_DISC_INSERTED)) {
        set_note(DEV_DISC, "no disc");
        return;
    }
    if (!__io_wiidvd.startup()) { set_note(DEV_DISC, "disc not readable"); return; }

    if (!di_block)
        di_block = memalign(32, DI_BLOCK);
    if (!di_block)
        return;

    if (!disc_raw_read(0, di_block, DI_BLOCK)) {
        DI_Reset();
        set_note(DEV_DISC, "disc not readable");
        return;
    }

    u32 wii = *(u32 *)(di_block + 0x18), gc = *(u32 *)(di_block + 0x1C);
    const char *kind;
    if (wii == WII_MAGIC) {
        kind = "Wii";
        disc_size = SZ_WII_SL;
        /* A dual layer disc also answers beyond the single layer end. */
        if (disc_raw_read(SZ_WII_SL, di_block, DI_BLOCK)) {
            disc_size = SZ_WII_DL;
            disc_raw_read(0, di_block, DI_BLOCK);
        } else {
            DI_Reset();
            disc_raw_read(0, di_block, DI_BLOCK);
        }
        snprintf(disc_iso_name, sizeof(disc_iso_name), "disc.iso");
    } else if (gc == GC_MAGIC) {
        kind = "GameCube";
        disc_size = SZ_GC;
        snprintf(disc_iso_name, sizeof(disc_iso_name), "disc.gcm");
    } else {
        set_note(DEV_DISC, "unsupported disc");
        return;
    }

    char id[7], title[65];
    memcpy(id, di_block, 6); id[6] = '\0';
    memcpy(title, di_block + 0x20, 64); title[64] = '\0';
    snprintf(disc_text, sizeof(disc_text),
             "Type: %s\nGame ID: %s\nTitle: %s\nDisc number: %u  Version: %u\nImage size: %llu bytes\n",
             kind, id, title, di_block[6], di_block[7], (unsigned long long)disc_size);

    disc_ok = true;
    d->mounted = true;
    char note[48];
    snprintf(note, sizeof(note), "%s  %s", id, kind);
    set_note(DEV_DISC, note);
    di_block_off = (u64)-1;
}

/* Any byte offset / length, through one cached 32 KiB block. */
static s32 disc_read_bytes(u64 pos, void *dst, u32 len)
{
    u8 *out = dst;
    u32 done = 0;
    while (done < len) {
        u64 blk = (pos + done) & ~(u64)(DI_BLOCK - 1);
        if (blk >= disc_size)
            break;
        if (blk != di_block_off) {
            int tries = 3;
            while (tries-- && !disc_raw_read(blk, di_block, DI_BLOCK)) {
                if (tries == 0)
                    return -1;
                DI_Reset();
            }
            di_block_off = blk;
        }
        u32 inner = (u32)(pos + done - blk);
        u32 n = DI_BLOCK - inner;
        if (n > len - done) n = len - done;
        memcpy(out + done, di_block + inner, n);
        done += n;
    }
    return (s32)done;
}

/* ---------------------------------------------------------------- public */

void virt_refresh(int dev)
{
    if (dev == DEV_SEEPROM) refresh_seeprom();
    else if (dev == DEV_DISC) refresh_disc();
}

bool virt_list(int dev, vfs_entry **out, int *count)
{
    vfs_entry *l = calloc(2, sizeof(vfs_entry));
    int n = 0;
    if (!l) return false;
    if (dev == DEV_SEEPROM && seeprom_ok) {
        snprintf(l[n].name, sizeof(l[n].name), "seeprom.bin");
        l[n++].size = SEEPROM_SIZE;
    } else if (dev == DEV_DISC && disc_ok) {
        snprintf(l[n].name, sizeof(l[n].name), "%s", disc_iso_name);
        l[n++].size = disc_size;
        snprintf(l[n].name, sizeof(l[n].name), "disc-info.txt");
        l[n++].size = strlen(disc_text);
    }
    *out = l;
    *count = n;
    return true;
}

bool virt_stat(int dev, const char *name, u64 *size)
{
    vfs_entry *l;
    int n;
    if (!virt_list(dev, &l, &n))
        return false;
    bool found = false;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(l[i].name, name)) {
            if (size) *size = l[i].size;
            found = true;
        }
    free(l);
    return found;
}

bool virt_open(vfs_file *f, int dev, const char *name)
{
    u64 size;
    if (!virt_stat(dev, name, &size))
        return false;
    f->dev = dev;
    f->size = size;
    f->pos = 0;
    f->mem = NULL;
    if (dev == DEV_SEEPROM) {
        f->vkind = V_SEEPROM;
    } else if (!strcasecmp(name, "disc-info.txt")) {
        f->vkind = V_TEXT;
    } else {
        f->vkind = V_DISC;
    }
    return true;
}

s32 virt_read(vfs_file *f, void *buf, u32 len)
{
    if (f->pos >= f->size)
        return 0;
    if (len > f->size - f->pos)
        len = (u32)(f->size - f->pos);
    s32 n;
    switch (f->vkind) {
    case V_SEEPROM: memcpy(buf, seeprom_data + f->pos, len); n = len; break;
    case V_TEXT:    memcpy(buf, disc_text + f->pos, len); n = len; break;
    default:        n = disc_read_bytes(f->pos, buf, len); break;
    }
    if (n > 0)
        f->pos += n;
    return n;
}

bool virt_seek(vfs_file *f, u64 off)
{
    f->pos = off > f->size ? f->size : off;
    return true;
}

void virt_close(vfs_file *f)
{
    f->vkind = 0;
}

void virt_shutdown(void)
{
    if (di_started) {
        DI_StopMotor();
        DI_Close();
        di_started = false;
    }
    free(di_block);
    di_block = NULL;
}

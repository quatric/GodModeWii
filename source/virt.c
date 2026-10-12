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
#define DI_BLOCK  0x8000u
#define FST_MAX   (16u * 1024 * 1024)

enum { V_SEEPROM = 1, V_DISC };
enum { DISC_NONE, DISC_GC, DISC_WII };

static u8 seeprom_data[SEEPROM_SIZE];
static bool seeprom_ok;

static int disc_kind;
static bool partition_open;
static bool di_started;
static u8 *di_buf;          /* 32-byte aligned bounce buffer */
static u8 *fst;             /* file system table of the disc */
static u32 fst_count;
static const char *fst_names;
static int fst_shift;       /* Wii stores offsets divided by 4 */

static void set_note(int dev, const char *note)
{
    snprintf(g_devs[dev].note, sizeof(g_devs[dev].note), "%s", note);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
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

/* ---------------------------------------------------------------- disc I/O */

/* off and len must be multiples of 32; buf 32-byte aligned. In the Wii data
 * partition IOS decrypts for us; on a GameCube disc this is a raw read. */
static bool raw_read(u64 off, void *buf, u32 len, bool in_partition)
{
    int ret = in_partition ? DI_Read(buf, len, (u32)(off >> 2))
                           : DI_UnencryptedRead(buf, len, (u32)(off >> 2));
    return ret == 0;
}

static bool read_retry(u64 off, void *buf, u32 len, bool in_partition)
{
    for (int tries = 0; tries < 3; tries++) {
        if (raw_read(off, buf, len, in_partition))
            return true;
        DI_Reset();
    }
    return false;
}

/* Any byte range of the (decrypted) disc file area. */
static s32 disc_read_bytes(u64 pos, void *dst, u32 len)
{
    u8 *out = dst;
    u32 done = 0;
    bool part = disc_kind == DISC_WII;
    while (done < len) {
        u64 at = pos + done;
        u64 start = at & ~(u64)31;
        u32 skew = (u32)(at - start);
        u32 want = len - done;
        u32 chunk = skew + want;
        if (chunk > DI_BLOCK) chunk = DI_BLOCK;
        u32 rd = (chunk + 31) & ~31u;
        if (!read_retry(start, di_buf, rd, part))
            return done ? (s32)done : -1;
        u32 n = chunk - skew;
        if (n > want) n = want;
        memcpy(out + done, di_buf + skew, n);
        done += n;
    }
    return (s32)done;
}

static void disc_forget(void)
{
    if (partition_open) {
        DI_ClosePartition();
        partition_open = false;
    }
    free(fst);
    fst = NULL;
    fst_count = 0;
    fst_names = NULL;
    disc_kind = DISC_NONE;
}

static bool load_fst(u64 fst_off, u32 fst_size)
{
    if (fst_size < 12 || fst_size > FST_MAX)
        return false;
    u32 alloc = (fst_size + 31) & ~31u;
    fst = memalign(32, alloc);
    if (!fst)
        return false;
    u32 done = 0;
    while (done < alloc) {
        u32 n = alloc - done > DI_BLOCK ? DI_BLOCK : alloc - done;
        if (!read_retry(fst_off + done, fst + done, n, disc_kind == DISC_WII))
            return false;
        done += n;
    }
    fst_count = be32(fst + 8);
    if (fst_count < 1 || fst_count * 12 > fst_size)
        return false;
    fst_names = (const char *)fst + fst_count * 12;
    return true;
}

static void refresh_disc(void)
{
    vfs_dev *d = &g_devs[DEV_DISC];
    d->mounted = false;
    disc_forget();

    if (g_is_vwii)         { set_note(DEV_DISC, "not available in vWii"); return; }
    if (!AHBPROT_DISABLED) { set_note(DEV_DISC, "needs Homebrew Channel (AHBPROT)"); return; }

    if (!di_started) {
        IosPatch_DIReadLimit(false);
        DI_UseCache(false);
        DI_Init();
        di_started = true;
    }
    if (di_fd < 0) { set_note(DEV_DISC, "drive unavailable"); return; }

    u32 cover = 0;
    if (DI_GetCoverRegister(&cover) != 0 || !(cover & DVD_COVER_DISC_INSERTED)) {
        set_note(DEV_DISC, "no disc");
        return;
    }
    if (!__io_wiidvd.startup()) { set_note(DEV_DISC, "disc not readable"); return; }

    if (!di_buf)
        di_buf = memalign(32, DI_BLOCK);
    if (!di_buf)
        return;

    if (!read_retry(0, di_buf, 0x440, false)) {
        set_note(DEV_DISC, "disc not readable");
        return;
    }

    char id[7];
    memcpy(id, di_buf, 6);
    id[6] = '\0';
    u32 wii = be32(di_buf + 0x18), gc = be32(di_buf + 0x1C);
    u64 fst_off;
    u32 fst_size;
    const char *kind;

    if (wii == WII_MAGIC) {
        kind = "Wii";
        /* Partition table -> first data partition (type 0). */
        u8 *pt = di_buf;
        if (!read_retry(0x40000, pt, 0x20, false)) { set_note(DEV_DISC, "disc not readable"); return; }
        u32 nparts = be32(pt);
        u64 tbl = (u64)be32(pt + 4) << 2;
        if (nparts == 0 || nparts > 4) { set_note(DEV_DISC, "no data partition"); return; }
        if (!read_retry(tbl, pt, 0x20, false)) { set_note(DEV_DISC, "disc not readable"); return; }
        u32 raw_off = 0;
        bool found = false;
        for (u32 i = 0; i < nparts; i++)
            if (be32(pt + i * 8 + 4) == 0) {
                raw_off = be32(pt + i * 8);
                found = true;
                break;
            }
        if (!found) { set_note(DEV_DISC, "no data partition"); return; }
        if (DI_OpenPartition(raw_off) != 0) {
            DI_Reset();
            if (DI_OpenPartition(raw_off) != 0) { set_note(DEV_DISC, "partition open failed"); return; }
        }
        partition_open = true;
        disc_kind = DISC_WII;
        fst_shift = 2;
    } else if (gc == GC_MAGIC) {
        kind = "GameCube";
        disc_kind = DISC_GC;
        fst_shift = 0;
    } else {
        set_note(DEV_DISC, "unsupported disc");
        return;
    }

    /* boot.bin: FST offset / size live at 0x424 / 0x428. */
    if (!read_retry(0, di_buf, 0x440, disc_kind == DISC_WII)) {
        disc_forget();
        set_note(DEV_DISC, "disc not readable");
        return;
    }
    fst_off = (u64)be32(di_buf + 0x424) << fst_shift;
    fst_size = be32(di_buf + 0x428) << fst_shift;
    if (!load_fst(fst_off, fst_size)) {
        disc_forget();
        set_note(DEV_DISC, "cannot read file table");
        return;
    }

    d->mounted = true;
    char note[48];
    snprintf(note, sizeof(note), "%s  %s  read-only", id, kind);
    set_note(DEV_DISC, note);
}

/* ---------------------------------------------------------------- FST walk */

static bool fst_is_dir(u32 i) { return fst[i * 12] != 0; }
static u32  fst_name_off(u32 i) { return be32(fst + i * 12) & 0x00FFFFFF; }
static u32  fst_off(u32 i) { return be32(fst + i * 12 + 4); }
static u32  fst_len(u32 i) { return be32(fst + i * 12 + 8); }
static const char *fst_name(u32 i) { return fst_names + fst_name_off(i); }

/* Resolves "a/b/c" to an FST index; 0 is the root. */
static bool fst_find(const char *rel, u32 *index)
{
    u32 cur = 0;
    char part[VFS_NAME_MAX];
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t l = strcspn(p, "/");
        if (l >= sizeof(part)) return false;
        memcpy(part, p, l);
        part[l] = '\0';
        p += l;
        if (!fst_is_dir(cur))
            return false;
        u32 end = cur == 0 ? fst_count : fst_len(cur);
        bool hit = false;
        for (u32 i = cur + 1; i < end;) {
            if (!strcasecmp(fst_name(i), part)) {
                cur = i;
                hit = true;
                break;
            }
            i = fst_is_dir(i) ? fst_len(i) : i + 1;
        }
        if (!hit)
            return false;
    }
    *index = cur;
    return true;
}

/* ---------------------------------------------------------------- public */

void virt_refresh(int dev)
{
    if (dev == DEV_SEEPROM) refresh_seeprom();
    else if (dev == DEV_DISC) refresh_disc();
}

bool virt_list(int dev, const char *rel, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;

    if (dev == DEV_SEEPROM) {
        if (!seeprom_ok || *rel)
            return seeprom_ok && !*rel;
        vfs_entry *l = calloc(1, sizeof(vfs_entry));
        if (!l) return false;
        snprintf(l[0].name, sizeof(l[0].name), "seeprom.bin");
        l[0].size = SEEPROM_SIZE;
        *out = l;
        *count = 1;
        return true;
    }

    u32 dir;
    if (!g_devs[DEV_DISC].mounted || !fst || !fst_find(rel, &dir) || !fst_is_dir(dir))
        return false;
    u32 end = dir == 0 ? fst_count : fst_len(dir);
    u32 n = 0;
    for (u32 i = dir + 1; i < end; i = fst_is_dir(i) ? fst_len(i) : i + 1)
        n++;
    vfs_entry *l = calloc(n ? n : 1, sizeof(vfs_entry));
    if (!l) return false;
    u32 k = 0;
    for (u32 i = dir + 1; i < end && k < n; i = fst_is_dir(i) ? fst_len(i) : i + 1) {
        snprintf(l[k].name, sizeof(l[k].name), "%s", fst_name(i));
        l[k].is_dir = fst_is_dir(i);
        l[k].size = l[k].is_dir ? 0 : (u64)fst_len(i);
        k++;
    }
    /* directories first, then by name, like every other drive */
    for (u32 a = 1; a < k; a++) {
        vfs_entry t = l[a];
        u32 b = a;
        while (b > 0 && (l[b - 1].is_dir != t.is_dir ? !l[b - 1].is_dir : strcasecmp(l[b - 1].name, t.name) > 0)) {
            l[b] = l[b - 1];
            b--;
        }
        l[b] = t;
    }
    *out = l;
    *count = k;
    return true;
}

bool virt_stat(int dev, const char *rel, bool *is_dir, u64 *size)
{
    if (dev == DEV_SEEPROM) {
        if (!seeprom_ok) return false;
        if (!*rel) { if (is_dir) *is_dir = true; if (size) *size = 0; return true; }
        if (strcasecmp(rel, "seeprom.bin")) return false;
        if (is_dir) *is_dir = false;
        if (size) *size = SEEPROM_SIZE;
        return true;
    }
    u32 i;
    if (!g_devs[DEV_DISC].mounted || !fst || !fst_find(rel, &i))
        return false;
    if (is_dir) *is_dir = fst_is_dir(i);
    if (size) *size = fst_is_dir(i) ? 0 : (u64)fst_len(i);
    return true;
}

bool virt_open(vfs_file *f, int dev, const char *rel)
{
    bool is_dir;
    u64 size;
    if (!virt_stat(dev, rel, &is_dir, &size) || is_dir)
        return false;
    f->dev = dev;
    f->size = size;
    f->pos = 0;
    if (dev == DEV_SEEPROM) {
        f->vkind = V_SEEPROM;
    } else {
        u32 i;
        fst_find(rel, &i);
        f->vkind = V_DISC;
        f->base = (u64)fst_off(i) << fst_shift;
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
    if (f->vkind == V_SEEPROM) {
        memcpy(buf, seeprom_data + f->pos, len);
        n = len;
    } else {
        n = disc_read_bytes(f->base + f->pos, buf, len);
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
    disc_forget();
    if (di_started) {
        DI_StopMotor();
        DI_Close();
        di_started = false;
    }
    free(di_buf);
    di_buf = NULL;
}

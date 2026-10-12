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
#include "keys.h"
#include "mcard.h"
#include "runtimeiospatch.h"

#define WII_MAGIC 0x5D1C9EA3u
#define GC_MAGIC  0xC2339F3Du
#define DI_BLOCK  0x8000u
#define FST_MAX   (16u * 1024 * 1024)

enum { V_SEEPROM = 1, V_DISC, V_KEYS, V_MCARD };
enum { DISC_NONE, DISC_GC, DISC_WII, DISC_RAW };

#define GC_DISC_SIZE 1459978240ULL
#define SECTOR 2048u
#define ERR_MAX 100000

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
static u32 part_raw_off;    /* Wii data partition, to reopen it after a drive reset */

/* Parts of the disc that are not in the file table, shown as sys/ and disc.iso */
typedef struct { const char *name; u64 off; u64 size; } sysfile;
static sysfile sysf[5];
static int sys_n;
static bool iso_ok;

/* Read errors seen while a file is being copied (see virt_disc_tolerate) */
typedef struct { u64 disc_off, len; } errrange;
static bool tol_on;
static bool (*tol_cancel)(void);
static errrange *errs;
static u32 err_n, err_cap;
static u64 err_bytes, err_file_base;
static bool err_overflow;
static u32 consec_fail;

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

static void drive_reset(void)
{
    DI_Reset();
    if (partition_open && DI_OpenPartition(part_raw_off) != 0) {
        DI_Reset();
        DI_OpenPartition(part_raw_off);
    }
}

static bool read_retry(u64 off, void *buf, u32 len, bool in_partition)
{
    for (int tries = 0; tries < 3; tries++) {
        if (raw_read(off, buf, len, in_partition))
            return true;
        drive_reset();
    }
    return false;
}

/* ---- tolerant reads: unreadable sectors become zeros and are written down ---- */

static void err_add(u64 disc_off, u64 len)
{
    err_bytes += len;
    if (err_n && errs[err_n - 1].disc_off + errs[err_n - 1].len == disc_off) {
        errs[err_n - 1].len += len;
        return;
    }
    if (err_n >= ERR_MAX) {                 /* absurdly damaged: keep counting in the last range */
        errs[err_n - 1].len = disc_off + len - errs[err_n - 1].disc_off;
        err_overflow = true;
        return;
    }
    if (err_n == err_cap) {
        u32 nc = err_cap ? err_cap * 2 : 256;
        errrange *ne = realloc(errs, nc * sizeof(errrange));
        if (!ne) {
            if (err_n) { errs[err_n - 1].len = disc_off + len - errs[err_n - 1].disc_off; err_overflow = true; }
            return;
        }
        errs = ne;
        err_cap = nc;
    }
    errs[err_n].disc_off = disc_off;
    errs[err_n].len = len;
    err_n++;
}

void virt_disc_tolerate(bool on, bool (*cancel)(void))
{
    tol_on = on;
    tol_cancel = on ? cancel : NULL;
    err_n = 0;
    err_bytes = 0;
    err_overflow = false;
    consec_fail = 0;
    if (!on) {
        free(errs);
        errs = NULL;
        err_cap = 0;
    }
}

u32 virt_disc_error_count(void) { return err_n; }
u64 virt_disc_error_bytes(void) { return err_bytes; }
bool virt_disc_error_overflow(void) { return err_overflow; }

bool virt_disc_error_get(u32 i, u64 *file_off, u64 *disc_off, u64 *len)
{
    if (i >= err_n) return false;
    if (file_off) *file_off = errs[i].disc_off - err_file_base;
    if (disc_off) *disc_off = errs[i].disc_off;
    if (len) *len = errs[i].len;
    return true;
}

static s32 disc_read_tolerant(u64 pos, u8 *out, u32 len)
{
    bool part = disc_kind == DISC_WII;
    u32 done = 0;
    while (done < len) {
        if (tol_cancel && tol_cancel())
            return done ? (s32)done : -1;
        u64 at = pos + done;
        u64 start = at & ~(u64)(SECTOR - 1);
        u32 skew = (u32)(at - start);
        u32 want = len - done;
        u32 chunk = skew + want;
        if (chunk > DI_BLOCK) chunk = DI_BLOCK;
        u32 rd = (chunk + SECTOR - 1) & ~(SECTOR - 1);
        u32 n = chunk - skew;
        if (n > want) n = want;
        if (raw_read(start, di_buf, rd, part)) {
            memcpy(out + done, di_buf + skew, n);
            consec_fail = 0;
            done += n;
            continue;
        }
        /* something in this block is bad: find out which sectors */
        for (u32 sec = 0; sec < rd; sec += SECTOR) {
            u64 sa = start + sec;
            u64 lo = sa > at ? sa : at, hi = sa + SECTOR < at + n ? sa + SECTOR : at + n;
            if (hi <= lo) continue;
            bool ok = false;
            int tries = consec_fail > 8 ? 1 : 2;
            for (int t = 0; t < tries && !ok; t++) {
                ok = raw_read(sa, di_buf + sec, SECTOR, part);
                if (!ok && (consec_fail < 8 || (consec_fail & 15) == 0))
                    drive_reset();
            }
            if (ok) {
                consec_fail = 0;
                memcpy(out + done + (lo - at), di_buf + sec + (lo - sa), hi - lo);
            } else {
                consec_fail++;
                memset(out + done + (lo - at), 0, hi - lo);
                err_add(lo, hi - lo);
            }
        }
        done += n;
    }
    return (s32)done;
}

/* Any byte range of the (decrypted) disc file area. */
static s32 disc_read_bytes(u64 pos, void *dst, u32 len)
{
    u8 *out = dst;
    if (tol_on)
        return disc_read_tolerant(pos, out, len);
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
    sys_n = 0;
    iso_ok = false;
    virt_disc_tolerate(false, NULL);
}

static void sys_add(const char *name, u64 off, u64 size)
{
    if (sys_n < 5 && size)
        sysf[sys_n++] = (sysfile){ name, off, size };
}

/* The parts of the disc that are not files: header, apploader, file table and main.dol. */
static void sys_setup(const u8 *boot)
{
    sys_n = 0;
    bool wii = disc_kind == DISC_WII;
    sys_add("boot.bin", 0, 0x440);
    sys_add("bi2.bin", 0x440, 0x2000);

    u8 *tmp = memalign(32, 0x100);
    if (!tmp) return;
    if (read_retry(0x2440, tmp, 0x20, wii)) {
        u64 sz = 0x20 + (u64)be32(tmp + 0x14) + be32(tmp + 0x18);
        if (be32(tmp + 0x14) && sz < 16u * 1024 * 1024)
            sys_add("apploader.img", 0x2440, sz);
    }
    u64 fst_o = (u64)be32(boot + 0x424) << fst_shift;
    u32 fst_s = be32(boot + 0x428) << fst_shift;
    if (fst_s >= 12 && fst_s <= FST_MAX && fst_o)
        sys_add("fst.bin", fst_o, fst_s);
    u64 dol_o = (u64)be32(boot + 0x420) << fst_shift;
    if (dol_o && read_retry(dol_o, tmp, 0x100, wii)) {
        u64 end = 0x100;
        for (int i = 0; i < 18; i++) {
            u32 off = be32(tmp + i * 4), len = be32(tmp + 0x90 + i * 4);
            if (off && len && (u64)off + len > end) end = (u64)off + len;
        }
        if (end < 64u * 1024 * 1024)
            sys_add("main.dol", dol_o, end);
    }
    free(tmp);
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
        if (!g_is_dolphin)
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
    for (int i = 0; i < 6; i++)
        if (id[i] < 0x20 || id[i] > 0x7E) id[i] = '?';
    id[6] = '\0';
    u32 wii = be32(di_buf + 0x18), gc = be32(di_buf + 0x1C);
    const char *kind;
    bool want_fst = true;

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
        part_raw_off = raw_off;
        partition_open = true;
        disc_kind = DISC_WII;
        fst_shift = 2;
    } else if (gc == GC_MAGIC) {
        kind = "GameCube";
        disc_kind = DISC_GC;
        fst_shift = 0;
    } else {
        /* Not a Nintendo disc as far as the header goes (homebrew, hacked or damaged
         * discs): the drive still hands out its sectors, so offer them as an image. */
        kind = "raw disc";
        disc_kind = DISC_RAW;
        fst_shift = 0;
        want_fst = false;
    }

    u8 boot[0x440];
    bool boot_ok = read_retry(0, di_buf, 0x440, disc_kind == DISC_WII);
    if (boot_ok)
        memcpy(boot, di_buf, sizeof(boot));
    else if (disc_kind == DISC_WII) {
        disc_forget();
        set_note(DEV_DISC, "disc not readable");
        return;
    }

    bool have_fst = false;
    if (want_fst && boot_ok) {
        u64 fst_o = (u64)be32(boot + 0x424) << fst_shift;
        u32 fst_s = be32(boot + 0x428) << fst_shift;
        have_fst = load_fst(fst_o, fst_s);
        if (!have_fst) {
            free(fst);
            fst = NULL;
            fst_count = 0;
        }
        if (disc_kind == DISC_WII && !have_fst) {
            disc_forget();
            set_note(DEV_DISC, "cannot read file table");
            return;
        }
        sys_setup(boot);
    }
    iso_ok = disc_kind != DISC_WII;

    d->mounted = true;
    char note[48];
    if (disc_kind == DISC_RAW)
        snprintf(note, sizeof(note), "unrecognised disc - raw image only");
    else if (!have_fst)
        snprintf(note, sizeof(note), "%s  %s  no file table", id, kind);
    else
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

/* sys/ and disc.iso are made up: they are not in the file table */
static void norm_rel(const char *rel, char *out, size_t sz)
{
    while (*rel == '/') rel++;
    snprintf(out, sz, "%s", rel);
    size_t l = strlen(out);
    while (l && out[l - 1] == '/') out[--l] = '\0';
}

static bool fst_has(const char *name)
{
    u32 i;
    return fst && fst_find(name, &i);
}

static bool want_sys_dir(void) { return sys_n > 0 && !fst_has("sys"); }
static bool want_iso(void) { return iso_ok && !fst_has("disc.iso"); }

static const sysfile *sys_lookup(const char *norm)
{
    if (strncasecmp(norm, "sys/", 4) || !want_sys_dir()) return NULL;
    for (int i = 0; i < sys_n; i++)
        if (!strcasecmp(norm + 4, sysf[i].name))
            return &sysf[i];
    return NULL;
}

static u64 iso_size(void) { return GC_DISC_SIZE; }

/* ---------------------------------------------------------------- public */

void virt_refresh(int dev)
{
    if (dev == DEV_SEEPROM) refresh_seeprom();
    else if (dev == DEV_DISC) refresh_disc();
    else if (dev == DEV_MCA || dev == DEV_MCB) mcard_refresh(dev);
    else if (dev == DEV_KEYS) keys_refresh();
}

bool virt_list(int dev, const char *rel, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;

    if (dev == DEV_KEYS)
        return keys_list(rel, out, count);
    if (dev == DEV_MCA || dev == DEV_MCB)
        return mcard_list(dev, rel, out, count);

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

    if (!g_devs[DEV_DISC].mounted)
        return false;
    char norm[VFS_PATH_MAX];
    norm_rel(rel, norm, sizeof(norm));
    if (!strcasecmp(norm, "sys") && want_sys_dir()) {
        vfs_entry *sl = calloc(sys_n, sizeof(vfs_entry));
        if (!sl) return false;
        for (int i = 0; i < sys_n; i++) {
            snprintf(sl[i].name, sizeof(sl[i].name), "%s", sysf[i].name);
            sl[i].size = sysf[i].size;
        }
        *out = sl;
        *count = sys_n;
        return true;
    }
    u32 dir = 0;
    bool root = !*norm;
    if (!root && (!fst || !fst_find(rel, &dir) || !fst_is_dir(dir)))
        return false;
    if (root && !fst && !want_sys_dir() && !want_iso())
        return false;
    u32 end = !fst ? 0 : (dir == 0 ? fst_count : fst_len(dir));
    u32 n = 0;
    for (u32 i = dir + 1; fst && i < end; i = fst_is_dir(i) ? fst_len(i) : i + 1)
        n++;
    u32 extra = root ? (want_sys_dir() ? 1 : 0) + (want_iso() ? 1 : 0) : 0;
    vfs_entry *l = calloc(n + extra ? n + extra : 1, sizeof(vfs_entry));
    if (!l) return false;
    u32 k = 0;
    for (u32 i = dir + 1; fst && i < end && k < n; i = fst_is_dir(i) ? fst_len(i) : i + 1) {
        snprintf(l[k].name, sizeof(l[k].name), "%s", fst_name(i));
        l[k].is_dir = fst_is_dir(i);
        l[k].size = l[k].is_dir ? 0 : (u64)fst_len(i);
        k++;
    }
    if (root && want_sys_dir()) {
        snprintf(l[k].name, sizeof(l[k].name), "sys");
        l[k++].is_dir = true;
    }
    if (root && want_iso()) {
        snprintf(l[k].name, sizeof(l[k].name), "disc.iso");
        l[k++].size = iso_size();
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
    if (dev == DEV_KEYS)
        return keys_stat(rel, is_dir, size);
    if (dev == DEV_MCA || dev == DEV_MCB)
        return mcard_stat(dev, rel, is_dir, size);
    if (dev == DEV_SEEPROM) {
        if (!seeprom_ok) return false;
        if (!*rel) { if (is_dir) *is_dir = true; if (size) *size = 0; return true; }
        if (strcasecmp(rel, "seeprom.bin")) return false;
        if (is_dir) *is_dir = false;
        if (size) *size = SEEPROM_SIZE;
        return true;
    }
    if (!g_devs[DEV_DISC].mounted)
        return false;
    char norm[VFS_PATH_MAX];
    norm_rel(rel, norm, sizeof(norm));
    if (!*norm) { if (is_dir) *is_dir = true; if (size) *size = 0; return true; }
    u32 i;
    if (fst && fst_find(rel, &i)) {
        if (is_dir) *is_dir = fst_is_dir(i);
        if (size) *size = fst_is_dir(i) ? 0 : (u64)fst_len(i);
        return true;
    }
    if (!strcasecmp(norm, "sys") && want_sys_dir()) { if (is_dir) *is_dir = true; if (size) *size = 0; return true; }
    if (!strcasecmp(norm, "disc.iso") && want_iso()) { if (is_dir) *is_dir = false; if (size) *size = iso_size(); return true; }
    const sysfile *sf = sys_lookup(norm);
    if (!sf) return false;
    if (is_dir) *is_dir = false;
    if (size) *size = sf->size;
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
    if (dev == DEV_MCA || dev == DEV_MCB) {
        f->vkind = V_MCARD;
        return mcard_open(f, dev, rel);
    }
    if (dev == DEV_KEYS) {
        f->vkind = V_KEYS;
        return keys_open(f, rel);
    }
    if (dev == DEV_SEEPROM) {
        f->vkind = V_SEEPROM;
    } else {
        u32 i;
        char norm[VFS_PATH_MAX];
        norm_rel(rel, norm, sizeof(norm));
        f->vkind = V_DISC;
        if (fst && fst_find(rel, &i)) {
            f->base = (u64)fst_off(i) << fst_shift;
        } else if (!strcasecmp(norm, "disc.iso")) {
            f->base = 0;
        } else {
            const sysfile *sf = sys_lookup(norm);
            if (!sf) return false;
            f->base = sf->off;
        }
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
    if (f->vkind == V_MCARD) {
        return mcard_read(f, buf, len);
    } else if (f->vkind == V_KEYS) {
        return keys_read(f, buf, len);
    } else if (f->vkind == V_SEEPROM) {
        memcpy(buf, seeprom_data + f->pos, len);
        n = len;
    } else {
        err_file_base = f->base;
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
    keys_shutdown();
    mcard_shutdown();
    disc_forget();
    if (di_started) {
        DI_StopMotor();
        DI_Close();
        di_started = false;
    }
    free(di_buf);
    di_buf = NULL;
}

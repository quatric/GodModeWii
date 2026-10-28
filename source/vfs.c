#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <malloc.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <sdcard/gcsd.h>
#include <ogc/usb.h>
#include <ogc/usbstorage.h>
#include <ogc/lwp_heap.h>
#include <ogc/isfs.h>

#include "vfs.h"
#include "virt.h"
#include "arcfs.h"
#include "mcard.h"

vfs_dev g_devs[DEV_COUNT] = {
    { "sd:",   "sd:/",   "SD Card",           "SD",   true,  false, false, 0, 0 },
    { "gca:",  "gca:/",  "SD Gecko (Slot A)", "GCA",  false, false, false, 0, 0 },
    { "gcb:",  "gcb:/",  "SD Gecko (Slot B)", "GCB",  false, false, false, 0, 0 },
    { "usb1:", "usb1:/", "USB Drive 1",       "USB1", false, false, false, 0, 0 },
    { "usb2:", "usb2:/", "USB Drive 2",       "USB2", false, false, false, 0, 0 },
    { "usb3:", "usb3:/", "USB Drive 3",       "USB3", false, false, false, 0, 0 },
    { "usb4:", "usb4:/", "USB Drive 4",       "USB4", false, false, false, 0, 0 },
    { "usb5:", "usb5:/", "USB Drive 5",       "USB5", false, false, false, 0, 0 },
    { "usb6:", "usb6:/", "USB Drive 6",       "USB6", false, false, false, 0, 0 },
    { "usb7:", "usb7:/", "USB Drive 7",       "USB7", false, false, false, 0, 0 },
    { "usb8:", "usb8:/", "USB Drive 8",       "USB8", false, false, false, 0, 0 },
    { "nand:", "nand:/", "Wii NAND",          "NAND", true,  false, false, 0, 0 },
    { "seeprom:", "seeprom:/", "SEEPROM",     "SEEP", true,  false, false, 0, 0 },
    { "disc:", "disc:/", "Disc Drive",        "DISC", true,  false, false, 0, 0 },
    { "mca:",  "mca:/",  "Memory Card A",     "MCA",  false, false, false, 0, 0 },
    { "mcb:",  "mcb:/",  "Memory Card B",     "MCB",  false, false, false, 0, 0 },
    { "keys:", "keys:/", "Console Keys",      "KEYS", true,  false, false, 0, 0 },
};

bool g_is_vwii, g_is_dolphin, g_nand_full;
const char *g_write_err;
static bool nand_ready;

/* ---------------------------------------------------------------- paths */

int vfs_dev_of(const char *path)
{
    for (int i = 0; i < DEV_COUNT; i++)
        if (strncmp(path, g_devs[i].prefix, strlen(g_devs[i].prefix)) == 0)
            return i;
    return -1;
}

bool vfs_is_virtual(int dev)
{
    return arcfs_dev_pinned(dev) || dev == DEV_SEEPROM || dev == DEV_DISC || dev == DEV_MCA || dev == DEV_MCB || dev == DEV_KEYS;
}

bool vfs_is_card(int dev)
{
    return dev == DEV_MCA || dev == DEV_MCB;
}

bool vfs_is_archive(const char *path)
{
    return arcfs_is_path(path);
}

bool vfs_is_readonly(const char *path)
{
    if (arcfs_is_path(path))
        return true;
    int d = vfs_dev_of(path);
    return d >= 0 && vfs_is_virtual(d) && !vfs_is_card(d);
}

/* "seeprom:/name" -> "name" ("" for the root) */
static const char *virt_name(const char *path)
{
    const char *s = strchr(path, '/');
    return s ? s + 1 : "";
}

const char *vfs_dev_root(int dev)
{
    return g_devs[dev].root;
}

bool vfs_is_root(const char *path)
{
    int d = vfs_dev_of(path);
    return d >= 0 && strcmp(path, vfs_dev_root(d)) == 0;
}

bool vfs_parent(const char *path, char *out, size_t outsz)
{
    int d = vfs_dev_of(path);
    if (d < 0 || vfs_is_root(path))
        return false;
    snprintf(out, outsz, "%s", path);
    char *bar = strrchr(out, '|');
    if (bar) {
        char *rest = bar + 1;
        while (*rest == '/') rest++;
        if (!*rest) {                       /* top of an archive: back to the folder holding it */
            char tmp[VFS_PATH_MAX * 2];
            *bar = '\0';
            snprintf(tmp, sizeof(tmp), "%s", out);
            return vfs_parent(tmp, out, outsz);
        }
        char *sl = strrchr(rest, '/');
        if (sl && sl[1]) { *sl = '\0'; return true; }
        if (sl) {                           /* trailing slash */
            *sl = '\0';
            sl = strrchr(rest, '/');
            if (sl) { *sl = '\0'; return true; }
        }
        bar[1] = '/'; bar[2] = '\0';
        return true;
    }
    char *slash = strrchr(out, '/');
    if (!slash)
        return false;
    if (slash == strchr(out, '/'))      /* "sd:/name" -> "sd:/" */
        slash[1] = '\0';
    else
        *slash = '\0';
    return true;
}

const char *vfs_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return (s && s[1]) ? s + 1 : path;
}

bool vfs_join(char *out, size_t outsz, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    int n = snprintf(out, outsz, "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", name);
    return n > 0 && (size_t)n < outsz;
}

bool vfs_is_within(const char *path, const char *dir)
{
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/' && dir[dl - 2] != ':')
        dl--;
    if (strncasecmp(path, dir, dl) != 0)
        return false;
    return path[dl] == '\0' || path[dl] == '/' || dir[dl - 1] == '/';
}

/* "nand:/title" -> "/title" */
static const char *nand_path(const char *path)
{
    return path + 5;
}

/* ---------------------------------------------------------------- USB */

/* libogc's __io_usbstorage only exposes the first drive. To use every USB
 * port (and every LUN of card readers) each drive gets its own handle and
 * its own DISC_INTERFACE, which libfat then mounts as usb1: .. usb4: */

#define USB_CLASS_MASS_STORAGE 0x08
#define USB_IO_MAX_BYTES (64 * 1024)

typedef struct {
    usbstorage_handle *h;
    u8 lun;
    u32 sector_size;
    u32 sectors;
    bool ok;
    bool hidden;    /* USBHide / ustealth: boot signature 55 AB instead of 55 AA */
} usb_slot;

static usb_slot uslot[USB_SLOTS];
static usbstorage_handle uhandle[USB_SLOTS];
static int uhandles_open;
static bool usb_inited;

/* libogc's USB storage code carves one 16 KiB transfer buffer per opened drive out
 * of a private 18 KiB heap, so only the first drive can ever be opened (the
 * second fails with -22, IPC_ENOMEM). The Makefile wraps __lwp_heap_allocate /
 * __lwp_heap_free so that, while a drive is being opened, that buffer comes from
 * this pool of MEM2 blocks instead. */
#define USB_XFER_BUF 16384
static u8 *ubuf_pool[USB_SLOTS];
static bool ubuf_used[USB_SLOTS];
static bool ubuf_active;
void *__real___lwp_heap_allocate(heap_cntrl *heap, u32 size);
BOOL __real___lwp_heap_free(heap_cntrl *heap, void *ptr);

void *__wrap___lwp_heap_allocate(heap_cntrl *heap, u32 size)
{
    if (ubuf_active && size == USB_XFER_BUF) {
        for (int i = 0; i < USB_SLOTS; i++) {
            if (ubuf_used[i])
                continue;
            if (!ubuf_pool[i]) {
                u32 hi = (u32)SYS_GetArena2Hi() & ~31u;
                u32 blk = hi - USB_XFER_BUF;
                if (blk < (u32)SYS_GetArena2Lo())
                    break;
                SYS_SetArena2Hi((void *)blk);
                ubuf_pool[i] = (u8 *)blk;
            }
            ubuf_used[i] = true;
            return ubuf_pool[i];
        }
    }
    return __real___lwp_heap_allocate(heap, size);
}

BOOL __wrap___lwp_heap_free(heap_cntrl *heap, void *ptr)
{
    for (int i = 0; i < USB_SLOTS; i++)
        if (ptr && ptr == ubuf_pool[i]) {
            ubuf_used[i] = false;
            return TRUE;
        }
    return __real___lwp_heap_free(heap, ptr);
}
static int usb_seen;            /* mass-storage devices IOS listed */
static char usb_diag[48];       /* why a device could not be used */
static u8 *usb_bounce;

static bool us_rw(int i, sec_t sector, sec_t n, void *rbuf, const void *wbuf)
{
    usb_slot *s = &uslot[i];
    if (!s->ok || !s->sector_size)
        return false;
    u32 per = USB_IO_MAX_BYTES / s->sector_size;
    if (per == 0) per = 1;
    if (!usb_bounce)
        usb_bounce = memalign(32, USB_IO_MAX_BYTES);
    if (!usb_bounce)
        return false;

    u8 *rp = rbuf;
    const u8 *wp = wbuf;
    while (n) {
        u32 cnt = n < per ? n : per;
        u32 bytes = cnt * s->sector_size;
        s32 r;
        if (rbuf) {
            r = USBStorage_Read(s->h, s->lun, sector, cnt, usb_bounce);
            if (r < 0) return false;
            if (sector == 0 && s->hidden && usb_bounce[510] == 0x55 && usb_bounce[511] == 0xAB)
                usb_bounce[511] = 0xAA;         /* show the MBR as if the drive were not hidden */
            memcpy(rp, usb_bounce, bytes);
            rp += bytes;
        } else {
            memcpy(usb_bounce, wp, bytes);
            if (sector == 0 && s->hidden && usb_bounce[510] == 0x55 && usb_bounce[511] == 0xAA)
                usb_bounce[511] = 0xAB;         /* keep the drive hidden */
            r = USBStorage_Write(s->h, s->lun, sector, cnt, usb_bounce);
            if (r < 0) return false;
            wp += bytes;
        }
        sector += cnt;
        n -= cnt;
    }
    return true;
}

#define USB_IFACE(I)                                                              \
    static bool us_startup_##I(void) { return uslot[I].ok; }                      \
    static bool us_inserted_##I(void) { return uslot[I].ok; }                     \
    static bool us_read_##I(sec_t s, sec_t n, void *b) { return us_rw(I, s, n, b, NULL); }  \
    static bool us_write_##I(sec_t s, sec_t n, const void *b) { return us_rw(I, s, n, NULL, b); } \
    static bool us_clear_##I(void) { return true; }                               \
    static bool us_shutdown_##I(void) { return true; }
USB_IFACE(0) USB_IFACE(1) USB_IFACE(2) USB_IFACE(3) USB_IFACE(4) USB_IFACE(5) USB_IFACE(6) USB_IFACE(7)

#define USB_ENTRY(I) { DEVICE_TYPE_WII_USB, FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_WII_USB, \
    us_startup_##I, us_inserted_##I, us_read_##I, us_write_##I, us_clear_##I, us_shutdown_##I }
static const DISC_INTERFACE usb_iface[USB_SLOTS] = { USB_ENTRY(0), USB_ENTRY(1), USB_ENTRY(2), USB_ENTRY(3),
                                                      USB_ENTRY(4), USB_ENTRY(5), USB_ENTRY(6), USB_ENTRY(7) };

static void usb_close_all(void)
{
    for (int i = 0; i < USB_SLOTS; i++) {
        /* unmounted by name in unmount_fat() */
        uslot[i].ok = false;
        g_devs[DEV_USB0 + i].mounted = false;
        g_devs[DEV_USB0 + i].always_show = false;
        g_devs[DEV_USB0 + i].note[0] = '\0';
    }
    usb_seen = 0;
    usb_diag[0] = '\0';
    for (int i = 0; i < uhandles_open; i++)
        USBStorage_Close(&uhandle[i]);
    uhandles_open = 0;
}

/* Returns how many volumes (drive / LUN) were found. Devices behind a hub are
 * flattened by IOS, so they show up in the same device list; the hub itself is
 * not mass storage and is skipped by the class filter. */
static int usb_scan_once(void)
{
    static usb_device_entry list[32];
    u8 cnt = 0;
    if (!usb_inited) {
        if (USBStorage_Initialize() < 0)
            return 0;
        usb_inited = true;
    }
    for (int i = 0; i < USB_SLOTS; i++)
        uslot[i].ok = false;
    for (int i = 0; i < uhandles_open; i++)
        USBStorage_Close(&uhandle[i]);
    uhandles_open = 0;
    usb_diag[0] = '\0';
    if (USB_GetDeviceList(list, 32, USB_CLASS_MASS_STORAGE, &cnt) < 0)
        return 0;
    usb_seen = cnt;

    int slot = 0;
    for (int d = 0; d < cnt && slot < USB_SLOTS && uhandles_open < USB_SLOTS; d++) {
        usbstorage_handle *h = &uhandle[uhandles_open];
        memset(h, 0, sizeof(*h));
        ubuf_active = true;
        s32 orr = USBStorage_Open(h, list[d].device_id, list[d].vid, list[d].pid);
        ubuf_active = false;
        if (orr < 0) {
            snprintf(usb_diag, sizeof(usb_diag), "%04X:%04X cannot be opened (%d)", list[d].vid, list[d].pid, (int)orr);
            continue;
        }
        int first = slot;
        int luns = (int)h->max_lun;            /* libogc already stores the LUN count */
        if (luns < 1) luns = 1;
        for (int l = 0; l < luns && slot < USB_SLOTS; l++) {
            u32 ss = 0, ns = 0;
            s32 mr = USBStorage_MountLUN(h, l);
            if (mr < 0) {
                snprintf(usb_diag, sizeof(usb_diag), "%04X:%04X not ready (%d)", list[d].vid, list[d].pid, (int)mr);
                continue;
            }
            if (USBStorage_ReadCapacity(h, l, &ss, &ns) < 0 || ss == 0) {
                if (!usb_diag[0])
                    snprintf(usb_diag, sizeof(usb_diag), "%04X:%04X has no media", list[d].vid, list[d].pid);
                continue;
            }
            uslot[slot].h = h;
            uslot[slot].lun = l;
            uslot[slot].sector_size = ss;
            uslot[slot].sectors = ns;
            uslot[slot].ok = true;
            uslot[slot].hidden = false;
            {
                /* ustealth / USBHide marks a drive by changing the last MBR byte to 0xAB */
                u8 *mbr = memalign(32, ss > 512 ? ss : 512);
                if (mbr) {
                    if (USBStorage_Read(h, l, 0, 1, mbr) >= 0 && mbr[510] == 0x55 && mbr[511] == 0xAB)
                        uslot[slot].hidden = true;
                    free(mbr);
                }
            }
            slot++;
        }
        if (slot == first)
            USBStorage_Close(h);            /* nothing usable on it */
        else
            uhandles_open++;
    }
    return slot;
}

bool vfs_usb_raw_read(int slot, u64 off, void *buf, u32 len)
{
    if (slot < 0 || slot >= USB_SLOTS || !uslot[slot].ok)
        return false;
    u32 ss = uslot[slot].sector_size;
    if (!ss || ss > 32768)
        return false;
    static u8 *tmp;
    if (!tmp)
        tmp = malloc(32768);
    if (!tmp)
        return false;
    u8 *out = buf;
    while (len) {
        u64 sector = off / ss;
        u32 skew = (u32)(off % ss);
        u32 secs = (skew + len + ss - 1) / ss;
        if (secs > 32768 / ss)
            secs = 32768 / ss;
        if (sector + secs > uslot[slot].sectors)
            return false;
        if (!us_rw(slot, (sec_t)sector, secs, tmp, NULL))
            return false;
        u32 n = secs * ss - skew;
        if (n > len)
            n = len;
        memcpy(out, tmp + skew, n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

u64 vfs_usb_raw_size(int slot)
{
    if (slot < 0 || slot >= USB_SLOTS || !uslot[slot].ok)
        return 0;
    return (u64)uslot[slot].sectors * uslot[slot].sector_size;
}

/* How many mass-storage devices IOS currently lists (cheap: nothing is opened). */
static int usb_device_count(void)
{
    static usb_device_entry list[32];
    u8 cnt = 0;
    if (!usb_inited) {
        if (USBStorage_Initialize() < 0)
            return 0;
        usb_inited = true;
    }
    if (USB_GetDeviceList(list, 32, USB_CLASS_MASS_STORAGE, &cnt) < 0)
        return 0;
    return cnt;
}

/* Drives behind a hub spin up and enumerate one after another, so first wait
 * until the device count stops changing, then open every drive exactly once
 * (IOS does not like a drive being closed and re-opened straight away). */
static bool is_fat_vbr(const u8 *s)
{
    return s[510] == 0x55 && s[511] == 0xAA && (!memcmp(s + 0x36, "FAT", 3) || !memcmp(s + 0x52, "FAT32", 5));
}

static u32 rd32le(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* Finds the volume the way FatFs does: a boot sector at sector 0, or else the first
 * MBR or GPT basic data partition that holds a FAT or exFAT one. libdvm treats these
 * discs as 512-byte sectors, so this does too. */
static bool scan_exfat(const DISC_INTERFACE *io, u8 *buf)
{
    static const u8 gpt_basic_data[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                           0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    u32 start[128];
    int n = 0;
    if (!io->readSectors(0, 1, buf))
        return false;
    if (!memcmp(buf + 3, "EXFAT   ", 8))
        return true;
    if (is_fat_vbr(buf))
        return false;
    if (buf[0x1C2] == 0xEE) {
        if (!io->readSectors(1, 1, buf) || memcmp(buf, "EFI PART", 8))
            return false;
        u32 table = rd32le(buf + 72), count = rd32le(buf + 80);
        for (u32 i = 0; i < count && i < 128; i++) {
            if (i % 4 == 0 && !io->readSectors(table + i / 4, 1, buf))
                break;
            const u8 *e = buf + i % 4 * 128;
            if (!memcmp(e, gpt_basic_data, 16))
                start[n++] = rd32le(e + 32);
        }
    } else {
        for (int i = 0; i < 4; i++)
            start[n++] = rd32le(buf + 0x1C6 + i * 16);
    }
    for (int i = 0; i < n; i++) {
        if (!start[i] || !io->readSectors(start[i], 1, buf))
            continue;
        if (!memcmp(buf + 3, "EXFAT   ", 8))
            return true;
        if (is_fat_vbr(buf))
            return false;
    }
    return false;
}

static bool disc_is_exfat(const DISC_INTERFACE *io)
{
    u8 *buf = memalign(32, 4096);
    bool exfat = buf && scan_exfat(io, buf);
    free(buf);
    return exfat;
}

static void mount_fat(int dev, const char *name, const DISC_INTERFACE *io)
{
    g_devs[dev].mounted = fatMountSimple(name, io);
    g_devs[dev].exfat = g_devs[dev].mounted && disc_is_exfat(io);
}

static void mount_usb(int tries)
{
    int last = -1, stable = 0;
    for (int i = 0; i < tries; i++) {
        int n = usb_device_count();
        stable = (n == last) ? stable + 1 : 0;
        last = n;
        if (n > 0 && stable >= 1)
            break;
        usleep(400000);
    }
    if (last > 0)
        for (int attempt = 0; attempt < 3; attempt++) {
            if (usb_scan_once() > 0)
                break;
            usleep(500000);
        }
    for (int i = 0; i < USB_SLOTS; i++) {
        if (!uslot[i].ok)
            continue;
        char name[8];
        snprintf(name, sizeof(name), "usb%d", i + 1);
        mount_fat(DEV_USB0 + i, name, &usb_iface[i]);
        if (!g_devs[DEV_USB0 + i].mounted && arcfs_mount_wfs(DEV_USB0 + i, i)) {
            /* a Wii U formatted drive: shown read-only through the WFS reader */
            g_devs[DEV_USB0 + i].mounted = true;
            snprintf(g_devs[DEV_USB0 + i].note, sizeof(g_devs[0].note), "Wii U WFS volume  read-only");
        } else if (g_devs[DEV_USB0 + i].mounted && uslot[i].hidden) {
            snprintf(g_devs[DEV_USB0 + i].note, sizeof(g_devs[0].note), "hidden by ustealth  read / write");
        } else if (!g_devs[DEV_USB0 + i].mounted) {
            /* IOS found it but libfat cannot: exFAT / NTFS / unknown partitioning */
            g_devs[DEV_USB0 + i].always_show = true;
            snprintf(g_devs[DEV_USB0 + i].note, sizeof(g_devs[0].note), "not FAT32 (Wii U WFS needs keys in sd:/wfs)");
        }
    }

    /* Devices that were listed but never became a volume: say so instead of hiding them. */
    int used = 0;
    for (int i = 0; i < USB_SLOTS; i++)
        if (uslot[i].ok) used = i + 1;
    if (usb_seen > 0 && used < USB_SLOTS) {
        int vols = 0;
        for (int i = 0; i < USB_SLOTS; i++)
            vols += uslot[i].ok;
        if (usb_diag[0] || vols == 0) {
            vfs_dev *d = &g_devs[DEV_USB0 + used];
            d->always_show = true;
            snprintf(d->note, sizeof(d->note), "%s", usb_diag[0] ? usb_diag : "device found but unreadable");
        }
    }
}

/* ---------------------------------------------------------------- mounting */

static void mount_sd(void)
{
    mount_fat(DEV_SD, "sd", &__io_wiisd);
}

static void mount_gecko(void)
{
    /* The Wii U has no GameCube memory card slots to probe. */
    if (g_is_vwii)
        return;
    mount_fat(DEV_GCA, "gca", &__io_gcsda);
    mount_fat(DEV_GCB, "gcb", &__io_gcsdb);
}

static void unmount_fat(void)
{
    fatUnmount("sd");
    fatUnmount("gca");
    fatUnmount("gcb");
    for (int i = 0; i < USB_SLOTS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "usb%d", i + 1);
        fatUnmount(name);
    }
    for (int i = 0; i < DEV_NAND; i++)
        g_devs[i].mounted = false;
}

static void nand_stats(void)
{
    static u32 stats[8] __attribute__((aligned(32)));
    memset(stats, 0, sizeof(stats));
    vfs_dev *d = &g_devs[DEV_NAND];
    d->space_known = false;
    if (!nand_ready || ISFS_GetStats(stats) < 0)
        return;
    /* cluster_size, free, used, bad, reserved clusters ... */
    u64 cs = stats[0];
    d->free = cs * stats[1];
    d->total = cs * (stats[1] + stats[2] + stats[3] + stats[4]);
    if (d->total == 0)
        d->total = cs * (stats[1] + stats[2]);
    d->space_known = d->total > 0;
}

void vfs_update_space(int dev)
{
    vfs_dev *d = &g_devs[dev];
    if (dev == DEV_NAND) {
        nand_stats();
        return;
    }
    if (dev == DEV_MCA || dev == DEV_MCB)
        return;             /* the memory card module fills in its own space */
    d->space_known = false;
    if (vfs_is_virtual(dev))
        return;
    if (!d->mounted)
        return;
    struct statvfs sv;
    if (statvfs(d->root, &sv) == 0) {
        u64 fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        d->total = (u64)sv.f_blocks * fr;
        d->free = (u64)sv.f_bfree * fr;
        d->space_known = true;
    }
}

void vfs_init(void)
{
    nand_ready = ISFS_Initialize() >= 0;
    g_devs[DEV_NAND].mounted = nand_ready;
    mount_sd();
    mount_gecko();
    mount_usb(8);
    virt_refresh(DEV_SEEPROM);
    virt_refresh(DEV_DISC);
    virt_refresh(DEV_MCA);
    virt_refresh(DEV_MCB);
    virt_refresh(DEV_KEYS);
    for (int i = 0; i < DEV_COUNT; i++)
        vfs_update_space(i);
}

void vfs_remount(void)
{
    arcfs_unpin_all();
    unmount_fat();
    usb_close_all();
    __io_wiisd.shutdown();
    mount_sd();
    mount_gecko();
    mount_usb(4);
    g_devs[DEV_NAND].mounted = nand_ready;
    virt_refresh(DEV_SEEPROM);
    virt_refresh(DEV_DISC);
    virt_refresh(DEV_MCA);
    virt_refresh(DEV_MCB);
    virt_refresh(DEV_KEYS);
    for (int i = 0; i < DEV_COUNT; i++)
        vfs_update_space(i);
}

void vfs_shutdown(void)
{
    arcfs_unpin_all();
    unmount_fat();
    usb_close_all();
    __io_wiisd.shutdown();
    virt_shutdown();
    if (usb_inited)
        USBStorage_Deinitialize();
    if (nand_ready)
        ISFS_Deinitialize();
    nand_ready = false;
}

/* ---------------------------------------------------------------- listing */

static int entry_cmp(const void *a, const void *b)
{
    const vfs_entry *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool nand_is_dir(const char *path)
{
    u32 n = 0;
    return ISFS_ReadDir(path, NULL, &n) >= 0;
}

static bool nand_file_size(const char *path, u64 *size)
{
    static fstats st __attribute__((aligned(32)));
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0)
        return false;
    bool ok = ISFS_GetFileStats(fd, &st) >= 0;
    if (ok)
        *size = st.file_length;
    ISFS_Close(fd);
    return ok;
}

bool vfs_list(const char *path, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;
    int dev = vfs_dev_of(path);
    if (dev < 0 || !g_devs[dev].mounted)
        return false;

    if (arcfs_is_path(path)) {
        if (!arcfs_list(path, out, count))
            return false;
        if (*count > 1)
            qsort(*out, *count, sizeof(vfs_entry), entry_cmp);
        return true;
    }
    if (vfs_is_virtual(dev))
        return virt_list(dev, virt_name(path), out, count);

    int cap = 0, n = 0;
    vfs_entry *list = NULL;

    if (dev == DEV_NAND) {
        const char *np = nand_path(path);
        u32 num = 0;
        if (ISFS_ReadDir(np, NULL, &num) < 0)
            return false;
        if (num) {
            char *buf = memalign(32, num * 13 + 32);
            if (!buf)
                return false;
            memset(buf, 0, num * 13 + 32);
            u32 got = num;
            if (ISFS_ReadDir(np, buf, &got) < 0) {
                free(buf);
                return false;
            }
            list = calloc(got ? got : 1, sizeof(vfs_entry));
            const char *p = buf;
            for (u32 i = 0; i < got && list; i++) {
                size_t l = strlen(p);
                if (l) {
                    vfs_entry *e = &list[n++];
                    snprintf(e->name, sizeof(e->name), "%s", p);
                    char child[VFS_PATH_MAX];
                    vfs_join(child, sizeof(child), np, p);
                    e->is_dir = nand_is_dir(child);
                    if (!e->is_dir && !nand_file_size(child, &e->size))
                        e->size = 0;
                }
                p += l + 1;
            }
            free(buf);
        }
    } else {
        DIR *dir = opendir(path);
        if (!dir)
            return false;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                vfs_entry *nl = realloc(list, cap * sizeof(vfs_entry));
                if (!nl) {
                    free(list);
                    closedir(dir);
                    return false;
                }
                list = nl;
            }
            vfs_entry *e = &list[n];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            char child[VFS_PATH_MAX];
            struct stat st;
            if (vfs_join(child, sizeof(child), path, de->d_name) && stat(child, &st) == 0) {
                e->is_dir = S_ISDIR(st.st_mode);
                e->size = e->is_dir ? 0 : (u64)st.st_size;
            }
            n++;
        }
        closedir(dir);
    }

    if (n > 1)
        qsort(list, n, sizeof(vfs_entry), entry_cmp);
    *out = list;
    *count = n;
    return true;
}

/* Files the operating systems drop on FAT volumes: not worth showing. */
bool vfs_is_hidden_name(const char *name)
{
    static const char *const junk[] = {
        "thumbs.db", "desktop.ini", ".ds_store", "system volume information", "$recycle.bin",
        ".trashes", ".spotlight-v100", ".fseventsd", ".temporaryitems", ".apdisk",
        "__macosx", "ehthumbs.db", "found.000",
    };
    if (name[0] == '.')                             /* dotfiles: .DS_Store, ._*, .Trashes, ... */
        return true;
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
        if (!strcasecmp(name, junk[i]))
            return true;
    return false;
}

bool vfs_list_visible(const char *path, vfs_entry **out, int *count)
{
    if (!vfs_list(path, out, count))
        return false;
    int dev = vfs_dev_of(path);
    if (dev < 0 || dev >= DEV_NAND || arcfs_is_path(path))   /* only FAT drives carry this junk */
        return true;
    int k = 0;
    for (int i = 0; i < *count; i++)
        if (!vfs_is_hidden_name((*out)[i].name))
            (*out)[k++] = (*out)[i];
    *count = k;
    return true;
}

bool vfs_stat(const char *path, bool *is_dir, u64 *size)
{
    int dev = vfs_dev_of(path);
    if (dev < 0 || !g_devs[dev].mounted)
        return false;
    if (vfs_is_root(path)) {
        if (is_dir) *is_dir = true;
        if (size) *size = 0;
        return true;
    }
    if (arcfs_is_path(path))
        return arcfs_stat(path, is_dir, size);
    if (vfs_is_virtual(dev)) {
        return virt_stat(dev, virt_name(path), is_dir, size);
    }
    if (dev == DEV_NAND) {
        const char *np = nand_path(path);
        if (nand_is_dir(np)) {
            if (is_dir) *is_dir = true;
            if (size) *size = 0;
            return true;
        }
        u64 sz = 0;
        if (!nand_file_size(np, &sz))
            return false;
        if (is_dir) *is_dir = false;
        if (size) *size = sz;
        return true;
    }
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (size) *size = S_ISDIR(st.st_mode) ? 0 : (u64)st.st_size;
    return true;
}

bool vfs_exists(const char *path)
{
    return vfs_stat(path, NULL, NULL);
}

/* ---------------------------------------------------------------- modify */

bool vfs_mkdir(const char *path)
{
    int dev = vfs_dev_of(path);
    if (dev < 0 || vfs_is_virtual(dev) || arcfs_is_path(path))
        return false;
    arcfs_flush();
    if (dev == DEV_NAND)
        return ISFS_CreateDir(nand_path(path), 0, 3, 3, 3) >= 0;
    return mkdir(path, 0777) == 0;
}

bool vfs_remove(const char *path, bool is_dir)
{
    int dev = vfs_dev_of(path);
    if (arcfs_is_path(path))
        return false;
    if (vfs_is_card(dev))
        return !is_dir && mcard_delete(dev, virt_name(path));
    if (dev < 0 || vfs_is_virtual(dev))
        return false;
    arcfs_flush();
    if (dev == DEV_NAND)
        return ISFS_Delete(nand_path(path)) >= 0;
    if (is_dir ? rmdir(path) == 0 : unlink(path) == 0)
        return true;
    /* FAT refuses to delete a read-only entry: drop the flag and try again */
    int attr = FAT_getAttr(path);
    if (attr < 0 || !(attr & ATTR_READONLY))
        return false;
    if (FAT_setAttr(path, attr & ~ATTR_READONLY) < 0)
        return false;
    if (is_dir ? rmdir(path) == 0 : unlink(path) == 0)
        return true;
    FAT_setAttr(path, attr);
    return false;
}

bool vfs_rename(const char *from, const char *to)
{
    int a = vfs_dev_of(from), b = vfs_dev_of(to);
    if (a < 0 || a != b || vfs_is_virtual(a) || arcfs_is_path(from) || arcfs_is_path(to))
        return false;
    arcfs_flush();
    if (a == DEV_NAND)
        return ISFS_Rename(nand_path(from), nand_path(to)) >= 0;
    return rename(from, to) == 0;
}

void vfs_clone_attrs(const char *from, const char *to)
{
    if (vfs_dev_of(from) != DEV_NAND || vfs_dev_of(to) != DEV_NAND)
        return;
    u32 owner; u16 group; u8 attr, op, gp, ot;
    if (ISFS_GetAttr(nand_path(from), &owner, &group, &attr, &op, &gp, &ot) >= 0)
        ISFS_SetAttr(nand_path(to), owner, group, attr, op, gp, ot);
}

/* ---------------------------------------------------------------- files */

bool vfs_open_read(vfs_file *f, const char *path)
{
    memset(f, 0, sizeof(*f));
    f->dev = vfs_dev_of(path);
    f->fd = -1;
    if (f->dev < 0)
        return false;
    if (arcfs_is_path(path))
        return arcfs_open(f, path);
    if (vfs_is_virtual(f->dev))
        return virt_open(f, f->dev, virt_name(path));
    if (f->dev == DEV_NAND) {
        f->fd = ISFS_Open(nand_path(path), ISFS_OPEN_READ);
        if (f->fd < 0)
            return false;
        u64 sz = 0;
        nand_file_size(nand_path(path), &sz);
        f->size = sz;
        return true;
    }
    f->fp = fopen(path, "rb");
    if (!f->fp)
        return false;
    fseek(f->fp, 0, SEEK_END);
    f->size = (u64)ftell(f->fp);
    fseek(f->fp, 0, SEEK_SET);
    return true;
}

bool vfs_open_write(vfs_file *f, const char *path)
{
    memset(f, 0, sizeof(*f));
    f->dev = vfs_dev_of(path);
    f->fd = -1;
    g_write_err = NULL;
    if (arcfs_is_path(path))
        return false;
    if (vfs_is_card(f->dev))
        return mcard_open_write(f, f->dev);
    if (f->dev < 0 || vfs_is_virtual(f->dev))
        return false;
    arcfs_flush();
    if (f->dev == DEV_NAND) {
        const char *np = nand_path(path);
        ISFS_Delete(np);        /* replace semantics; failure is fine if absent */
        if (ISFS_CreateFile(np, 0, 3, 3, 3) < 0)
            return false;
        f->fd = ISFS_Open(np, ISFS_OPEN_RW);
        return f->fd >= 0;
    }
    f->fp = fopen(path, "wb");
    return f->fp != NULL;
}

s32 vfs_read(vfs_file *f, void *buf, u32 len)
{
    if (f->arc)
        return arcfs_read(f, buf, len);
    if (vfs_is_virtual(f->dev))
        return virt_read(f, buf, len);
    if (f->dev == DEV_NAND)
        return ISFS_Read(f->fd, buf, len);
    size_t n = fread(buf, 1, len, f->fp);
    if (n == 0 && ferror(f->fp))
        return -1;
    return (s32)n;
}

s32 vfs_write(vfs_file *f, const void *buf, u32 len)
{
    if (f->arc)
        return -1;
    if (vfs_is_card(f->dev))
        return mcard_write(f, buf, len);
    if (vfs_is_virtual(f->dev))
        return -1;
    if (f->dev == DEV_NAND)
        return ISFS_Write(f->fd, buf, len);
    size_t n = fwrite(buf, 1, len, f->fp);
    return (n == len) ? (s32)n : -1;
}

bool vfs_seek(vfs_file *f, u64 off)
{
    if (f->arc)
        return arcfs_seek(f, off);
    if (vfs_is_virtual(f->dev))
        return virt_seek(f, off);
    if (f->dev == DEV_NAND)
        return ISFS_Seek(f->fd, (s32)off, 0) >= 0;
    return fseek(f->fp, (long)off, SEEK_SET) == 0;
}

bool vfs_close_checked(vfs_file *f)
{
    bool ok = true;
    if (f->arc) {
        arcfs_close(f);
        return true;
    }
    if (vfs_is_card(f->dev) && f->ctx) {
        ok = mcard_close_write(f);
    } else if (vfs_is_virtual(f->dev)) {
        virt_close(f);
        return true;
    } else if (f->dev == DEV_NAND) {
        if (f->fd >= 0)
            ISFS_Close(f->fd);
    } else if (f->fp) {
        fflush(f->fp);
        fclose(f->fp);
    }
    f->fp = NULL;
    f->fd = -1;
    return ok;
}

void vfs_close(vfs_file *f)
{
    vfs_close_checked(f);
}

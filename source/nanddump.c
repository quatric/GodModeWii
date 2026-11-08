/*
 * NAND backup, ported from nanddumper@IOS by thepikachugamer (https://github.com/Naim2000/nanddumper_ios,
 * GPL-2.0). See nanddump.h and nanddumper/README.md.
 *
 * How it gets at the raw NAND, as nanddumper@IOS does it:
 *   1. The /dev/sha exploit (mkwcat): SHA_Init with its context pointer at 0xFFFE0028 makes IOS overwrite a
 *      thread's saved PC, so stage0 (in low MEM1) runs in IOS, which calls stage1, which starts the
 *      "realcode" thread: it lifts the PPC's memory protections, becomes root (UID 0) and serves
 *      /dev/realcode (virtual -> physical addresses, cache maintenance).
 *   2. The FS module's code is patched so /dev/flash can be opened: the root-only /dev/boot2 open is
 *      pointed at the (stubbed out) flash open, and the two names are swapped.
 *   3. Every page is read with its spare area (ECC) through /dev/flash; a bad block (check byte in either
 *      of its first two pages) is written as zeros. The file is BootMii's: 4096 blocks x 64 pages x
 *      (2048 + 64) bytes, then the 0x400 byte keys.bin. A SHA-1 of it (by /dev/sha) goes in <name>.sha1 and
 *      sha1sums.txt.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <gccore.h>
#include <ogc/ipc.h>
#include <ogc/isfs.h>
#include <ogc/es.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#include "nanddump.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include "otp.h"
#include "seeprom.h"
#include "runtimeiospatch.h"
#include "nd_stage1_bin.h"
#include "nd_realcode_bin.h"

#define BACKUP_DIR "wii/backups"
#define IS_WIIU ((*(volatile u32 *)0xCD8005A0 & 0xFFFF0000) == 0xCAFE0000)

/* ---------------------------------------------------------------- /dev/sha and the exploit */

typedef struct {
    u32 state[5];
    u32 length[2];
} sha_ctx;

static int sha_init(sha_ctx *ctx)
{
    ioctlv v[3];
    memset(v, 0, sizeof(v));
    v[1].data = ctx;
    if ((uintptr_t)ctx < 0xFFF00000) v[1].len = sizeof(*ctx);
    return IOS_Ioctlv(0x10001, 0, 1, 2, v);
}

static int sha_update(sha_ctx *ctx, void *data, u32 size)
{
    int ret = 0;
    ioctlv v[3];
    memset(v, 0, sizeof(v));
    v[1].data = ctx;
    v[1].len = sizeof(*ctx);
    for (u32 x = 0; x < size;) {
        u32 len = size - x > 0x10000 ? 0x10000 : size - x;
        v[0].data = (u8 *)data + x;
        v[0].len = len;
        ret = IOS_Ioctlv(0x10001, 1, 1, 2, v);
        if (ret < 0) break;
        x += len;
    }
    return ret;
}

static int sha_finish(sha_ctx *ctx, u32 hash[5])
{
    ioctlv v[3];
    memset(v, 0, sizeof(v));
    v[1].data = ctx;
    v[1].len = sizeof(*ctx);
    v[2].data = hash;
    v[2].len = 20;
    return IOS_Ioctlv(0x10001, 2, 1, 2, v);
}

struct thread_parameters {
    u32 entrypoint, argument, stack_top, stack_size, priority, pid, threadid_out;
};

static const u32 stage0[10] = {
    0x4906468d, 0x48064907, 0x478846c0, 0x477846c0,     /* thumb: sp = 0x10100000; blx stage1(params) */
    0xe3a00000, 0xee070f90, 0xeafffffc,                 /* arm: wait for interrupt, forever */
    0x10100000, 0xdeadbeef, 0x0badc0de,
};

static bool hijacked;
bool nanddump_ios_busy(void)
{
    if (!hijacked) return false;
    int fd = IOS_Open("/dev/realcode", 0);              /* gone after an IOS reload */
    if (fd >= 0) IOS_Close(fd);
    else hijacked = false;
    return hijacked;
}

static int sha_exploit(const void *entry, bool thumb, void *stack, u32 stack_size, int prio)
{
    static struct thread_parameters params ATTRIBUTE_ALIGN(32);
    params.entrypoint = MEM_VIRTUAL_TO_PHYSICAL(entry) | (thumb ? 1 : 0);
    params.argument = 0;
    params.stack_top = MEM_VIRTUAL_TO_PHYSICAL((u8 *)stack + stack_size);
    params.stack_size = stack_size;
    params.priority = (u32)prio;
    params.pid = 0;
    params.threadid_out = sizeof(stage0);

    DCFlushRange((void *)entry, nd_realcode_bin_size);
    DCFlushRange((void *)nd_stage1_bin, nd_stage1_bin_size);

    /* stage0 goes at the very start of MEM1, and the IOS thread it takes over keeps idling there (in its
     * last three words) until IOS is reloaded: those 0x28 bytes must stay as they are until then */
    u32 *mem1 = (u32 *)SYS_BASE_CACHED;
    memcpy(mem1, stage0, sizeof(stage0));
    mem1[8] = MEM_VIRTUAL_TO_PHYSICAL(&params);
    mem1[9] = MEM_VIRTUAL_TO_PHYSICAL(nd_stage1_bin) | 1;
    DCFlushRange(mem1, sizeof(stage0));
    volatile int *tid = (volatile int *)((u8 *)SYS_BASE_UNCACHED + sizeof(stage0));
    *tid = 0;
    DCFlushRange(&params, sizeof(params));

    int ret = sha_init((sha_ctx *)0xFFFE0028);
    if (ret == 0) {
        for (int t = 1000; !(ret = *tid); t--) {
            if (!t) { ret = -1; break; }
            usleep(1000);
        }
    }
    if (ret > 0) hijacked = true;
    return ret;
}

/* ---------------------------------------------------------------- /dev/realcode */

static u8 realcode_stack[0x1000] ATTRIBUTE_ALIGN(32);
static int rc_fd = -1;

static int realcode_init(void)
{
    if (rc_fd >= 0) {
        int fd = IOS_Open("/dev/realcode", 0);              /* still there (no IOS reload since)? */
        if (fd >= 0) { IOS_Close(fd); return 0; }
        rc_fd = -1;
    }
    int fd = IOS_Open("/dev/realcode", 0);
    if (fd >= 0) { rc_fd = fd; return 0; }
    int ret = sha_exploit(nd_realcode_bin, true, realcode_stack, sizeof(realcode_stack), 0x7F);
    if (ret < 0) return ret;
    for (int tries = 500; tries; tries--) {
        fd = IOS_Open("/dev/realcode", 0);
        if (fd != IPC_ENOENT) break;
        usleep(50);
    }
    if (fd < 0) return fd;
    rc_fd = fd;
    return 0;
}

static void *rc_virt_to_phys(void *addr) { return (void *)IOS_Ioctl(rc_fd, 0, addr, 0, 0, 0); }
static void rc_invalidate_icache(void) { IOS_Ioctl(rc_fd, 1, 0, 0, 0, 0); }
static void rc_invalidate_dcache(const void *addr, u32 len)
{
    static u32 p[2] ATTRIBUTE_ALIGN(32);
    p[0] = (u32)addr, p[1] = len;
    IOS_Ioctl(rc_fd, 2, p, sizeof(p), 0, 0);
}

/* ---------------------------------------------------------------- /dev/flash */

static int patch_flash_access(char *err, size_t en)
{
    int fd = IOS_Open("/dev/flash", 0);
    if (fd >= 0) {                                          /* patched already */
        IOS_Close(fd);
        return 0;
    }
    u8 *va = (u8 *)0x20000000;                               /* the FS module's code, in IOS */
    u8 *text = (u8 *)MEM_PHYSICAL_TO_K0((u32)rc_virt_to_phys(va));
    int translate = va - text;
    DCInvalidateRange(text, 0x8000);

    static const u16 patA[] = { 0x2300, 0x2b01, 0xd102 };    /* mov r3,#0; cmp r3,#1; bne (new IOS) */
    static const u16 patB[] = { 0x4643, 0x2b01, 0xd102 };    /* mov r3,r8; cmp r3,#1; bne (old IOS) */
    static const u16 pat2[] = { 0x68ab, 0x2b00, 0xd002, 0x2001, 0x4240, 0xe008 };   /* the /dev/boot2 uid check */

    u16 *pc = memmem(text, 0x8000, patA, sizeof(patA));
    if (!pc) pc = memmem(text, 0x8000, patB, sizeof(patB));
    if (!pc) {
        snprintf(err, en, "This IOS's FS module isn't one the NAND reader knows (no stubbed /dev/flash check)");
        return -1;
    }
    pc += 3;
    int x = ((pc[0] & 0x7FF) << 11) | (pc[1] & 0x7FF), m = 1 << 21, dst = (x ^ m) - m;
    u16 *open_flash = pc + 2 + dst;

    pc = memmem(pc, 0x100, pat2, sizeof(pat2));
    if (!pc) {
        snprintf(err, en, "This IOS's FS module isn't one the NAND reader knows (no /dev/boot2 check)");
        return -2;
    }
    pc += 6;

    char *s = NULL;
    if (fd == -6) {
        s = memmem(text, 0x8000, "\0boot2", 7);
        if (!s) { snprintf(err, en, "No \"boot2\" in the FS module"); return -3; }
        s++;
        strcpy(s, "flash");
    } else if (fd == -106) {
        static const char both[16] = "flash\0\0\0boot2";
        s = memmem(text, 0x8000, both, 16);
        if (!s) { snprintf(err, en, "No \"flash\" / \"boot2\" in the FS module"); return -4; }
        strcpy(s, "boot2");
        strcpy(s + 8, "flash");
    } else {
        snprintf(err, en, "Opening /dev/flash gave %d", fd);
        return fd;
    }
    dst = open_flash - (pc + 2);
    pc[0] = 0xF000 | ((dst >> 11) & 0x7FF);                 /* bl FS_OpenFlash */
    pc[1] = 0xF800 | (dst & 0x7FF);
    DCFlushRange(pc, 4);
    DCFlushRange(s, 32);
    rc_invalidate_icache();
    rc_invalidate_dcache(s + translate, 32);
    return 0;
}

typedef struct {
    u32 nand_size, block_size, page_size, spare_size, spare_misc_size;   /* log2 */
    u16 block_copy_mask, supports_page_copy, check_byte_ofs, pad;
} flash_info;

enum { FLASH_EIO = -1, FLASH_ECC = -11, FLASH_ECC_CRIT = -12 };

static int fl_fd = -1;
static flash_info fl ATTRIBUTE_ALIGN(32);

static bool page_erased(const u8 *page)
{
    const u32 *ecc = (const u32 *)(page + (1u << fl.page_size) + (1u << fl.spare_size));
    for (int i = 1 << (fl.page_size - 9); i; i--)
        if (~ecc[-i]) return false;
    return true;
}

static int read_page(u32 idx, u8 *out)
{
    u32 sz = (1u << fl.page_size) + (1u << fl.spare_size);
    int ret = IOS_Seek(fl_fd, (s32)idx, SEEK_SET);
    if (ret < 0) return ret;
    ret = IOS_Read(fl_fd, out, (s32)sz);
    if (ret == (int)sz || (ret == FLASH_ECC_CRIT && page_erased(out))) ret = 0;
    return ret;
}

/* ---------------------------------------------------------------- console details */

static char setting[0x100] ATTRIBUTE_ALIGN(32);
static bool setting_ok;

static void setting_load(void)
{
    static const u32 key[8] = { 0xfaf4e9d3, 0xa74e9c39, 0x73e7ce9d, 0x3b76edda,
                                0xb56bd7ae, 0x5dbb76ed, 0xdbb76fdf, 0xbf7ffefd };
    if (setting_ok) return;
    s32 fd = ISFS_Open("/title/00000001/00000002/data/setting.txt", ISFS_OPEN_READ);
    if (fd < 0) return;
    s32 r = ISFS_Read(fd, setting, sizeof(setting));
    ISFS_Close(fd);
    if (r != (s32)sizeof(setting)) return;
    size_t n = strnlen(setting, sizeof(setting));
    u32 *w = (u32 *)setting;
    for (int i = 0; i < 8; i++)
        for (int j = i; j < 0x40; j += 8) w[j] ^= key[i];
    memset(setting + n, 0, sizeof(setting) - n);
    setting_ok = true;
}

static bool setting_find(const char *item, char *out, size_t n)
{
    if (!setting_ok) return false;
    const char *p = setting;
    size_t il = strlen(item);
    while (*p && p < setting + sizeof(setting)) {
        const char *eq = strchr(p, '='), *end = strchr(p, '\r');
        if (!end) end = strchr(p, '\n');
        if (!eq || !end) break;
        if ((size_t)(eq - p) == il && !memcmp(p, item, il)) {
            size_t vl = (size_t)(end - eq - 1);
            if (vl >= n) return false;
            memcpy(out, eq + 1, vl);
            out[vl] = 0;
            return true;
        }
        while (*end && isspace((unsigned char)*end)) end++;
        p = end;
    }
    return false;
}

static void serial_of(char out[24])
{
    char code[4], serno[12];
    setting_load();
    if (setting_find("CODE", code, sizeof(code)) && setting_find("SERNO", serno, sizeof(serno)))
        snprintf(out, 24, "%s%s", code, serno);
    else
        snprintf(out, 24, "UNKNOWN");
}

static const char *model_of(char model[16])
{
    setting_load();
    if (!setting_find("MODEL", model, 16)) {
        snprintf(model, 16, "UNKNOWN");
        return "UNKNOWN";
    }
    u32 id = 0;
    if (ES_GetDeviceID(&id) == 0 && (id >> 28) == 2) return "vWii (Wii U)";
    if (!strncmp(model, "RVL-001", 7)) return "Wii";
    if (!strncmp(model, "RVL-101", 7)) return "Wii Family Edition";
    if (!strncmp(model, "RVL-201", 7)) return "Wii Mini";
    if (!strncmp(model, "RVT", 3)) return "NDEV 1.x";
    if (!strncmp(model, "RVD", 3)) return "NDEV 2.x";
    return "UNKNOWN";
}

/* keys.bin, BootMii's layout: a comment, the OTP at 0x100, the SEEPROM at 0x200. On a Wii U the SEEPROM
 * data a Wii keeps (NG key ID and signature) is in OTP bank 6, which c2w copies to the top of SRAM. */
static bool build_keys(u8 keys[0x400], char *err, size_t en)
{
    memset(keys, 0, 0x400);
    u32 id = 0;
    ES_GetDeviceID(&id);
    char model[16], serial[24];
    const char *type = model_of(model);
    serial_of(serial);
    snprintf((char *)keys, 0x100,
             "nanddumper@IOS by thepikachugamer (in GodModeWii)\nConsole model: %s [%s]\nConsole ID: %08x\n"
             "Serial number: %s\n\nthank you mkwcat <3", type, model, (unsigned)id, serial);
    if (!otp_read_all(keys + 0x100)) {
        snprintf(err, en, "Couldn't read the OTP");
        return false;
    }
    if (IS_WIIU) {
        memcpy(keys + 0x200, (const void *)0xCD4E7F00, 0x100);
    } else if (!seeprom_read(keys + 0x200)) {
        snprintf(err, en, "Couldn't read the SEEPROM");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- the backup */

static int pick_drive(const char *title)
{
    const char *items[1 + USB_SLOTS];
    int devs[1 + USB_SLOTS], n = 0;
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (!g_devs[d].mounted || vfs_is_readonly(g_devs[d].root)) continue;
        items[n] = g_devs[d].name;
        devs[n++] = d;
    }
    if (!n) {
        ui_message(title, C_RED, "There is no writable SD card or USB drive to save to.");
        return -1;
    }
    if (n == 1) return devs[0];
    int r = ui_menu("SAVE THE BACKUP ON", items, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

static bool mkdir_p(const char *path)
{
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
        if (s > p && s[-1] == ':') continue;
        *s = 0;
        if (mkdir(p, 0777) < 0 && errno != EEXIST) return false;
        *s = '/';
    }
    return mkdir(p, 0777) == 0 || errno == EEXIST;
}

static void bar(const char *title, const char *line1, const char *line2, u64 done, u64 total)
{
    ui_splash(title);
    int r = ui_rows() / 2 + 1, cols = ui_cols();
    ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(line1)) / 2, "", line1);
    ui_field(r + 1, 0, cols - 1, "%*s%s", (cols - (int)strlen(line2)) / 2, "", line2);
    ui_bar(r + 2, (cols - 50) / 2, 50, total ? (int)(done * 100 / total) : 0, C_GREEN);
}

static bool write_keys(const char *path, const u8 keys[0x400])
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(keys, 0x400, 1, f) == 1;
    return fclose(f) == 0 && ok;
}

/* `full`: nand.bin too; otherwise keys.bin only. */
static void run(bool full)
{
    const char *T = full ? "NAND BACKUP" : "KEYS BACKUP";
    char err[200] = "";
    if (g_is_dolphin) {
        ui_message(T, C_YELLOW, "Dolphin has no real NAND chip or IOS to read it with: this only works on a Wii or a Wii U.");
        return;
    }
    int dev = pick_drive(T);
    if (dev < 0) return;

    char q[500];
    if (full)
        snprintf(q, sizeof(q),
                 "Back up the whole NAND to %s?\n\nMade the way nanddumper@IOS makes it: a 1:1 BootMii-format nand.bin "
                 "(about 529 MB, with its ECC data) and keys.bin in /wii/backups, plus its SHA-1. It takes about 5 "
                 "minutes; hold B to stop. The NAND is only read.\n\nIOS is patched in memory to allow it (until the "
                 "next IOS reload).", g_devs[dev].name);
    else
        snprintf(q, sizeof(q),
                 "Save this console's keys.bin (BootMii's layout: OTP and SEEPROM) to /wii/backups on %s?\n\n"
                 "It holds keys unique to your console. Keep it private.", g_devs[dev].name);
    if (!ui_confirm(T, C_CYAN, q)) return;

    ui_splash("Getting into IOS...");
    int ret = realcode_init();
    if (ret < 0 && (full || IS_WIIU || !AHBPROT_DISABLED)) {
        snprintf(err, sizeof(err), "Couldn't run code in IOS (the /dev/sha exploit gave %d)", ret);
        goto fail;
    }

    u8 *keys = memalign(32, 0x400);
    if (!keys) { snprintf(err, sizeof(err), "Out of memory"); goto fail; }
    if (!build_keys(keys, err, sizeof(err))) { free(keys); goto fail; }

    char serial[24], dir[VFS_PATH_MAX], kpath[VFS_PATH_MAX + 48], npath[VFS_PATH_MAX + 64];
    serial_of(serial);
    snprintf(dir, sizeof(dir), "%s%s", g_devs[dev].root, BACKUP_DIR);
    snprintf(kpath, sizeof(kpath), "%s/%s_keys.bin", dir, serial);
    if (!mkdir_p(dir)) {
        free(keys);
        snprintf(err, sizeof(err), "Couldn't create /%s on %s", BACKUP_DIR, g_devs[dev].name);
        goto fail;
    }
    bool kok = write_keys(kpath, keys);
    if (!full) {
        memset(keys, 0, 0x400);
        free(keys);
        if (!kok) { snprintf(err, sizeof(err), "Couldn't write %s", kpath); goto fail; }
        char msg[VFS_PATH_MAX + 120];
        snprintf(msg, sizeof(msg), "Saved %s\n\nKeep it private: it holds this console's unique keys.", kpath);
        ui_message("KEYS SAVED", C_GREEN, msg);
        return;
    }

    /* --- /dev/flash */
    ui_splash("Opening the NAND...");
    if (patch_flash_access(err, sizeof(err)) < 0) { free(keys); goto fail; }
    fl_fd = IOS_Open("/dev/flash", 0);
    if (fl_fd < 0 || IOS_Ioctl(fl_fd, 1, NULL, 0, &fl, sizeof(fl)) < 0) {
        snprintf(err, sizeof(err), "Couldn't open /dev/flash (%d)", fl_fd);
        if (fl_fd >= 0) IOS_Close(fl_fd);
        fl_fd = -1;
        free(keys);
        goto fail;
    }
    u32 page_sz = (1u << fl.page_size) + (1u << fl.spare_size);
    u32 pages = 1u << (fl.block_size - fl.page_size);
    u32 block_sz = page_sz * pages;
    u32 blocks = 1u << (fl.nand_size - fl.block_size);
    u64 file_sz = (u64)block_sz * blocks;
    const u32 group = 8;
    u8 *buf = memalign(64, block_sz * group);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date[16];
    snprintf(date, sizeof(date), "%02d%02d%02d", tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday);
    struct stat st;
    for (int i = 0; i < 100; i++) {
        snprintf(npath, sizeof(npath), "%s/%s_%s_nand_%02d.bin", dir, date, serial, i);
        if (stat(npath, &st) < 0) break;
    }
    struct statvfs fs;
    FILE *fp = NULL;
    if (!buf) {
        snprintf(err, sizeof(err), "Out of memory");
    } else if (statvfs(g_devs[dev].root, &fs) == 0 && (u64)fs.f_bsize * fs.f_bfree <= file_sz + 0x400) {
        snprintf(err, sizeof(err), "%s needs at least %u MB free", g_devs[dev].name, (unsigned)(file_sz >> 20) + 1);
    } else if (!(fp = fopen(npath, "wb"))) {
        snprintf(err, sizeof(err), "Couldn't create %s", npath);
    } else if (fseeko(fp, (off_t)file_sz, SEEK_SET) < 0 || fwrite(keys, 0x400, 1, fp) != 1 || fseeko(fp, 0, SEEK_SET) < 0) {
        snprintf(err, sizeof(err), "Couldn't make room for the backup (is the drive full?)");   /* sized up front */
    } else {
        static sha_ctx sha ATTRIBUTE_ALIGN(32);
        static u32 hash[5] ATTRIBUTE_ALIGN(32);
        memset(&sha, 0, sizeof(sha));
        sha_init(&sha);
        u32 bad = 0, corrected = 0, uncorrectable = 0, ioerr = 0;
        u64 began = ticks_to_millisecs(gettime()), last = 0;
        bool stop = false, wfail = false;
        for (u32 b = 0; b < blocks && !stop && !wfail; b++) {
            u8 *blk = buf + (b % group) * block_sz;
            for (u32 p = 0; p < pages; p++) {
                u8 *pg = blk + p * page_sz;
                int r = read_page(b * pages + p, pg);
                if (p < 2 && (r == 0 || r == FLASH_ECC_CRIT) && pg[(1u << fl.page_size) + fl.check_byte_ofs] != 0xFF) {
                    memset(blk, 0, block_sz);                        /* a bad block, as BootMii writes it */
                    bad++;
                    break;
                }
                if (r == FLASH_ECC) corrected++;
                else if (r == FLASH_ECC_CRIT) uncorrectable++;
                else if (r < 0) ioerr++;
            }
            sha_update(&sha, blk, block_sz);
            u32 cnt = b % group + 1;
            if ((cnt == group || b + 1 == blocks) && fwrite(buf, (size_t)block_sz * cnt, 1, fp) != 1) wfail = true;
            u64 t = ticks_to_millisecs(gettime());
            if (t - last >= 250 || b + 1 == blocks) {
                last = t;
                char l1[80], l2[96];
                snprintf(l1, sizeof(l1), "Block %u of %u  (hold B to stop)", (unsigned)(b + 1), (unsigned)blocks);
                snprintf(l2, sizeof(l2), "bad blocks %u, ECC corrected %u, uncorrectable %u", (unsigned)bad,
                         (unsigned)corrected, (unsigned)uncorrectable);
                bar(T, l1, l2, b + 1, blocks);
            }
            if (input_cancel_held() && ui_confirm(T, C_YELLOW, "Stop the NAND backup? The unfinished file is deleted."))
                stop = true;
        }
        memcpy(buf, keys, 0x400);
        sha_update(&sha, buf, 0x400);
        sha_finish(&sha, hash);
        bool ok = !stop && !wfail && fwrite(keys, 0x400, 1, fp) == 1;
        ok = (fclose(fp) == 0) && ok;
        fp = NULL;
        if (!ok) {
            remove(npath);
            snprintf(err, sizeof(err), stop ? "Stopped. The unfinished backup was deleted."
                                            : "Couldn't write the backup (is the drive full?). It was deleted.");
        } else {
            char spath[VFS_PATH_MAX + 80];
            snprintf(spath, sizeof(spath), "%s.sha1", npath);
            FILE *s = fopen(spath, "wb");
            if (s) { fwrite(hash, 20, 1, s); fclose(s); }
            const char *base = strrchr(npath, '/') + 1;
            snprintf(spath, sizeof(spath), "%s/sha1sums.txt", dir);
            if ((s = fopen(spath, "a"))) {
                fprintf(s, "%08x%08x%08x%08x%08x *%s\n", (unsigned)hash[0], (unsigned)hash[1], (unsigned)hash[2],
                        (unsigned)hash[3], (unsigned)hash[4], base);
                fclose(s);
            }
            char msg[VFS_PATH_MAX * 2 + 400];
            snprintf(msg, sizeof(msg),
                     "Saved %s\nand %s\n\nSHA-1 %08x%08x%08x%08x%08x\n%u bad block(s), %u page(s) ECC corrected, %u "
                     "uncorrectable, %u read error(s). Took %u s.%s",
                     npath, kpath, (unsigned)hash[0], (unsigned)hash[1], (unsigned)hash[2], (unsigned)hash[3],
                     (unsigned)hash[4], (unsigned)bad, (unsigned)corrected, (unsigned)uncorrectable, (unsigned)ioerr,
                     (unsigned)((ticks_to_millisecs(gettime()) - began) / 1000),
                     kok ? "" : "\n\n(keys.bin couldn't be saved on its own; it is at the end of nand.bin.)");
            ui_message("NAND BACKED UP", uncorrectable || ioerr ? C_YELLOW : C_GREEN, msg);
        }
    }
    if (fp) {
        fclose(fp);
        remove(npath);
    }
    IOS_Close(fl_fd);
    fl_fd = -1;
    free(buf);
    memset(keys, 0, 0x400);
    free(keys);
    vfs_update_space(dev);
    if (!err[0]) return;
fail:
    ui_message(full ? "NAND BACKUP FAILED" : "KEYS BACKUP FAILED", C_RED, err);
}

void nanddump_menu(void)
{
    const char *items[] = { "Whole NAND (nand.bin + keys.bin, about 529 MB)", "Console keys only (keys.bin)" };
    int r = ui_menu("BACK UP THE NAND (nanddumper@IOS)", items, NULL, 2, 0);
    if (r >= 0) run(r == 0);
}

/*
 * DOL / ELF launcher. See launch.h.
 *
 * Ported from YAWM ModMii Edition's appboot.c (GPL-2.0; OverjoY, blackb0x,
 * scooby74029, Peter0x44, XFlak). The booter in appboot/ is theirs too, from
 * Segher Boessenkool's crt0 / link script.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/machine/processor.h>

#include "launch.h"
#include "vfs.h"
#include "ui.h"
#include "archive.h"
#include "runtimeiospatch.h"
#include "appboot_bin.h"

#define APP_ADDR   0x92000000u          /* where the booter looks for the program */
#define ARGS_ADDR  0x91000000u          /* length, then the argument block at +0x20 */
#define APP_MAX    (16 * 1024 * 1024)
#define ARGS_MAX   1024

typedef struct {
    u32 offsetText[7], offsetData[11];
    u32 addressText[7], addressData[11];
    u32 sizeText[7], sizeData[11];
    u32 addressBSS, sizeBSS, entrypoint;
} dolhdr;

static u8 *app;
static u32 app_size;
static char args[ARGS_MAX];             /* in .bss (MEM1), clear of the copy at APP_ADDR */
static u32 args_len;

static bool looks_runnable(const u8 *b, u32 size)
{
    if (size >= 0x34 && !memcmp(b, "\x7F" "ELF", 4))
        /* 32-bit, big endian, PowerPC */
        return b[4] == 1 && b[5] == 2 && be16(b + 0x12) == 20;
    if (size < 0x100)
        return false;
    u32 entry = be32(b + 0xE0);
    if (entry < 0x80000000u || entry >= 0x81800000u)
        return false;
    for (int i = 0; i < 18; i++) {
        u32 off = be32(b + i * 4), sz = be32(b + 0x90 + i * 4);
        if (sz && (off < 0x100 || off > size || sz > size - off))
            return false;
    }
    return true;
}

/* Adds "<arg>" from each <arg>...</arg> in meta.xml's <arguments> block. */
static void add_meta_args(const char *dir)
{
    char path[VFS_PATH_MAX];
    if (!vfs_join(path, sizeof(path), dir, "meta.xml"))
        return;
    vfs_file f;
    if (!vfs_open_read(&f, path))
        return;

    u32 n = f.size < 16384 ? (u32)f.size : 16384;
    char *x = memalign(32, n + 32);
    s32 got = x ? vfs_read(&f, x, n) : -1;
    vfs_close(&f);
    if (got <= 0) {
        free(x);
        return;
    }

    x[got] = '\0';

    char *a = strstr(x, "<arguments>"), *end = a ? strstr(a, "</arguments>") : NULL;
    for (char *p = a; p && end && (p = strstr(p, "<arg>")) && p < end;) {
        p += 5;
        char *q = strstr(p, "</arg>");
        if (!q || q > end) break;
        u32 len = (u32)(q - p);
        if (args_len + len + 2 > ARGS_MAX) break;
        memcpy(args + args_len, p, len);
        args_len += len;
        args[args_len++] = '\0';
        p = q + 6;
    }
    free(x);
}

/* "usb3:/apps/x/boot.dol" -> "usb:/apps/x/boot.dol", as the HBC would name it. */
static void add_path_arg(const char *path)
{
    char p[VFS_PATH_MAX];
    if (!strncmp(path, "usb", 3) && path[3] >= '1' && path[3] <= '8' && path[4] == ':')
        snprintf(p, sizeof(p), "usb%s", path + 4);
    else
        snprintf(p, sizeof(p), "%s", path);
    u32 len = strlen(p);
    if (len + 2 > ARGS_MAX) len = ARGS_MAX - 2;
    memcpy(args, p, len);
    args[len] = '\0';
    args_len = len + 1;
}

bool launch_prepare(const char *path)
{
    free(app);
    app = NULL;
    vfs_file f;
    if (!vfs_open_read(&f, path)) {
        ui_message("LAUNCH", C_RED, "Cannot open the file.");
        return false;
    }
    if (f.size > APP_MAX || f.size < 0x40) {
        vfs_close(&f);
        ui_message("LAUNCH", C_RED, "This file is too big or too small to be a Wii program.");
        return false;
    }

    app_size = (u32)f.size;
    app = memalign(32, (app_size + 31) & ~31u);

    u32 got = 0;
    vfs_unbuffered(&f);
    ui_splash("Loading...");
    while (app && got < app_size) {
        s32 r = vfs_read(&f, app + got, app_size - got > 0x40000 ? 0x40000 : app_size - got);
        if (r <= 0) break;
        got += r;
    }
    vfs_close(&f);
    if (!app || got != app_size || !looks_runnable(app, app_size)) {
        free(app);
        app = NULL;
        ui_message("LAUNCH", C_RED,
                   got != app_size ? "Could not read the whole file."
                                   : "This isn't a Wii DOL or ELF.");
        return false;
    }

    add_path_arg(path);
    char dir[VFS_PATH_MAX];
    if (!vfs_is_archive(path) && vfs_parent(path, dir, sizeof(dir)))
        add_meta_args(dir);
    args[args_len++] = '\0';            /* list terminator */
    return true;
}

void launch_go(void)
{
    if (!app)
        return;
    u32 level = IRQ_Disable();

    /* The program first (it may sit anywhere in MEM2, memmove copes with overlap), then the
     * arguments. */
    memmove((void *)APP_ADDR, app, app_size);
    DCFlushRange((void *)APP_ADDR, app_size);
    *(vu32 *)ARGS_ADDR = args_len;
    memcpy((void *)(ARGS_ADDR + 0x20), args, args_len);
    DCFlushRange((void *)ARGS_ADDR, 0x20 + args_len);

    /* The booter: a DOL linked at 0x81330000. */
    const dolhdr *dol = (const dolhdr *)appboot_bin;
    for (int i = 0; i < 7; i++) {
        if (!dol->sizeText[i] || dol->addressText[i] < 0x100) continue;
        memmove((void *)dol->addressText[i], appboot_bin + dol->offsetText[i], dol->sizeText[i]);
        DCFlushRange((void *)dol->addressText[i], dol->sizeText[i]);
        ICInvalidateRange((void *)dol->addressText[i], dol->sizeText[i]);
    }
    for (int i = 0; i < 11; i++) {
        if (!dol->sizeData[i]) continue;
        memmove((void *)dol->addressData[i], appboot_bin + dol->offsetData[i], dol->sizeData[i]);
        DCFlushRange((void *)dol->addressData[i], dol->sizeData[i]);
    }
    void (*entry)(void) = (void (*)(void))dol->entrypoint;
    IRQ_Restore(level);

    /* Same IOS again, keeping HW_AHBPROT off for the program when we have it. */
    s32 ios = IOS_GetVersion();
    if (AHBPROT_DISABLED && ios > 0 && ios < 200 && !g_is_dolphin)
        IosPatch_AHBPROT(false);
    if (ios > 0)
        __IOS_LaunchNewIOS(ios);
    if (AHBPROT_DISABLED)
        mask32(0x0D800180, 1 << 21, 0);         /* DVD access back on */

    entry();
}

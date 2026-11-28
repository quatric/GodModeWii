/*
 * GodModeWii - leaving for BootMii (as IOS254) or Priiloader.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * Priiloader reads a magic word at 0x8132FFFB when the System Menu starts, as the Homebrew Channel uses it:
 * "Daco" opens Priiloader's menu, "Pune" starts the Wii Menu itself (skipping whatever Priiloader autoboots).
 * Priiloader moves the System Menu's boot content <id>.app to 1<id>.app (low 28 bits), which shows it's there.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/es.h>
#include <ogc/isfs.h>
#include <ogc/ipc.h>

#include "bootopts.h"
#include "vfs.h"

#define SM_TID   0x0000000100000002ull
#define IOS254   0x00000001000000FEull

static u8 *stored_tmd(u64 tid, u32 *size)
{
    u32 sz = 0;
    if (ES_GetStoredTMDSize(tid, &sz) < 0 || !sz || sz > MAX_SIGNED_TMD_SIZE) return NULL;
    u8 *b = memalign(32, (sz + 31) & ~31);
    if (b && ES_GetStoredTMD(tid, (signed_blob *)b, sz) < 0) { free(b); b = NULL; }
    *size = sz;
    return b;
}

bool bootmii_ios_present(void)
{
    if (g_is_vwii) return false;
    u32 sz;
    u8 *t = stored_tmd(IOS254, &sz);
    if (!t) return false;
    u16 ver = t[0x1DC] << 8 | t[0x1DD];
    free(t);
    return ver != 0xFF00;                       /* 65280: the stub */
}

bool priiloader_present(void)
{
    u32 sz;
    u8 *t = stored_tmd(SM_TID, &sz);
    if (!t) return false;
    u16 n = t[0x1DE] << 8 | t[0x1DF], boot = t[0x1E0] << 8 | t[0x1E1];
    u32 cid = 0;
    for (u16 i = 0; i < n && 0x1E4 + (i + 1) * 0x24 <= sz; i++) {
        const u8 *r = t + 0x1E4 + i * 0x24;
        if ((r[4] << 8 | r[5]) == boot) cid = (u32)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
    }
    free(t);
    char path[64];
    snprintf(path, sizeof(path), "/title/00000001/00000002/content/1%07x.app", (unsigned)(cid & 0x0FFFFFFF));
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0) return false;
    ISFS_Close(fd);
    return true;
}

void priiloader_magic(bool menu)
{
    volatile u8 *p = (volatile u8 *)0x8132FFFB;
    const char *w = menu ? "Daco" : "Pune";
    for (int i = 0; i < 4; i++) p[i] = (u8)w[i];
    DCFlushRange((void *)((u32)p & ~31), 64);
}

void bootmii_boot(void)
{
    IOS_ReloadIOS(254);
}

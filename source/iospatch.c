/*
 * GodModeWii - patches the running IOS in MEM2 (needs HW_AHBPROT off).
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * Each patch is a run of bytes found in IOS's code and the bytes written over
 * part of it; the patterns are the well-known ones the Wii homebrew scene has
 * used since 2010 (see docs/CREDITS.md).
 */
#include <string.h>
#include <ogc/cache.h>
#include <ogc/machine/processor.h>

#include "iospatch.h"

typedef struct {
    const u8 *find; u8 find_len;
    u8 at;                       /* where the new bytes go, from the start of the match */
    const u8 *put; u8 put_len;
} patch_t;

#define P(f, a, p) { f, sizeof(f), a, p, sizeof(p) }

static const u8 ahbprot_f[] = { 0x68, 0x5B, 0x22, 0xEC, 0x00, 0x52, 0x18, 0x9B, 0x68, 0x1B, 0x46, 0x98, 0x07, 0xDB };
static const u8 ahbprot_p[] = { 0x01 };
static const u8 isfs_f[]    = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };
static const u8 bal_p[]     = { 0xE0 };   /* Thumb: a conditional branch becomes always */
static const u8 nop_p[]     = { 0x46, 0xC0 };
static const u8 di_f[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x0A, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x7E, 0xD4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08 };
static const u8 di_p[]      = { 0x7E, 0xD4 };
static const u8 hash_f[]    = { 0x20, 0x07, 0x23, 0xA2 };
static const u8 hash2_f[]   = { 0x20, 0x07, 0x4B, 0x0B };
static const u8 zero_p[]    = { 0x00 };
static const u8 version_f[] = { 0xD2, 0x01, 0x4E, 0x56 };
static const u8 sys1_f[]    = { 0x68, 0x1A, 0x2A, 0x01, 0xD0, 0x05 };
static const u8 sys2_f[]    = { 0xD0, 0x02, 0x33, 0x06, 0x42, 0x9A, 0xD1, 0x01 };
static const u8 sys2_p[]    = { 0x46, 0xC0, 0x33, 0x06, 0x42, 0x9A, 0xE0, 0x01 };
static const u8 sys3_f[]    = { 0x68, 0xFB, 0x2B, 0x00, 0xDB, 0x01 };
static const u8 sys3_p[]    = { 0x10 };

/* Patch every match of each pattern; returns the number patched, or -1 without access. */
static int apply(const patch_t *ps, int n)
{
    if (!AHBPROT_DISABLED) return -1;
    write32(0x0D8B420A, read32(0x0D8B420A) & 0xFFFF);   /* MEM2 protection off */
    u8 *lo = (u8 *)*(vu32 *)0x80003134;                  /* where IOS starts in MEM2 */
    u8 *hi = (u8 *)0x94000000;
    int count = 0;
    for (int k = 0; k < n; k++) {
        const patch_t *p = &ps[k];
        for (u8 *m = lo; m + p->find_len <= hi; m++) {
            if (*m != p->find[0] || memcmp(m, p->find, p->find_len)) continue;
            memcpy(m + p->at, p->put, p->put_len);
            DCFlushRange((void *)((u32)(m + p->at) & ~31), 64);
            ICInvalidateRange((void *)((u32)(m + p->at) & ~31), 64);
            count++;
        }
    }
    return count;
}

int iospatch_ahbprot(void)
{
    static const patch_t ps[] = { P(ahbprot_f, 25, ahbprot_p) };
    return apply(ps, 1);
}

int iospatch_isfs(void)
{
    static const patch_t ps[] = { P(isfs_f, 2, bal_p) };
    return apply(ps, 1);
}

int iospatch_di_readlimit(void)
{
    static const patch_t ps[] = { P(di_f, 12, di_p) };
    return apply(ps, 1);
}

int iospatch_wad_install(bool vwii)
{
    static const patch_t ps[] = {
        P(hash_f, 1, zero_p), P(hash2_f, 1, zero_p), P(version_f, 0, bal_p),
        /* vWii: ES refuses to install system titles */
        P(sys1_f, 4, nop_p), P(sys2_f, 0, sys2_p), P(sys3_f, 5, sys3_p),
    };
    return apply(ps, vwii ? 6 : 3);
}

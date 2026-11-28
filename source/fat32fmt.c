/*
 * GodModeWii - format the SD card or a USB drive as FAT32, offered only when it holds something the Homebrew
 * Channel can't load from. See fat32fmt.h.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * One MBR partition (type 0x0C, LBA) from 1 MiB on, and FAT32 laid out as Microsoft's FAT specification
 * describes: 32 reserved sectors (boot sector, FSInfo at 1, backup boot sector at 6), two FATs, the root
 * folder in cluster 2, the cluster size by volume size as Windows picks it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>

#include "fat32fmt.h"

static void le16(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8); }
static void le32(u8 *p, u32 v) { le16(p, v), le16(p + 2, v >> 16); }

/* Zeros over n sectors from lba, in big writes */
static bool zero(fat32_write_fn write, void *io, u32 ss, u64 lba, u64 n, u8 *z, u32 zsec,
                 void (*progress)(u64, u64, void *), void *ctx, u64 *done, u64 total)
{
    while (n) {
        u32 k = n < zsec ? (u32)n : zsec;
        if (!write(io, lba, k, z)) return false;
        lba += k, n -= k, *done += k;
        if (progress) progress(*done, total, ctx);
    }
    (void)ss;
    return true;
}

bool fat32_layout(u64 sectors, u32 ss, fat32_plan *p)
{
    memset(p, 0, sizeof(*p));
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) return false;
    p->ss = ss;
    p->start = (1u << 20) / ss;                                     /* 1 MiB, as every partitioning tool does */
    u64 vol = sectors > p->start ? sectors - p->start : 0;
    if (vol > 0xFFFFFFFFull) vol = 0xFFFFFFFFull;                   /* MBR's limit: the rest stays unused */
    p->vol = (u32)vol;
    u64 bytes = vol * ss;
    u32 cluster = bytes <= (8ull << 30) ? 4096 : bytes <= (16ull << 30) ? 8192 : bytes <= (32ull << 30) ? 16384 : 32768;
    p->spc = cluster < ss ? 1 : cluster / ss;
    p->rsvd = 32;
    u32 fat = 1;
    for (int i = 0; i < 8; i++) {                                    /* FAT size and cluster count settle quickly */
        u64 clusters = (vol - p->rsvd - 2ull * fat) / p->spc;
        fat = (u32)(((clusters + 2) * 4 + ss - 1) / ss);
    }
    p->fat = fat;
    p->clusters = (u32)((vol - p->rsvd - 2ull * fat) / p->spc);
    return p->clusters >= 65525 && p->clusters < 0x0FFFFFF5;      /* what makes it FAT32 and not FAT16 */
}

bool fat32_format(fat32_write_fn write, void *io, u64 sectors, u32 ss, const char *label,
                  void (*progress)(u64 done, u64 total, void *ctx), void *ctx, char *err, size_t en)
{
    fat32_plan p;
    if (!fat32_layout(sectors, ss, &p)) {
        snprintf(err, en, "The drive is too small or too big for FAT32 (or has %u-byte sectors).", (unsigned)ss);
        return false;
    }
    const u32 zsec = (256u * 1024) / ss;
    u8 *z = memalign(64, zsec * ss), *s = memalign(64, ss);
    if (!z || !s) { free(z); free(s); snprintf(err, en, "Out of memory."); return false; }
    memset(z, 0, zsec * ss);
    u64 fat0 = p.start + p.rsvd, root = fat0 + 2ull * p.fat, done = 0;
    u64 total = p.start + p.rsvd + 2ull * p.fat + p.spc + 34;
    bool ok = true;
    /* old partition tables and GPT (front and back), the reserved area, both FATs and the root folder: zeros */
    ok = ok && zero(write, io, ss, 0, p.start, z, zsec, progress, ctx, &done, total);
    ok = ok && zero(write, io, ss, sectors - 34, 34, z, zsec, progress, ctx, &done, total);
    ok = ok && zero(write, io, ss, p.start, p.rsvd + 2ull * p.fat + p.spc, z, zsec, progress, ctx, &done, total);

    u32 serial = (u32)time(NULL) * 2654435761u;
    if (ok) {                                                        /* the MBR */
        memset(s, 0, ss);
        le32(s + 440, serial);
        u8 *e = s + 446;
        e[1] = 0xFE, e[2] = 0xFF, e[3] = 0xFF, e[4] = 0x0C, e[5] = 0xFE, e[6] = 0xFF, e[7] = 0xFF;    /* CHS: use LBA */
        le32(e + 8, p.start);
        le32(e + 12, p.vol);
        s[510] = 0x55, s[511] = 0xAA;
        ok = write(io, 0, 1, s);
    }
    if (ok) {                                                        /* the boot sector, and its backup at 6 */
        memset(s, 0, ss);
        s[0] = 0xEB, s[1] = 0x58, s[2] = 0x90;
        memcpy(s + 3, "MSWIN4.1", 8);
        le16(s + 11, ss);
        s[13] = (u8)p.spc;
        le16(s + 14, p.rsvd);
        s[16] = 2;                                                   /* FATs */
        s[21] = 0xF8;                                                /* fixed disk */
        le16(s + 24, 63), le16(s + 26, 255);
        le32(s + 28, p.start);
        le32(s + 32, p.vol);
        le32(s + 36, p.fat);
        le32(s + 44, 2);                                             /* root folder cluster */
        le16(s + 48, 1), le16(s + 50, 6);                            /* FSInfo, backup boot sector */
        s[64] = 0x80, s[66] = 0x29;
        le32(s + 67, serial);
        char lab[12];
        snprintf(lab, sizeof(lab), "%-11.11s", label && *label ? label : "NO NAME");
        memcpy(s + 71, lab, 11);
        memcpy(s + 82, "FAT32   ", 8);
        s[510] = 0x55, s[511] = 0xAA;
        ok = write(io, p.start, 1, s) && write(io, p.start + 6, 1, s);
    }
    if (ok) {                                                        /* FSInfo, and its backup at 7 */
        memset(s, 0, ss);
        le32(s, 0x41615252);
        le32(s + 484, 0x61417272);
        le32(s + 488, p.clusters - 1);                               /* free: all but the root folder's */
        le32(s + 492, 3);
        le32(s + 508, 0xAA550000);
        ok = write(io, p.start + 1, 1, s) && write(io, p.start + 7, 1, s);
    }
    for (int f = 0; ok && f < 2; f++) {                               /* each FAT: media, end mark, root folder */
        memset(s, 0, ss);
        le32(s, 0x0FFFFFF8), le32(s + 4, 0x0FFFFFFF), le32(s + 8, 0x0FFFFFFF);
        ok = write(io, fat0 + (u64)f * p.fat, 1, s);
    }
    if (ok && label && *label) {                                     /* the volume label in the root folder too */
        memset(s, 0, ss);
        char lab[12];
        snprintf(lab, sizeof(lab), "%-11.11s", label);
        memcpy(s, lab, 11);
        s[11] = 0x08;
        ok = write(io, root, 1, s);
    }
    if (!ok) snprintf(err, en, "Writing to the drive failed.");
    free(z);
    free(s);
    return ok;
}

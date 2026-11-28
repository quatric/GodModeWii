/*
 * GodModeWii - WBFS partitions (the old USB loader format), read-only.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * The layout as libwbfs (Kwiirk, Waninkoko and others) writes it: a head sector ("WBFS", the number of drive
 * sectors, log2 of the drive's and of WBFS's sector size, then one byte per disc slot), and for each disc a block
 * after it with the disc's first 0x100 bytes and a table of 16-bit WBFS sector numbers, one per WBFS-sector-sized
 * piece of the disc (0: never written, reads as zeros). Each game shows up as "<ID6> <title>.iso".
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "archive.h"

#define WII_SEC_S       15                  /* 32 KiB */
#define WII_SECS_DISC   (143432u * 2)       /* a dual-layer disc in Wii sectors */
#define SL_SIZE         4699979776ull
#define DL_SIZE         8511160320ull
#define README          0xFFFFu

static const char readme[] =
    "This drive is in WBFS, the format old USB loaders used before they could read games from FAT32 or NTFS.\n"
    "GodModeWii only reads it. Nothing current needs it: copy the games off (they open as .iso), then format the\n"
    "drive FAT32 (or NTFS) and put them back as .wbfs or .iso files in /wbfs/ for USB Loader GX, WiiFlow and others.\n";

typedef struct {
    uint32_t wbfs_s, per_disc;
    int n;
    uint8_t **table;                        /* each disc's sector table, big-endian 16-bit entries */
} wbfs;

bool arc_wbfs_probe(const src_t *file, const char *name)
{
    /* a .wbfs file is the same format with one game in it: the disc image reader opens those straight into the game */
    size_t nl = name ? strlen(name) : 0;
    if (nl >= 5 && (!strcasecmp(name + nl - 5, ".wbfs") || !strcasecmp(name + nl - 5, ".wbf1"))) return false;
    uint8_t h[12];
    if (file->size < 0x100000 || !src_read_full(file, 0, h, sizeof(h)) || memcmp(h, "WBFS", 4)) return false;
    return h[8] >= 9 && h[8] <= 12 && h[9] >= WII_SEC_S && h[9] <= 30 && h[9] >= h[8];
}

static int32_t wbfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    wbfs *w = a->priv;
    if (n->c == README) {
        if (off >= sizeof(readme) - 1) return 0;
        uint32_t k = (uint32_t)(sizeof(readme) - 1 - off) < len ? (uint32_t)(sizeof(readme) - 1 - off) : len;
        memcpy(buf, readme + off, k);
        return (int32_t)k;
    }
    const uint8_t *t = w->table[n->c];
    uint64_t ss = 1ull << w->wbfs_s;
    uint32_t done = 0;
    while (done < len && off + done < n->size) {
        uint64_t o = off + done;
        uint32_t blk = (uint32_t)(o >> w->wbfs_s), in = (uint32_t)(o & (ss - 1));
        uint64_t k = ss - in;
        if (k > len - done) k = len - done;
        if (k > n->size - o) k = n->size - o;
        uint16_t wl = blk < w->per_disc ? (uint16_t)(t[blk * 2] << 8 | t[blk * 2 + 1]) : 0;
        if (!wl) memset((uint8_t *)buf + done, 0, (size_t)k);
        else if (!src_read_full(&a->src, (uint64_t)wl * ss + in, (uint8_t *)buf + done, (uint32_t)k)) return -1;
        done += (uint32_t)k;
    }
    return (int32_t)done;
}

static void wbfs_destroy(archive_t *a)
{
    wbfs *w = a->priv;
    if (!w) return;
    for (int i = 0; i < w->n; i++) free(w->table[i]);
    free(w->table);
    free(w);
}

archive_t *arc_wbfs_open(const src_t *file, const char *name)
{
    if (!arc_wbfs_probe(file, name)) return NULL;
    uint8_t h0[12];
    src_read_full(file, 0, h0, sizeof(h0));
    uint32_t hd_s = h0[8], wbfs_s = h0[9], hd_sz = 1u << hd_s;
    uint64_t n_hd = (uint32_t)(h0[4] << 24 | h0[5] << 16 | h0[6] << 8 | h0[7]);
    uint8_t *head = malloc(hd_sz);
    wbfs *w = calloc(1, sizeof(*w));
    archive_t *a = NULL;
    if (!head || !w || !src_read_full(file, 0, head, hd_sz)) goto fail;
    w->wbfs_s = wbfs_s;
    w->per_disc = WII_SECS_DISC >> (wbfs_s - WII_SEC_S);
    uint64_t n_wii = n_hd * hd_sz >> WII_SEC_S, n_wbfs = n_wii >> (wbfs_s - WII_SEC_S);
    uint32_t info_sz = (0x100 + w->per_disc * 2 + hd_sz - 1) & ~(hd_sz - 1);
    int64_t free_lba = (int64_t)(((1ull << wbfs_s) - n_wbfs / 8) >> hd_s);
    int max = free_lba > 1 ? (int)((free_lba - 1) / (info_sz >> hd_s)) : 0;
    if (max > (int)hd_sz - 12) max = (int)hd_sz - 12;
    if (!(w->table = calloc(max ? max : 1, sizeof(uint8_t *)))) goto fail;
    if (!(a = archive_alloc("WBFS"))) goto fail;
    tree_add(&a->tree, 0, "READ ME - WBFS is an old format.txt", false, sizeof(readme) - 1, 0, 0, README);
    uint8_t *info = malloc(info_sz);
    for (int i = 0; info && i < max; i++) {
        if (!head[12 + i]) continue;
        uint64_t at = (uint64_t)(1 + (uint64_t)i * (info_sz >> hd_s)) << hd_s;
        if (!src_read_full(file, at, info, info_sz)) continue;
        uint8_t *t = malloc(w->per_disc * 2);
        if (!t) break;
        memcpy(t, info + 0x100, w->per_disc * 2);
        uint32_t last = 0;
        for (uint32_t k = 0; k < w->per_disc; k++) if (t[k * 2] | t[k * 2 + 1]) last = k + 1;
        uint64_t used = (uint64_t)last << wbfs_s;
        char nm[96], title[65];
        memcpy(title, info + 0x20, 64);
        title[64] = 0;
        snprintf(nm, sizeof(nm), "%.6s %s.iso", (const char *)info, title);
        tree_clean_name(nm);
        w->table[w->n] = t;
        tree_add(&a->tree, 0, nm, false, used > SL_SIZE ? DL_SIZE : SL_SIZE, 0, 0, (uint32_t)w->n);
        w->n++;
    }
    free(info);
    free(head);
    a->priv = w;
    a->read = wbfs_read;
    a->destroy = wbfs_destroy;
    a->base = *file;
    a->src = a->base;
    return a;
fail:
    if (w) { free(w->table); free(w); }
    free(head);
    g_archive_err = "Can't read the WBFS partition";
    return NULL;
}

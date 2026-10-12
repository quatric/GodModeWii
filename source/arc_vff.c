/*
 * VFF: the small FAT12 / FAT16 volume the Wii uses for WiiConnect24 and Mii data
 * (wc24 mailboxes, Check Mii Out, ...). Layout from marcan's vffdump.py and
 * wii-tools/vffmod: a 0x20 byte header, two FATs, a 0x1000 byte root directory,
 * then the data clusters. Read-only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"

#define VFF_MAGIC 0x56464620u           /* "VFF " */
#define VFF_ROOT_SIZE 0x1000u
#define MAX_ENTRIES 20000
#define MAX_DIR_BYTES (4u * 1024 * 1024)

typedef struct {
    uint32_t cluster_size;
    uint32_t cluster_count;
    int fat_bits;                       /* 12 or 16 */
    uint8_t *fat;
    uint32_t fat_bytes;
    uint64_t data_off;
    int entries;
} vffpriv;

static uint32_t fat_get(const vffpriv *v, uint32_t n)
{
    if (v->fat_bits == 16) {
        if ((uint64_t)n * 2 + 2 > v->fat_bytes) return 0xFFFF;
        return v->fat[n * 2] | (v->fat[n * 2 + 1] << 8);
    }
    uint32_t off = (n / 2) * 3;
    if ((uint64_t)off + 3 > v->fat_bytes) return 0xFFF;
    if (n & 1) return (v->fat[off + 1] >> 4) | (v->fat[off + 2] << 4);
    return v->fat[off] | ((v->fat[off + 1] & 0xF) << 8);
}

static bool fat_used(const vffpriv *v, uint32_t c)
{
    uint32_t rsvd = v->fat_bits == 16 ? 0xFFF0 : 0xFF0;
    return c >= 2 && c < rsvd;
}

/* The chain of start as one buffer (directories); NULL on a broken chain. */
static uint8_t *read_chain(const archive_t *a, const vffpriv *v, uint32_t start, uint32_t *out_len)
{
    uint32_t n = 0, c = start;
    while (fat_used(v, c)) {
        if (++n > v->cluster_count || (uint64_t)n * v->cluster_size > MAX_DIR_BYTES) return NULL;
        c = fat_get(v, c);
    }
    if (!n) return NULL;
    uint8_t *buf = malloc((size_t)n * v->cluster_size);
    if (!buf) return NULL;
    c = start;
    for (uint32_t i = 0; i < n; i++) {
        if (!src_read_full(&a->src, v->data_off + (uint64_t)(c - 2) * v->cluster_size, buf + (size_t)i * v->cluster_size, v->cluster_size)) {
            free(buf);
            return NULL;
        }
        c = fat_get(v, c);
    }
    *out_len = n * v->cluster_size;
    return buf;
}

static void entry_name(const uint8_t *d, char *out)
{
    char base[9], ext[4];
    memcpy(base, d, 8); base[8] = '\0';
    memcpy(ext, d + 8, 3); ext[3] = '\0';
    for (int i = 7; i >= 0 && base[i] == ' '; i--) base[i] = '\0';
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = '\0';
    if (d[12] & 0x08) for (char *p = base; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (d[12] & 0x10) for (char *p = ext; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (base[0] == 0x05) base[0] = (char)0xE5;
    if (ext[0]) snprintf(out, 20, "%s.%s", base, ext);
    else snprintf(out, 20, "%s", base);
}

static bool walk_dir(archive_t *a, vffpriv *v, const uint8_t *dir, uint32_t len, int parent, int depth)
{
    if (depth > 12) return true;
    for (uint32_t off = 0; off + 32 <= len; off += 32) {
        const uint8_t *d = dir + off;
        if (d[0] == 0x00) continue;                 /* vffdump keeps going past empty slots */
        if (d[0] == 0xE5) continue;
        uint8_t attr = d[11];
        if ((attr & 0x0F) == 0x0F) continue;        /* long name piece */
        if (attr & 0x08) continue;                  /* volume label */
        char name[24];
        entry_name(d, name);
        if (!strcmp(name, ".") || !strcmp(name, "..") || !name[0]) continue;
        uint32_t start = d[26] | (d[27] << 8);
        uint32_t size = (uint32_t)d[28] | ((uint32_t)d[29] << 8) | ((uint32_t)d[30] << 16) | ((uint32_t)d[31] << 24);
        if (++v->entries > MAX_ENTRIES) return false;
        if (attr & 0x10) {
            int idx = tree_add(&a->tree, parent, name, true, 0, 0, 0, 0);
            if (idx < 0) return false;
            uint32_t dl;
            uint8_t *sub = fat_used(v, start) ? read_chain(a, v, start, &dl) : NULL;
            if (sub) {
                bool ok = walk_dir(a, v, sub, dl, idx, depth + 1);
                free(sub);
                if (!ok) return false;
            }
        } else {
            if (tree_add(&a->tree, parent, name, false, size, start, 0, 0) < 0) return false;
        }
    }
    return true;
}

static int32_t vff_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    vffpriv *v = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    uint32_t cs = v->cluster_size;
    uint32_t c = (uint32_t)n->a;
    for (uint64_t skip = off / cs; skip; skip--) {          /* walk to the cluster holding off */
        if (!fat_used(v, c)) return -1;
        c = fat_get(v, c);
    }
    uint8_t *dst = buf;
    uint32_t done = 0;
    uint32_t in_cluster = (uint32_t)(off % cs);
    while (done < len) {
        if (!fat_used(v, c)) return done ? (int32_t)done : -1;
        /* extend over clusters that follow each other so they are read in one go */
        uint32_t run = 1, last = c;
        while ((uint64_t)run * cs - in_cluster < len - done) {
            uint32_t nx = fat_get(v, last);
            if (nx != last + 1 || !fat_used(v, nx)) break;
            last = nx;
            run++;
        }
        uint32_t want = run * cs - in_cluster;
        if (want > len - done) want = len - done;
        if (!src_read_full(&a->src, v->data_off + (uint64_t)(c - 2) * cs + in_cluster, dst + done, want))
            return done ? (int32_t)done : -1;
        done += want;
        in_cluster = 0;
        c = fat_get(v, last);
    }
    return (int32_t)done;
}

static void vff_destroy(archive_t *a)
{
    vffpriv *v = a->priv;
    if (v) { free(v->fat); free(v); }
}

bool arc_vff_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x10];
    if (file->size < 0x1000 || !src_read_full(file, 0, h, sizeof(h))) return false;
    return be32(h) == VFF_MAGIC;
}

archive_t *arc_vff_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x20];
    if (!src_read_full(file, 0, h, sizeof(h)) || be32(h) != VFF_MAGIC) return NULL;
    uint32_t volsize = be32(h + 8);
    uint32_t cs = (uint32_t)be16(h + 12) * 16;
    if (cs < 512 || cs > 64 * 1024 || (cs & (cs - 1)) || !volsize) return NULL;
    uint32_t count = volsize / cs;
    if (count < 2 || count > 0xFFF5) return NULL;           /* FAT32 isn't used */

    vffpriv *v = calloc(1, sizeof(*v));
    archive_t *a = archive_alloc("VFF");
    if (!v || !a) { free(v); archive_close(a); return NULL; }
    a->priv = v;
    a->destroy = vff_destroy;
    a->read = vff_read;
    v->cluster_size = cs;
    v->cluster_count = count;
    uint32_t fat_raw;
    if (count > 0xFF5) { v->fat_bits = 16; fat_raw = count * 2; }
    else { v->fat_bits = 12; fat_raw = ((count + 1) / 2) * 3; }
    v->fat_bytes = (fat_raw + cs - 1) & ~(cs - 1);
    v->data_off = 0x20 + 2 * (uint64_t)v->fat_bytes + VFF_ROOT_SIZE;
    v->fat = malloc(v->fat_bytes);
    uint8_t *root = malloc(VFF_ROOT_SIZE);
    a->base = *file;
    a->src = a->base;
    bool ok = v->fat && root && src_read_full(file, 0x20, v->fat, v->fat_bytes) &&
              src_read_full(file, 0x20 + 2 * (uint64_t)v->fat_bytes, root, VFF_ROOT_SIZE);
    if (ok) {
        /* the first FAT entry is a reserved marker; anything else isn't a VFF */
        uint32_t f0 = fat_get(v, 0);
        ok = v->fat_bits == 16 ? f0 >= 0xFFF0 : f0 >= 0xFF0;
    }
    if (ok) ok = walk_dir(a, v, root, VFF_ROOT_SIZE, 0, 0);
    free(root);
    if (!ok) {
        memset(&a->base, 0, sizeof(a->base));
        memset(&a->src, 0, sizeof(a->src));
        archive_close(a);
        return NULL;
    }
    return a;
}

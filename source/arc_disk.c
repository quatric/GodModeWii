/*
 * Partition tables of disk images and raw drives, read-only: MBR (with extended / logical partitions),
 * GPT and the Apple partition map. Each partition appears as a file ("1_Linux.part") that opens as
 * whatever file system it holds (ext2/3/4, XFS, HFS+, APFS, ZFS, ...) or as a plain image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"

typedef struct { uint64_t start, size; char label[48]; } part_t;

#define MAX_PARTS 128

static const char *mbr_type(uint8_t t)
{
    switch (t) {
    case 0x01: case 0x04: case 0x06: case 0x0E: return "FAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS-exFAT";
    case 0x82: return "swap";
    case 0x83: return "Linux";
    case 0x8E: return "LVM";
    case 0xA8: return "Apple-UFS";
    case 0xAB: return "Apple-boot";
    case 0xAF: return "Apple-HFS";
    case 0xEE: return "GPT";
    case 0xEF: return "EFI";
    case 0xFD: return "RAID";
    case 0xA5: case 0xA6: case 0xA9: return "BSD";
    default: return "partition";
    }
}

static bool fat_or_ntfs_boot(const uint8_t *s)
{
    return !memcmp(s + 3, "NTFS    ", 8) || !memcmp(s + 3, "EXFAT   ", 8) || !memcmp(s + 0x36, "FAT", 3) || !memcmp(s + 0x52, "FAT32", 5);
}

static bool mbr_valid(const uint8_t *s, uint64_t size, int *used, bool *only_gpt)
{
    if (s[510] != 0x55 || s[511] != 0xAA || fat_or_ntfs_boot(s)) return false;
    int n = 0, gpt = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        if (e[0] != 0 && e[0] != 0x80) return false;
        uint32_t start = le32(e + 8), cnt = le32(e + 12);
        if (!e[4] || !cnt) continue;
        if ((uint64_t)start * 512 >= size) return false;
        n++;
        if (e[4] == 0xEE) gpt++;
    }
    *used = n;
    *only_gpt = n > 0 && gpt == n;
    return n > 0;
}

static int gpt_ss(const src_t *f, uint8_t *h)
{
    static const int sizes[] = { 512, 4096 };
    for (int i = 0; i < 2; i++)
        if (f->size > (uint64_t)sizes[i] * 4 && src_read_full(f, sizes[i], h, 92) && !memcmp(h, "EFI PART", 8)) return sizes[i];
    return 0;
}

static bool apm_valid(const src_t *f)
{
    uint8_t h[512];
    if (f->size < 4096 || !src_read_full(f, 0, h, 512) || h[0] != 'E' || h[1] != 'R') return false;
    uint8_t p[512];
    return src_read_full(f, 512, p, 512) && p[0] == 'P' && p[1] == 'M';
}

bool arc_disk_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[512];
    if (file->size < 4096) return false;
    if (gpt_ss(file, h)) return true;
    if (apm_valid(file)) return true;
    if (!src_read_full(file, 0, h, 512)) return false;
    int used;
    bool only_gpt;
    return mbr_valid(h, file->size, &used, &only_gpt) && !only_gpt;
}

static void add_part(part_t *p, int *n, uint64_t start, uint64_t size, const char *label, uint64_t total)
{
    if (*n >= MAX_PARTS || !size || start >= total) return;
    if (start + size > total) size = total - start;
    p[*n].start = start;
    p[*n].size = size;
    snprintf(p[*n].label, sizeof(p[*n].label), "%s", label);
    (*n)++;
}

static int read_mbr(const src_t *f, const uint8_t *s, part_t *p)
{
    int n = 0;
    uint64_t ext_start = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        uint8_t t = e[4];
        if (!t || t == 0xEE) continue;
        uint64_t start = (uint64_t)le32(e + 8) * 512, size = (uint64_t)le32(e + 12) * 512;
        if (t == 0x05 || t == 0x0F || t == 0x85) { if (!ext_start) ext_start = start; continue; }
        add_part(p, &n, start, size, mbr_type(t), f->size);
    }
    if (ext_start) {                                            /* the chain of logical partitions */
        uint64_t ebr = ext_start;
        for (int guard = 0; guard < 64; guard++) {
            uint8_t b[512];
            if (ebr + 512 > f->size || !src_read_full(f, ebr, b, 512) || b[510] != 0x55 || b[511] != 0xAA) break;
            const uint8_t *e0 = b + 446, *e1 = b + 446 + 16;
            if (e0[4]) add_part(p, &n, ebr + (uint64_t)le32(e0 + 8) * 512, (uint64_t)le32(e0 + 12) * 512, mbr_type(e0[4]), f->size);
            if (!e1[4] || !le32(e1 + 8)) break;
            ebr = ext_start + (uint64_t)le32(e1 + 8) * 512;
        }
    }
    return n;
}

static const char *gpt_type(const uint8_t *g)
{
    /* mixed endian GUIDs: first three fields little endian */
    static const struct { uint8_t g[16]; const char *name; } known[] = {
        { { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, "Linux" },
        { { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }, "Data" },
        { { 0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC }, "HFS+" },
        { { 0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC }, "APFS" },
        { { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, "EFI" },
        { { 0xC3, 0x8C, 0x89, 0x6A, 0xD2, 0x1D, 0xB2, 0x11, 0x99, 0xA6, 0x08, 0x00, 0x20, 0x73, 0x66, 0x31 }, "ZFS" },
        { { 0xBA, 0x7C, 0x6E, 0x51, 0xCF, 0x6E, 0xD6, 0x11, 0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B }, "ZFS" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (!memcmp(g, known[i].g, 16)) return known[i].name;
    return "partition";
}

static int read_gpt(const src_t *f, int ss, const uint8_t *h, part_t *p)
{
    uint64_t lba = le64(h + 72);
    uint32_t count = le32(h + 80), esz = le32(h + 84);
    if (esz < 128 || esz > 4096 || !count) return 0;
    if (count > 256) count = 256;
    uint8_t *buf = malloc(esz);
    if (!buf) return 0;
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!src_read_full(f, lba * ss + (uint64_t)i * esz, buf, esz)) break;
        bool empty = true;
        for (int k = 0; k < 16 && empty; k++) empty = buf[k] == 0;
        if (empty) continue;
        uint64_t first = le64(buf + 32), last = le64(buf + 40);
        if (last < first) continue;
        char label[48];
        const char *type = gpt_type(buf);
        uint16_t u[36];
        int ul = 0;
        for (; ul < 36 && le16(buf + 56 + ul * 2); ul++) u[ul] = le16(buf + 56 + ul * 2);
        char nm[48];
        fs_utf16_to_utf8(u, (size_t)ul, nm, sizeof(nm));
        tree_clean_name(nm);
        snprintf(label, sizeof(label), "%s", nm[0] ? nm : type);
        add_part(p, &n, first * ss, (last - first + 1) * ss, label, f->size);
    }
    free(buf);
    return n;
}

static int read_apm(const src_t *f, part_t *p)
{
    uint8_t h[512];
    if (!src_read_full(f, 0, h, 512)) return 0;
    uint32_t bsz = be16(h + 2);
    if (bsz < 512 || bsz > 4096) bsz = 512;
    int n = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < 128; i++) {
        uint8_t e[512];
        if (!src_read_full(f, (uint64_t)(i + 1) * bsz, e, 512) || e[0] != 'P' || e[1] != 'M') break;
        if (!i) count = be32(e + 4);
        char type[33];
        memcpy(type, e + 48, 32);
        type[32] = 0;
        if (!strncmp(type, "Apple_partition_map", 19) || !strncmp(type, "Apple_Free", 10) || !strncmp(type, "Apple_Driver", 12) ||
            !strncmp(type, "Apple_Void", 10) || !strncmp(type, "Apple_Patches", 13))
            { if (i + 1 >= count) break; continue; }
        const char *t = !strncmp(type, "Apple_", 6) ? type + 6 : type;
        add_part(p, &n, (uint64_t)be32(e + 8) * bsz, (uint64_t)be32(e + 12) * bsz, t, f->size);
        if (i + 1 >= count) break;
    }
    return n;
}

static int32_t disk_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    return a->src.read(a->src.ctx, n->a + off, buf, len);
}

archive_t *arc_disk_open(const src_t *file, const char *name)
{
    (void)name;
    part_t *parts = calloc(MAX_PARTS, sizeof(part_t));
    if (!parts) return NULL;
    uint8_t h[512];
    int n = 0, ss;
    const char *kind = "MBR";
    if ((ss = gpt_ss(file, h)) != 0) { n = read_gpt(file, ss, h, parts); kind = "GPT"; }
    else if (apm_valid(file)) { n = read_apm(file, parts); kind = "Apple partition map"; }
    else if (src_read_full(file, 0, h, 512)) n = read_mbr(file, h, parts);
    if (!n) { free(parts); g_archive_err = "No partitions found"; return NULL; }
    archive_t *a = archive_alloc(kind);
    if (!a) { free(parts); return NULL; }
    for (int i = 0; i < n; i++) {
        char fname[96];
        snprintf(fname, sizeof(fname), "%d_%s.part", i + 1, parts[i].label);
        tree_clean_name(fname);
        tree_add(&a->tree, 0, fname, false, parts[i].size, parts[i].start, 0, 0);
    }
    free(parts);
    a->read = disk_read;
    a->base = *file;
    a->src = a->base;
    return a;
}

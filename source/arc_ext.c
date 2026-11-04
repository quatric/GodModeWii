/*
 * ext2 / ext3 / ext4, read-only, from a disk image or a partition: superblock and group descriptors,
 * inodes (128 byte and larger), extent trees and the classic block map, linear and hashed directories
 * (read as plain lists of entries), inline data for small files. The journal is ignored, so a volume
 * that was not cleanly unmounted shows what was last written to the main area.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"

#define EXT_MAGIC 0xEF53

#define INC_COMPRESSION 0x0001u
#define INC_META_BG     0x0010u
#define INC_64BIT       0x0080u
#define INC_INLINE_DATA 0x8000u
#define INC_ENCRYPT     0x10000u
#define INC_UNSUPPORTED (INC_COMPRESSION)

#define FL_EXTENTS 0x80000u
#define FL_INLINE  0x10000000u

typedef struct {
    fsc_t c;
    uint32_t bs, ipg, bpg, isz, first_data, dsz, incompat;
    uint64_t blocks, groups;
    uint8_t *gd;                /* group descriptors, as read */
    uint32_t ino_cached;        /* inode in `ino` */
    uint8_t ino[512];
} extfs;

static bool ext_probe_sb(const src_t *f, uint8_t sb[1024])
{
    if (f->size < 4096 || !src_read_full(f, 1024, sb, 1024)) return false;
    if (le16(sb + 0x38) != EXT_MAGIC) return false;
    uint32_t lbs = le32(sb + 0x18);
    if (lbs > 6 || !le32(sb + 0x20) || !le32(sb + 0x28)) return false;
    return true;
}

bool arc_ext_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t sb[1024];
    return ext_probe_sb(file, sb);
}

static bool ext_inode(extfs *x, uint32_t n, uint8_t *out)
{
    if (!n) return false;
    if (x->ino_cached == n) { memcpy(out, x->ino, x->isz); return true; }
    uint64_t grp = (n - 1) / x->ipg, idx = (n - 1) % x->ipg;
    if (grp >= x->groups) return false;
    const uint8_t *g = x->gd + grp * x->dsz;
    uint64_t table = le32(g + 8);
    if (x->dsz >= 64) table |= (uint64_t)le32(g + 0x28) << 32;
    uint64_t off = table * x->bs + idx * x->isz;
    if (!fsc_read(&x->c, off, x->ino, x->isz)) return false;
    x->ino_cached = n;
    memcpy(out, x->ino, x->isz);
    return true;
}

static uint64_t ext_isize(const uint8_t *in)
{
    uint16_t mode = le16(in);
    uint64_t sz = le32(in + 4);
    if ((mode & 0xF000) == 0x8000) sz |= (uint64_t)le32(in + 0x6C) << 32;
    return sz;
}

/* logical block -> physical block; *run = how many following logical blocks are contiguous (>= 1).
 * phys 0 = a hole. */
static bool ext_map(extfs *x, const uint8_t *in, uint64_t lblk, uint64_t *phys, uint32_t *run)
{
    uint32_t flags = le32(in + 0x20);
    const uint8_t *iblock = in + 0x28;
    *phys = 0;
    *run = 1;
    if (flags & FL_EXTENTS) {
        uint8_t node[4096];
        const uint8_t *p = iblock;
        uint32_t depth_left = 8;
        for (;;) {
            if (le16(p) != 0xF30A || depth_left-- == 0) return false;
            uint32_t entries = le16(p + 2), depth = le16(p + 6);
            if (depth == 0) {
                for (uint32_t i = 0; i < entries; i++) {
                    const uint8_t *e = p + 12 + i * 12;
                    uint64_t first = le32(e);
                    uint32_t len = le16(e + 4);
                    if (len > 32768) len -= 32768;
                    if (lblk >= first && lblk < first + len) {
                        *phys = (le32(e + 8) | (uint64_t)le16(e + 6) << 32) + (lblk - first);
                        *run = (uint32_t)(first + len - lblk);
                        return true;
                    }
                }
                return true;                                    /* a hole */
            }
            const uint8_t *pick = NULL;
            for (uint32_t i = 0; i < entries; i++) {
                const uint8_t *e = p + 12 + i * 12;
                if (le32(e) <= lblk) pick = e;
            }
            if (!pick) return true;
            uint64_t leaf = le32(pick + 4) | (uint64_t)le16(pick + 8) << 32;
            if (x->bs > sizeof(node) || !fsc_read(&x->c, leaf * x->bs, node, x->bs)) return false;
            p = node;
            /* `node` is reused on the next level: the entries were already picked from it */
        }
    }
    /* classic block map */
    uint32_t per = x->bs / 4;
    uint8_t blk[4096];
    if (x->bs > sizeof(blk)) return false;
    if (lblk < 12) { *phys = le32(iblock + lblk * 4); return true; }
    lblk -= 12;
    int level;
    uint64_t span = per;
    if (lblk < per) level = 1;
    else if ((lblk -= per) < (uint64_t)per * per) { level = 2; span = (uint64_t)per * per; }
    else { lblk -= (uint64_t)per * per; level = 3; span = (uint64_t)per * per * per; }
    if (lblk >= span) return true;
    uint64_t cur = le32(iblock + (11 + level) * 4);
    for (int l = level; l >= 1 && cur; l--) {
        uint64_t div = 1;
        for (int k = 1; k < l; k++) div *= per;
        uint32_t idx = (uint32_t)((lblk / div) % per);
        uint8_t w[4];
        if (!fsc_read(&x->c, cur * x->bs + idx * 4, w, 4)) return false;
        cur = le32(w);
    }
    *phys = cur;
    return true;
}

/* bytes [off, off+len) of an inode's data */
static int32_t ext_data(extfs *x, const uint8_t *in, uint64_t size, uint64_t off, uint8_t *buf, uint32_t len)
{
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    bool fast_link = (le16(in) & 0xF000) == 0xA000 && size < 60 && !le32(in + 0x1C);   /* the target sits in i_block */
    if ((le32(in + 0x20) & FL_INLINE) || fast_link) {            /* inline data: the 60 bytes of i_block */
        uint8_t tmp[60];
        memcpy(tmp, in + 0x28, 60);
        if (off >= 60) { memset(buf, 0, len); return (int32_t)len; }
        uint32_t n = 60 - (uint32_t)off;
        if (n > len) n = len;
        memcpy(buf, tmp + off, n);
        if (n < len) memset(buf + n, 0, len - n);
        return (int32_t)len;
    }
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, lblk = pos / x->bs;
        uint32_t skew = (uint32_t)(pos % x->bs), run;
        uint64_t phys;
        if (!ext_map(x, in, lblk, &phys, &run)) return done ? (int32_t)done : -1;
        uint32_t n = run * x->bs - skew;
        if (n > len - done) n = len - done;
        if (!phys) memset(buf + done, 0, n);
        else if (!fsc_read(&x->c, phys * x->bs + skew, buf + done, n)) return done ? (int32_t)done : -1;
        done += n;
    }
    return (int32_t)done;
}

static int32_t ext_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    extfs *x = a->priv;
    uint8_t in[512];
    if (!ext_inode(x, (uint32_t)n->a, in)) return -1;
    return ext_data(x, in, ext_isize(in), off, buf, len);
}

static void ext_add(archive_t *a, int parent, uint32_t ino, const uint8_t *nm, size_t nl)
{
    extfs *x = a->priv;
    uint8_t in[512];
    if (!ext_inode(x, ino, in)) return;
    uint16_t type = le16(in) & 0xF000;
    if (type != 0x4000 && type != 0x8000 && type != 0xA000) return;      /* devices, pipes, sockets */
    char name[256];
    fs_name(name, sizeof(name), nm, nl, "unnamed");
    uint64_t size = ext_isize(in);
    tree_add(&a->tree, parent, name, type == 0x4000, type == 0x4000 ? 0 : size, ino, 0, 0);
}

static bool ext_expand(archive_t *a, int node)
{
    extfs *x = a->priv;
    uint8_t in[512];
    if (!ext_inode(x, (uint32_t)a->tree.n[node].a, in)) return false;
    uint64_t size = ext_isize(in);
    if (size > 512u * 1024 * 1024) return false;
    uint8_t *buf = malloc(x->bs);
    if (!buf) return false;
    int entries = 0;
    if (le32(in + 0x20) & FL_INLINE) {
        /* entries after the parent inode number, in i_block; the rest (in an xattr) is not read */
        const uint8_t *p = in + 0x28 + 4, *end = in + 0x28 + 60;
        while (p + 8 <= end) {
            uint32_t ino = le32(p), rl = le16(p + 4), nl = p[6];
            if (rl < 8 || p + rl > end) break;
            if (ino && nl && 8 + nl <= rl && !(nl == 1 && p[8] == '.') && !(nl == 2 && p[8] == '.' && p[9] == '.'))
                ext_add(a, node, ino, p + 8, nl);
            p += rl;
        }
        free(buf);
        return true;
    }
    for (uint64_t pos = 0; pos < size && entries < FS_MAX_DIR_ENTRIES; pos += x->bs) {
        if (ext_data(x, in, size, pos, buf, x->bs) != (int32_t)(size - pos < x->bs ? size - pos : x->bs)) break;
        uint32_t blk = (uint32_t)(size - pos < x->bs ? size - pos : x->bs);
        for (uint32_t o = 0; o + 8 <= blk;) {
            uint32_t ino = le32(buf + o), rl = le16(buf + o + 4), nl = buf[o + 6];
            if (rl < 8 || (rl & 3) || o + rl > blk) break;
            if (ino && nl && 8 + nl <= rl && !(nl == 1 && buf[o + 8] == '.') &&
                !(nl == 2 && buf[o + 8] == '.' && buf[o + 9] == '.')) {
                ext_add(a, node, ino, buf + o + 8, nl);
                entries++;
            }
            o += rl;
        }
    }
    free(buf);
    return true;
}

static void ext_destroy(archive_t *a)
{
    extfs *x = a->priv;
    if (!x) return;
    fsc_free(&x->c);
    free(x->gd);
    free(x);
}

archive_t *arc_ext_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t sb[1024];
    if (!ext_probe_sb(file, sb)) return NULL;
    uint32_t incompat = le32(sb + 0x60);
    if (incompat & INC_UNSUPPORTED) { g_archive_err = "Compressed ext volumes are not supported"; return NULL; }
    extfs *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->incompat = incompat;
    x->bs = 1024u << le32(sb + 0x18);
    x->bpg = le32(sb + 0x20);
    x->ipg = le32(sb + 0x28);
    x->first_data = le32(sb + 0x14);
    x->isz = le32(sb + 0x4C) >= 1 ? le16(sb + 0x58) : 128;
    if (x->isz < 128 || x->isz > 512 || x->bs > 4096) { free(x); g_archive_err = "Unsupported ext block or inode size"; return NULL; }
    x->dsz = (incompat & INC_64BIT) ? le16(sb + 0xFE) : 32;
    if (x->dsz < 32 || x->dsz > 128) x->dsz = 32;
    x->blocks = le32(sb + 4);
    if (incompat & INC_64BIT) x->blocks |= (uint64_t)le32(sb + 0x150) << 32;
    x->groups = (x->blocks - x->first_data + x->bpg - 1) / x->bpg;
    if (!x->groups || x->groups > 1u << 22) { free(x); g_archive_err = "Bad ext superblock"; return NULL; }
    if (!fsc_init(&x->c, file)) { free(x); return NULL; }
    uint64_t gdsz = x->groups * x->dsz;
    x->gd = malloc(gdsz);
    uint64_t gdoff = (uint64_t)(x->first_data + 1) * x->bs;
    if (x->bs == 1024 && x->first_data == 0) gdoff = 2048;
    archive_t *a = NULL;
    if (!x->gd || !src_read_full(file, gdoff, x->gd, (uint32_t)gdsz)) {
        g_archive_err = "Can't read the ext group descriptors";
    } else
        a = archive_alloc((incompat & 0x40) ? "ext4" : (le32(sb + 0x5C) & 4) ? "ext3" : "ext2");
    if (!a) {
        fsc_free(&x->c);
        free(x->gd);
        free(x);
        return NULL;
    }
    a->priv = x;
    a->destroy = ext_destroy;
    a->read = ext_read;
    a->expand = ext_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].a = 2;                                          /* the root directory */
    return a;
}

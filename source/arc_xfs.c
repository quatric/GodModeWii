/*
 * XFS (version 4 and the CRC-enabled version 5), read-only: superblock, allocation group addressing,
 * inodes with local / extent-list / B+tree data forks, short-form, block, leaf and node directories.
 * Everything is big endian. Reverse-mapping, realtime volumes, reflink sharing and the 64 bit extent
 * counters of "large extent counts" volumes are not handled (files that need them read as empty).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"

typedef struct { uint64_t off, blk, cnt; bool unwritten; } xext;

typedef struct {
    fsc_t c;
    uint32_t bs, agblocks, agcount, isz, inopblock;
    uint8_t blocklog, inopblog, agblklog, dirblklog;
    uint64_t rootino;
    bool v5, ftype;
    uint32_t corelen;
    /* the extent list of the last file that was read */
    uint64_t cached_ino;
    xext *ext;
    uint32_t next;
} xfs;

static bool xfs_sb(const src_t *f, uint8_t sb[512])
{
    if (f->size < 65536 || !src_read_full(f, 0, sb, 512)) return false;
    if (memcmp(sb, "XFSB", 4)) return false;
    uint32_t bs = be32(sb + 4);
    if (bs < 512 || bs > 65536 || (bs & (bs - 1))) return false;
    if (!be32(sb + 0x54) || !be32(sb + 0x58)) return false;
    uint16_t isz = be16(sb + 0x68);
    if (isz < 256 || isz > 2048) return false;
    return true;
}

bool arc_xfs_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t sb[512];
    return xfs_sb(file, sb);
}

/* inode number -> byte address */
static bool ino_addr(xfs *x, uint64_t ino, uint64_t *addr)
{
    uint32_t off = (uint32_t)(ino & ((1u << x->inopblog) - 1));
    uint64_t agbno = (ino >> x->inopblog) & ((1ull << x->agblklog) - 1);
    uint64_t agno = ino >> (x->inopblog + x->agblklog);
    if (agno >= x->agcount || agbno >= x->agblocks) return false;
    *addr = ((agno * x->agblocks + agbno) * x->bs) + (uint64_t)off * x->isz;
    return true;
}

static uint64_t fsb_addr(xfs *x, uint64_t fsb)
{
    uint64_t agno = fsb >> x->agblklog, agbno = fsb & ((1ull << x->agblklog) - 1);
    return (agno * x->agblocks + agbno) * x->bs;
}

static bool xfs_inode(xfs *x, uint64_t ino, uint8_t *buf)
{
    uint64_t at;
    if (!ino_addr(x, ino, &at) || !fsc_read(&x->c, at, buf, x->isz)) return false;
    return buf[0] == 'I' && buf[1] == 'N';
}

static void unpack_ext(const uint8_t *p, xext *e)
{
    uint64_t hi = be64(p), lo = be64(p + 8);
    e->unwritten = hi >> 63;
    e->off = (hi >> 9) & ((1ull << 54) - 1);
    e->blk = ((hi & 0x1FF) << 43) | (lo >> 21);
    e->cnt = lo & 0x1FFFFF;
}

#define BTREE_MAX_EXT (1u << 20)

/* Collects the extents of an inode's data fork (formats 2 and 3) into x->ext. */
static bool load_extents(xfs *x, uint64_t ino, const uint8_t *in)
{
    if (x->cached_ino == ino && x->ext) return true;
    free(x->ext);
    x->ext = NULL;
    x->next = 0;
    x->cached_ino = 0;
    uint8_t fmt = in[5];
    uint32_t nextents = be32(in + 0x4C);
    const uint8_t *fork = in + x->corelen;
    uint32_t forksize = in[0x52] ? (uint32_t)in[0x52] * 8 : x->isz - x->corelen;
    if (fmt == 2) {
        if (nextents > BTREE_MAX_EXT || (uint64_t)nextents * 16 > forksize) return false;
        x->ext = malloc((nextents ? nextents : 1) * sizeof(xext));
        if (!x->ext) return false;
        for (uint32_t i = 0; i < nextents; i++) unpack_ext(fork + i * 16, &x->ext[i]);
        x->next = nextents;
    } else if (fmt == 3) {
        uint32_t level = be16(fork), nrecs = be16(fork + 2);
        uint32_t maxrecs = (forksize - 4) / 16;
        if (!level || nrecs > maxrecs || !nrecs) return false;
        uint32_t cap = nextents + 16;
        if (cap > BTREE_MAX_EXT) return false;
        x->ext = malloc(cap * sizeof(xext));
        if (!x->ext) return false;
        uint64_t ptr = be64(fork + 4 + maxrecs * 8);                    /* first child */
        uint32_t hdr = x->v5 ? 72 : 24;
        uint8_t *blk = malloc(x->bs);
        if (!blk) return false;
        bool ok = true;
        /* down the left edge to the leaves, then along them */
        for (int guard = 0; ok && guard < 16 && level > 1; guard++, level--) {
            if (!fsc_read(&x->c, fsb_addr(x, ptr), blk, x->bs)) { ok = false; break; }
            uint32_t nr = be16(blk + 6), mr = (x->bs - hdr) / 16;
            if (!nr || nr > mr) { ok = false; break; }
            ptr = be64(blk + hdr + mr * 8);
        }
        uint64_t cur = ptr;
        uint32_t hops = 0;
        while (ok && cur != (uint64_t)-1 && hops++ < BTREE_MAX_EXT) {
            if (!fsc_read(&x->c, fsb_addr(x, cur), blk, x->bs) || be16(blk + 4) != 0) { ok = false; break; }
            uint32_t nr = be16(blk + 6);
            if (hdr + (uint64_t)nr * 16 > x->bs) { ok = false; break; }
            for (uint32_t i = 0; i < nr; i++) {
                if (x->next == cap) {
                    cap *= 2;
                    xext *ne = cap <= BTREE_MAX_EXT ? realloc(x->ext, cap * sizeof(xext)) : NULL;
                    if (!ne) { ok = false; break; }
                    x->ext = ne;
                }
                unpack_ext(blk + hdr + i * 16, &x->ext[x->next++]);
            }
            cur = be64(blk + 16);                                       /* right sibling */
        }
        free(blk);
        if (!ok) { free(x->ext); x->ext = NULL; x->next = 0; return false; }
    } else
        return false;
    x->cached_ino = ino;
    return true;
}

/* bytes of a regular file or of a directory's data fork (offset in bytes) */
static int32_t xfs_data(xfs *x, uint64_t ino, const uint8_t *in, uint64_t size, uint64_t off, uint8_t *buf, uint32_t len)
{
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    uint8_t fmt = in[5];
    if (fmt == 1) {                                                   /* inline (short symlinks, tiny directories) */
        uint32_t avail = in[0x52] ? (uint32_t)in[0x52] * 8 : x->isz - x->corelen;
        if (off + len > avail) return -1;
        memcpy(buf, in + x->corelen + off, len);
        return (int32_t)len;
    }
    if (!load_extents(x, ino, in)) return -1;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, lblk = pos / x->bs;
        uint32_t skew = (uint32_t)(pos % x->bs);
        /* extents are sorted by file offset: binary search */
        int64_t lo = 0, hi = (int64_t)x->next - 1, found = -1;
        while (lo <= hi) {
            int64_t mid = (lo + hi) / 2;
            if (x->ext[mid].off <= lblk) { found = mid; lo = mid + 1; } else hi = mid - 1;
        }
        uint32_t n;
        if (found >= 0 && lblk < x->ext[found].off + x->ext[found].cnt) {
            const xext *e = &x->ext[found];
            uint64_t into = lblk - e->off;
            n = (uint32_t)((e->cnt - into) * x->bs - skew > len - done ? len - done : (e->cnt - into) * x->bs - skew);
            if (e->unwritten) memset(buf + done, 0, n);
            else if (!fsc_read(&x->c, fsb_addr(x, e->blk) + into * x->bs + skew, buf + done, n)) return done ? (int32_t)done : -1;
        } else {                                                      /* a hole: up to the next extent */
            uint64_t next_off = found + 1 < (int64_t)x->next ? x->ext[found + 1].off : UINT64_MAX;
            uint64_t span = next_off == UINT64_MAX ? len - done : (next_off - lblk) * x->bs - skew;
            n = span > len - done ? len - done : (uint32_t)span;
            memset(buf + done, 0, n);
        }
        done += n;
    }
    return (int32_t)done;
}

static uint64_t xfs_isize(const uint8_t *in) { return be64(in + 0x38); }

static int32_t xfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    xfs *x = a->priv;
    uint8_t *in = malloc(x->isz);
    if (!in) return -1;
    int32_t r = -1;
    if (xfs_inode(x, n->a, in)) r = xfs_data(x, n->a, in, xfs_isize(in), off, buf, len);
    free(in);
    return r;
}

static void xfs_add(archive_t *a, int parent, uint64_t ino, const uint8_t *nm, size_t nl)
{
    xfs *x = a->priv;
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) return;
    uint8_t *in = malloc(x->isz);
    if (!in) return;
    if (xfs_inode(x, ino, in)) {
        uint16_t type = be16(in + 2) & 0xF000;
        if (type == 0x4000 || type == 0x8000 || type == 0xA000) {
            char name[256];
            fs_name(name, sizeof(name), nm, nl, "unnamed");
            tree_add(&a->tree, parent, name, type == 0x4000, type == 0x4000 ? 0 : xfs_isize(in), ino, 0, 0);
        }
    }
    free(in);
}

/* entries of one data block / single-block directory (`blk` is the directory block, dirbs bytes) */
static void parse_dir_block(archive_t *a, int node, const uint8_t *blk, uint32_t dirbs)
{
    xfs *x = a->priv;
    uint32_t magic = be32(blk), hdr;
    bool single;
    if (magic == 0x58443242) { hdr = 16; single = true; }              /* XD2B */
    else if (magic == 0x58443244) { hdr = 16; single = false; }        /* XD2D */
    else if (magic == 0x58444233) { hdr = 64; single = true; }         /* XDB3 */
    else if (magic == 0x58444433) { hdr = 64; single = false; }        /* XDD3 */
    else return;
    uint32_t end = dirbs;
    if (single) {
        uint32_t cnt = be32(blk + dirbs - 8);
        if (8 + (uint64_t)cnt * 8 > dirbs - hdr) return;
        end = dirbs - 8 - cnt * 8;
    }
    for (uint32_t o = hdr; o + 8 <= end;) {
        if (be16(blk + o) == 0xFFFF) {                                 /* free space */
            uint32_t l = be16(blk + o + 2);
            if (l < 8 || (l & 7)) break;
            o += l;
            continue;
        }
        uint64_t ino = be64(blk + o);
        uint32_t nl = blk[o + 8];
        uint32_t sz = (8 + 1 + nl + (x->ftype ? 1 : 0) + 2 + 7) & ~7u;
        if (!nl || o + sz > end) break;
        xfs_add(a, node, ino, blk + o + 9, nl);
        o += sz;
    }
}

static bool xfs_expand(archive_t *a, int node)
{
    xfs *x = a->priv;
    uint64_t ino = a->tree.n[node].a;
    uint8_t *in = malloc(x->isz);
    if (!in) return false;
    if (!xfs_inode(x, ino, in)) { free(in); return false; }
    uint8_t fmt = in[5];
    bool ok = true;
    if (fmt == 1) {                                                    /* short form */
        const uint8_t *p = in + x->corelen;
        uint32_t count = p[0], i8 = p[1], isz = i8 ? 8 : 4;
        p += 2 + isz;                                                  /* the parent */
        uint32_t entries = i8 ? i8 : count;
        const uint8_t *end = in + x->isz;
        for (uint32_t i = 0; i < entries && p + 3 <= end; i++) {
            uint32_t nl = p[0];
            const uint8_t *name = p + 3;
            const uint8_t *q = name + nl + (x->ftype ? 1 : 0);
            if (q + isz > end) break;
            uint64_t child = isz == 8 ? be64(q) : be32(q);
            xfs_add(a, node, child, name, nl);
            p = q + isz;
        }
        free(in);
        return true;
    }
    if (fmt != 2 && fmt != 3) { free(in); return false; }
    if (!load_extents(x, ino, in)) { free(in); return false; }
    uint32_t dirbs = x->bs << x->dirblklog;
    uint64_t leafoff = (1ull << 35) / x->bs;                           /* directory data lives below 32 GiB */
    uint8_t *blk = malloc(dirbs);
    xext *ex = x->ext;                                                 /* xfs_add may load other extent lists */
    uint32_t nex = x->next;
    xext *mine = malloc((nex ? nex : 1) * sizeof(xext));
    if (!blk || !mine) { free(blk); free(mine); free(in); return false; }
    memcpy(mine, ex, nex * sizeof(xext));
    int entries = 0;
    for (uint32_t i = 0; i < nex && ok && entries < FS_MAX_DIR_ENTRIES; i++) {
        if (mine[i].off >= leafoff) break;
        uint32_t per = 1u << x->dirblklog;
        for (uint64_t b = 0; b + per <= mine[i].cnt; b += per) {
            if (mine[i].unwritten) continue;
            if (!fsc_read(&x->c, fsb_addr(x, mine[i].blk + b), blk, dirbs)) { ok = false; break; }
            parse_dir_block(a, node, blk, dirbs);
            entries++;
        }
    }
    free(mine);
    free(blk);
    free(in);
    return ok;
}

static void xfs_destroy(archive_t *a)
{
    xfs *x = a->priv;
    if (!x) return;
    fsc_free(&x->c);
    free(x->ext);
    free(x);
}

archive_t *arc_xfs_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t sb[512];
    if (!xfs_sb(file, sb)) return NULL;
    xfs *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->bs = be32(sb + 4);
    x->agblocks = be32(sb + 0x54);
    x->agcount = be32(sb + 0x58);
    x->isz = be16(sb + 0x68);
    x->inopblock = be16(sb + 0x6A);
    x->blocklog = sb[0x78];
    x->inopblog = sb[0x7B];
    x->agblklog = sb[0x7C];
    x->dirblklog = sb[0xC0];
    x->rootino = be64(sb + 0x38);
    x->v5 = (be16(sb + 0x64) & 0xF) == 5;
    x->ftype = x->v5 ? (be32(sb + 0xD8) & 1) : (be32(sb + 0xC8) & 0x200);
    x->corelen = x->v5 ? 176 : 100;
    archive_t *a = NULL;
    uint32_t incompat = x->v5 ? be32(sb + 0xD8) : 0;
    if (incompat & 0x10) g_archive_err = "XFS volumes with large extent counts are not supported";
    else if (x->bs != (1u << x->blocklog) || x->dirblklog > 8 || x->inopblock != (1u << x->inopblog)) g_archive_err = "Bad XFS superblock";
    else if (!fsc_init(&x->c, file)) g_archive_err = "Out of memory";
    else a = archive_alloc("XFS");
    if (!a) { free(x); return NULL; }
    a->priv = x;
    a->destroy = xfs_destroy;
    a->read = xfs_read;
    a->expand = xfs_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].a = x->rootino;
    return a;
}

/*
 * ZFS (OpenZFS / Solaris, little endian pools), read-only, from one disk or partition of a single-disk or
 * mirrored pool: the vdev label's newest uberblock, the meta object set, the dataset tree, and each
 * file system dataset's objects (ZAP directories, dnodes with indirect blocks, system attributes). Blocks
 * can be stored raw or compressed with lzjb, zle, lz4, gzip or zstd; checksums are not verified. Not read:
 * RAID-Z or striped pools, encrypted datasets, gang blocks, snapshots, zvols, extended attributes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fsutil.h"
#include "zstd.h"

#define UB_MAGIC 0x00bab10cull
#define BP_SIZE 128
#define DN_SIZE 512
#define LABEL_SIZE (256 * 1024)
#define VDEV_START 0x400000ull
#define MAX_DATASETS 64
#define BC_N 8
#define BC_MAX (128 * 1024)

typedef struct { uint8_t *raw; uint32_t len; } dnode_t;       /* a dnode with its extra slots */

typedef struct {
    uint8_t meta[DN_SIZE * 8];  /* the objset's meta dnode (never more than a few slots) */
    bool ok;
} objset_t;

typedef struct {
    char path[256];
    objset_t os;
    uint64_t root_obj;
    uint64_t sa_attrs_obj;
    int sa_mode_id, sa_size_id, sa_symlink_id;
    int sa_len[64];             /* registered length by attribute id, 0 = variable */
    uint16_t layouts[64][32];   /* attribute ids of each layout */
    int layout_n[64];
    bool sa_loaded, bad;
} dataset_t;

typedef struct {
    uint64_t off;
    uint32_t len;
    uint32_t stamp;
    uint8_t *data;
} bcache_t;

typedef struct {
    fsc_t c;
    objset_t mos;
    dataset_t *ds;
    int nds;
    char pool[64];
    bcache_t bc[BC_N];
    uint32_t tick;
} zfs;

static uint64_t le64p(const uint8_t *p) { return le64(p); }

/* ---- label / uberblock ------------------------------------------------------------------- */

static bool find_uberblock(const src_t *f, uint8_t ub[1024])
{
    static uint8_t arr[128 * 1024];
    uint8_t nv[12];
    if (f->size < 4 * LABEL_SIZE + VDEV_START) return false;
    /* cheap first: the label's nvlist starts with { encoding 0 (XDR), little endian 1 }, version 0, flags 1 */
    if (!src_read_full(f, 16384, nv, sizeof(nv)) || nv[0] != 0 || nv[1] != 1 || be32(nv + 4) != 0 || be32(nv + 8) != 1) return false;
    if (!src_read_full(f, 128 * 1024, arr, sizeof(arr))) return false;
    uint64_t best = 0;
    bool found = false;
    for (uint32_t o = 0; o + 1024 <= sizeof(arr); o += 1024) {
        if (le64p(arr + o) != UB_MAGIC) continue;
        uint64_t txg = le64p(arr + o + 16);
        if (!found || txg >= best) { best = txg; memcpy(ub, arr + o, 1024); found = true; }
    }
    return found;
}

bool arc_zfs_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t ub[1024];
    return find_uberblock(file, ub);
}

/* the pool's name from the label's nvlist: XDR pairs, each starting with its own encoded size */
static void pool_name(const src_t *f, char *out, size_t outsz)
{
    static uint8_t nv[16384];
    snprintf(out, outsz, "zpool");
    if (!src_read_full(f, 16384, nv, sizeof(nv))) return;
    uint32_t pos = 4 + 8;                                        /* header, version, flags */
    while (pos + 16 < sizeof(nv)) {
        uint32_t esz = be32(nv + pos);
        if (esz < 16 || pos + esz > sizeof(nv)) break;
        uint32_t nlen = be32(nv + pos + 8), p = pos + 12 + ((nlen + 3) & ~3u);
        if (nlen == 4 && !memcmp(nv + pos + 12, "name", 4) && p + 12 <= sizeof(nv) && be32(nv + p) == 9) {
            uint32_t sl = be32(nv + p + 8);
            if (sl && p + 12 + sl <= sizeof(nv)) {
                if (sl >= outsz) sl = (uint32_t)outsz - 1;
                memcpy(out, nv + p + 12, sl);
                out[sl] = 0;
                tree_clean_name(out);
            }
            return;
        }
        pos += esz;
    }
}

/* ---- decompression ------------------------------------------------------------------- */

static bool lzjb_dec(const uint8_t *s, uint32_t sl, uint8_t *d, uint32_t dl)
{
    uint32_t sp = 0, dp = 0, copymap = 0, copymask = 1 << 7;
    while (dp < dl) {
        if ((copymask <<= 1) == (1 << 8)) {
            if (sp >= sl) return false;
            copymask = 1;
            copymap = s[sp++];
        }
        if (copymap & copymask) {
            if (sp + 2 > sl) return false;
            uint32_t mlen = (s[sp] >> 2) + 3, off = ((s[sp] << 8) | s[sp + 1]) & 0x3FF;
            sp += 2;
            if (off > dp || !off) return false;
            while (mlen-- && dp < dl) { d[dp] = d[dp - off]; dp++; }
        } else {
            if (sp >= sl) return false;
            d[dp++] = s[sp++];
        }
    }
    return true;
}

static bool zle_dec(const uint8_t *s, uint32_t sl, uint8_t *d, uint32_t dl)
{
    uint32_t sp = 0, dp = 0, n = 64;
    while (sp < sl && dp < dl) {
        uint32_t len = 1 + s[sp++];
        if (len <= n) {
            if (sp + len > sl || len > dl - dp) { if (sp + len > sl) return false; len = dl - dp; }
            memcpy(d + dp, s + sp, len);
            sp += len;
        } else {
            len -= n;
            if (len > dl - dp) len = dl - dp;
            memset(d + dp, 0, len);
        }
        dp += len;
    }
    memset(d + dp, 0, dl - dp);
    return true;
}

static bool lz4_block(const uint8_t *s, uint32_t sl, uint8_t *d, uint32_t dl)
{
    uint32_t sp = 0, dp = 0;
    while (sp < sl) {
        uint8_t tok = s[sp++];
        uint32_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (sp >= sl) return false; b = s[sp++]; lit += b; } while (b == 255); }
        if (lit > sl - sp || lit > dl - dp) return false;
        memcpy(d + dp, s + sp, lit);
        sp += lit; dp += lit;
        if (sp >= sl) break;
        if (sp + 2 > sl) return false;
        uint32_t off = s[sp] | s[sp + 1] << 8;
        sp += 2;
        uint32_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (sp >= sl) return false; b = s[sp++]; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > dp || ml > dl - dp) return false;
        for (uint32_t i = 0; i < ml; i++, dp++) d[dp] = d[dp - off];
    }
    return dp == dl;
}

/* one block's payload -> lsize bytes */
static bool decompress(int comp, const uint8_t *s, uint32_t sl, uint8_t *d, uint32_t dl)
{
    switch (comp) {
    case 2:                                                      /* off */
        memcpy(d, s, sl < dl ? sl : dl);
        if (sl < dl) memset(d + sl, 0, dl - sl);
        return true;
    case 1: case 3: return lzjb_dec(s, sl, d, dl);               /* on / lzjb */
    case 4: memset(d, 0, dl); return true;                       /* empty */
    case 14: return zle_dec(s, sl, d, dl);
    case 15: {                                                   /* lz4: u32 BE length first */
        if (sl < 4) return false;
        uint32_t cl = be32(s);
        if (cl > sl - 4) return false;
        return lz4_block(s + 4, cl, d, dl);
    }
    case 16: {                                                   /* zstd: u32 BE length, u32 version, frame */
        if (sl < 8) return false;
        uint32_t cl = be32(s);
        if (cl > sl - 8) return false;
        size_t r = ZSTD_decompress(d, dl, s + 8, cl);
        return !ZSTD_isError(r);
    }
    default:
        if (comp >= 5 && comp <= 13) {                           /* gzip 1-9 */
            uLongf out = dl;
            if (uncompress(d, &out, s, sl) != Z_OK) return false;
            if (out < dl) memset(d + out, 0, dl - out);
            return true;
        }
        return false;
    }
}

/* ---- block pointers ------------------------------------------------------------------- */

static bool bp_hole(const uint8_t *bp)
{
    if ((le64p(bp + 48) >> 39) & 1) return false;                /* embedded data */
    return !le64p(bp) && !le64p(bp + 8);                         /* first DVA empty */
}

/* The block `bp` points at, into out (cap bytes at least lsize); *lsize is its logical size. */
static bool bp_read(zfs *z, const uint8_t *bp, uint8_t *out, uint32_t cap, uint32_t *lsize, uint32_t hole_size)
{
    uint64_t prop = le64p(bp + 48);
    if ((prop >> 39) & 1) {                                      /* embedded */
        uint32_t ls = (uint32_t)(prop & 0x1FFFFFF) + 1, ps = (uint32_t)((prop >> 25) & 0x7F) + 1;
        int comp = (int)((prop >> 32) & 0x7F);
        uint8_t pay[112];
        memcpy(pay, bp, 48);
        memcpy(pay + 48, bp + 56, 24);
        memcpy(pay + 72, bp + 88, 40);
        if (ls > cap || ps > 112) return false;
        *lsize = ls;
        return decompress(comp, pay, ps, out, ls);
    }
    if (bp_hole(bp)) {
        *lsize = hole_size;
        if (hole_size > cap) return false;
        memset(out, 0, hole_size);
        return true;
    }
    uint32_t ls = (uint32_t)((prop & 0xFFFF) + 1) << 9, ps = (uint32_t)(((prop >> 16) & 0xFFFF) + 1) << 9;
    int comp = (int)((prop >> 32) & 0x7F);
    if (ls > cap) return false;
    *lsize = ls;
    uint8_t *tmp = comp == 2 ? out : malloc(ps);
    if (!tmp) return false;
    bool ok = false;
    for (int d = 0; d < 3 && !ok; d++) {
        uint64_t w0 = le64p(bp + d * 16), w1 = le64p(bp + d * 16 + 8);
        if (!w0 && !w1) continue;
        if ((w0 >> 32) != 0 || (w1 >> 63)) continue;             /* another top-level vdev, or a gang block */
        uint64_t at = ((w1 & 0x7FFFFFFFFFFFFFFFull) << 9) + VDEV_START;
        if (comp == 2 && ps > ls) ps = ls;
        ok = fsc_read(&z->c, at, tmp, ps);
    }
    if (ok && comp != 2) ok = decompress(comp, tmp, ps, out, ls);
    if (tmp != out) free(tmp);
    return ok;
}

/* cached read of a whole block by DVA (for indirect blocks and data) */
static const uint8_t *bp_cached(zfs *z, const uint8_t *bp, uint32_t hole_size, uint32_t *len)
{
    uint64_t prop = le64p(bp + 48);
    bool cacheable = !((prop >> 39) & 1) && !bp_hole(bp);
    uint64_t key = le64p(bp + 8);
    if (cacheable)
        for (int i = 0; i < BC_N; i++)
            if (z->bc[i].data && z->bc[i].off == key) { z->bc[i].stamp = ++z->tick; *len = z->bc[i].len; return z->bc[i].data; }
    uint32_t cap = cacheable ? (uint32_t)((prop & 0xFFFF) + 1) << 9 : ((prop >> 39) & 1) ? (uint32_t)(prop & 0x1FFFFFF) + 1 : hole_size;
    if (!cap) cap = hole_size;
    int slot = 0;
    for (int i = 0; i < BC_N; i++)
        if (z->bc[i].stamp < z->bc[slot].stamp) slot = i;
    bcache_t *c = &z->bc[slot];
    free(c->data);
    c->data = malloc(cap ? cap : 1);
    c->len = 0;
    c->off = (uint64_t)-1;
    if (!c->data) return NULL;
    uint32_t ls;
    if (!bp_read(z, bp, c->data, cap, &ls, hole_size)) { free(c->data); c->data = NULL; return NULL; }
    c->len = ls;
    c->off = cacheable ? key : (uint64_t)-1;
    c->stamp = ++z->tick;
    *len = ls;
    return c->data;
}

/* ---- dnodes ------------------------------------------------------------------- */

static const uint8_t *dn_bp(const uint8_t *dn, int i) { return dn + 64 + i * BP_SIZE; }
static uint32_t dn_datablk(const uint8_t *dn) { return (uint32_t)le16(dn + 8) << 9; }

/* data block `blkid` of an object; the returned pointer is valid until the next call */
static const uint8_t *dn_block(zfs *z, const uint8_t *dn, uint64_t blkid, uint32_t *len)
{
    uint32_t levels = dn[2], nbp = dn[3], ibs = dn[1], dbs = dn_datablk(dn);
    if (!levels || !nbp || ibs < 12 || ibs > 24 || !dbs) return NULL;
    uint32_t epbs = ibs - 7;
    uint8_t bp[BP_SIZE];
    uint64_t top = blkid >> ((levels - 1) * epbs);
    if (top >= nbp) return NULL;
    memcpy(bp, dn_bp(dn, (int)top), BP_SIZE);
    for (uint32_t l = levels - 1; l > 0; l--) {
        uint32_t ilen;
        const uint8_t *ind = bp_cached(z, bp, 1u << ibs, &ilen);
        if (!ind) return NULL;
        uint32_t idx = (uint32_t)((blkid >> ((l - 1) * epbs)) & ((1u << epbs) - 1));
        if ((idx + 1) * BP_SIZE > ilen) return NULL;
        memcpy(bp, ind + idx * BP_SIZE, BP_SIZE);
    }
    return bp_cached(z, bp, dbs, len);
}

/* bytes of an object (does not stop at the logical size) */
static bool dn_read(zfs *z, const uint8_t *dn, uint64_t off, uint8_t *buf, uint32_t len)
{
    uint32_t dbs = dn_datablk(dn);
    if (!dbs) return false;
    while (len) {
        uint64_t blk = off / dbs;
        uint32_t skew = (uint32_t)(off % dbs), blen;
        const uint8_t *b = dn_block(z, dn, blk, &blen);
        if (!b || skew >= blen) return false;
        uint32_t n = blen - skew > len ? len : blen - skew;
        memcpy(buf, b + skew, n);
        buf += n; off += n; len -= n;
    }
    return true;
}

/* dnode number `obj` of an object set, with its extra slots (raw: DN_SIZE * (1 + extra) bytes) */
static bool get_dnode(zfs *z, const objset_t *os, uint64_t obj, uint8_t *raw, uint32_t cap)
{
    if (!dn_read(z, os->meta, obj * DN_SIZE, raw, DN_SIZE)) return false;
    uint32_t extra = raw[12];
    if (extra && cap >= DN_SIZE * (1 + extra)) {
        if (!dn_read(z, os->meta, obj * DN_SIZE + DN_SIZE, raw + DN_SIZE, DN_SIZE * extra)) return false;
    }
    return raw[0] != 0;
}

static bool load_objset(zfs *z, const uint8_t *bp, objset_t *os)
{
    uint32_t len;
    uint8_t *buf = malloc(1 << 17);
    if (!buf) return false;
    memset(os, 0, sizeof(*os));
    bool ok = bp_read(z, bp, buf, 1 << 17, &len, 0) && len >= DN_SIZE;
    if (ok) { memcpy(os->meta, buf, DN_SIZE); os->ok = true; }
    free(buf);
    return ok;
}

/* ---- ZAP ------------------------------------------------------------------- */

typedef bool (*zap_fn)(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be);

static bool zap_each(zfs *z, const uint8_t *dn, zap_fn fn, void *ctx)
{
    uint32_t dbs = dn_datablk(dn), len;
    const uint8_t *b = dn_block(z, dn, 0, &len);
    if (!b || len < 64) return false;
    uint64_t type = le64p(b);
    if (type == 0x8000000000000003ull) {                        /* micro ZAP */
        uint8_t *copy = malloc(len);
        if (!copy) return false;
        memcpy(copy, b, len);
        for (uint32_t o = 64; o + 64 <= len; o += 64) {
            const uint8_t *e = copy + o;
            if (!e[14]) continue;
            char name[51];
            memcpy(name, e + 14, 50);
            name[50] = 0;
            if (!fn(ctx, name, e, 8, 1, false)) break;
        }
        free(copy);
        return true;
    }
    if (type != 0x8000000000000001ull) return false;            /* fat ZAP: the leaves are the other blocks */
    uint64_t nblocks = le64p(dn + 16) + 1;                       /* dn_maxblkid */
    uint8_t *leaf = malloc(dbs);
    if (!leaf) return false;
    uint32_t shift = 0;
    while ((1u << shift) < dbs) shift++;
    uint32_t hash_bytes = 2u << (shift - 5), chunks_at = 48 + hash_bytes;
    uint32_t nchunks = (dbs - chunks_at) / 24;
    for (uint64_t bi = 1; bi < nblocks; bi++) {
        uint32_t ll;
        const uint8_t *lb = dn_block(z, dn, bi, &ll);
        if (!lb || ll < dbs) continue;
        if (le64p(lb) != 0x8000000000000000ull || le32(lb + 24) != 0x2AB1EAF) continue;
        memcpy(leaf, lb, dbs);
        for (uint32_t c = 0; c < nchunks; c++) {
            const uint8_t *e = leaf + chunks_at + c * 24;
            if (e[0] != 252) continue;
            uint32_t intlen = e[1], nname = le16(e + 6), nval = le16(e + 10);
            uint32_t nchunk = le16(e + 4), vchunk = le16(e + 8);
            if (nname == 0 || nname > 256 || nval > 1024 || !intlen || intlen > 8) continue;
            char name[257];
            uint32_t got = 0;
            for (uint32_t ch = nchunk, guard = 0; ch < nchunks && got < nname && guard++ < 64; ) {
                const uint8_t *a = leaf + chunks_at + ch * 24;
                uint32_t n = nname - got > 21 ? 21 : nname - got;
                memcpy(name + got, a + 1, n);
                got += n;
                ch = le16(a + 22);
                if (ch == 0xFFFF) break;
            }
            name[got < 256 ? got : 256] = 0;
            uint8_t val[1024 * 8 > 4096 ? 4096 : 1024 * 8];
            uint32_t vbytes = nval * intlen, vgot = 0;
            if (vbytes > sizeof(val)) continue;
            for (uint32_t ch = vchunk, guard = 0; ch < nchunks && vgot < vbytes && guard++ < 512; ) {
                const uint8_t *a = leaf + chunks_at + ch * 24;
                uint32_t n = vbytes - vgot > 21 ? 21 : vbytes - vgot;
                memcpy(val + vgot, a + 1, n);
                vgot += n;
                ch = le16(a + 22);
                if (ch == 0xFFFF) break;
            }
            if (vgot < vbytes) continue;
            if (!fn(ctx, name, val, intlen, nval, true)) { free(leaf); return true; }
        }
    }
    free(leaf);
    return true;
}

static uint64_t zap_int(const uint8_t *val, uint32_t intlen, bool be)
{
    uint64_t v = 0;
    if (be) for (uint32_t i = 0; i < intlen; i++) v = v << 8 | val[i];
    else v = le64p(val);
    return v;
}

typedef struct { const char *want; uint64_t value; bool found; } zfind_t;

static bool zfind_cb(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be)
{
    zfind_t *f = ctx;
    (void)numints;
    if (strcmp(name, f->want)) return true;
    f->value = zap_int(val, intlen, be);
    f->found = true;
    return false;
}

static bool zap_lookup(zfs *z, const objset_t *os, uint64_t obj, const char *name, uint64_t *out)
{
    uint8_t dn[DN_SIZE * 4];
    if (!get_dnode(z, os, obj, dn, sizeof(dn))) return false;
    zfind_t f = { name, 0, false };
    if (!zap_each(z, dn, zfind_cb, &f) || !f.found) return false;
    *out = f.value;
    return true;
}

/* ---- datasets ------------------------------------------------------------------- */

static const uint8_t *bonus(const uint8_t *dn, uint32_t *len)
{
    *len = le16(dn + 10);
    return dn + 64 + dn[3] * BP_SIZE;
}

typedef struct { zfs *z; const char *prefix; } childctx;
typedef struct { char name[128]; uint64_t dir_obj; } childent;
typedef struct { childent *e; int n, cap; } childlist;

static bool child_cb(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be)
{
    childlist *l = ctx;
    (void)numints;
    if (name[0] == '$' || name[0] == '%') return true;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        childent *ne = realloc(l->e, nc * sizeof(childent));
        if (!ne) return false;
        l->e = ne;
        l->cap = nc;
    }
    snprintf(l->e[l->n].name, sizeof(l->e[l->n].name), "%s", name);
    l->e[l->n].dir_obj = zap_int(val, intlen, be);
    l->n++;
    return true;
}

static void add_dataset(zfs *z, const char *path, uint64_t dir_obj, int depth)
{
    if (z->nds >= MAX_DATASETS || depth > 8) return;
    uint8_t dn[DN_SIZE * 4];
    if (!get_dnode(z, &z->mos, dir_obj, dn, sizeof(dn))) return;
    uint32_t bl;
    const uint8_t *b = bonus(dn, &bl);
    if (bl < 40) return;
    uint64_t head = le64p(b + 8), childzap = le64p(b + 32);
    if (head) {
        uint8_t ddn[DN_SIZE * 4];
        if (get_dnode(z, &z->mos, head, ddn, sizeof(ddn))) {
            uint32_t dbl;
            const uint8_t *db = bonus(ddn, &dbl);
            if (dbl >= 128 + BP_SIZE) {
                dataset_t *d = &z->ds[z->nds];
                memset(d, 0, sizeof(*d));
                snprintf(d->path, sizeof(d->path), "%s", path);
                if (load_objset(z, db + 128, &d->os)) z->nds++;
            }
        }
    }
    if (childzap) {
        uint8_t cdn[DN_SIZE * 4];
        childlist l = { 0 };
        if (get_dnode(z, &z->mos, childzap, cdn, sizeof(cdn))) zap_each(z, cdn, child_cb, &l);
        for (int i = 0; i < l.n; i++) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s%s%s", path, path[0] ? "-" : "", l.e[i].name);
            add_dataset(z, sub, l.e[i].dir_obj, depth + 1);
        }
        free(l.e);
    }
}

typedef struct { dataset_t *d; } sactx;

static bool reg_cb(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be)
{
    dataset_t *d = ctx;
    (void)numints;
    uint64_t v = zap_int(val, intlen, be);
    int id = (int)(v & 0xFFFF), len = (int)((v >> 24) & 0xFFFF);
    if (id >= 64) return true;
    d->sa_len[id] = len;
    if (!strcmp(name, "ZPL_MODE")) d->sa_mode_id = id + 1;
    else if (!strcmp(name, "ZPL_SIZE")) d->sa_size_id = id + 1;
    else if (!strcmp(name, "ZPL_SYMLINK")) d->sa_symlink_id = id + 1;
    return true;
}

static bool layout_cb(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be)
{
    dataset_t *d = ctx;
    int n = atoi(name);
    if (n < 0 || n >= 64 || intlen != 2) return true;
    if (numints > 32) numints = 32;
    for (uint32_t i = 0; i < numints; i++) d->layouts[n][i] = be ? (uint16_t)(val[i * 2] << 8 | val[i * 2 + 1]) : le16(val + i * 2);
    d->layout_n[n] = (int)numints;
    return true;
}

static void load_master(zfs *z, dataset_t *d)
{
    if (d->sa_loaded) return;
    d->sa_loaded = true;
    uint64_t v;
    if (!zap_lookup(z, &d->os, 1, "ROOT", &d->root_obj)) { d->bad = true; return; }
    if (zap_lookup(z, &d->os, 1, "SA_ATTRS", &d->sa_attrs_obj) && d->sa_attrs_obj) {
        uint64_t reg, lay;
        if (zap_lookup(z, &d->os, d->sa_attrs_obj, "REGISTRY", &reg)) {
            uint8_t dn[DN_SIZE * 4];
            if (get_dnode(z, &d->os, reg, dn, sizeof(dn))) zap_each(z, dn, reg_cb, d);
        }
        if (zap_lookup(z, &d->os, d->sa_attrs_obj, "LAYOUTS", &lay)) {
            uint8_t dn[DN_SIZE * 4];
            if (get_dnode(z, &d->os, lay, dn, sizeof(dn))) zap_each(z, dn, layout_cb, d);
        }
    }
    (void)v;
}

/* mode and size of an object from its bonus buffer (a legacy znode or system attributes) */
static bool object_attrs(zfs *z, dataset_t *d, const uint8_t *dn, uint16_t *mode, uint64_t *size)
{
    (void)z;
    uint32_t bl;
    const uint8_t *b = bonus(dn, &bl);
    uint8_t bt = dn[4];
    if (bt == 17 && bl >= 88) { *mode = (uint16_t)le64p(b + 72); *size = le64p(b + 80); return true; }
    if (bt != 44 || bl < 8 || le32(b) != 0x2F505A) return false;
    uint16_t info = le16(b + 4);
    int layout = info & 0x3FF;
    uint32_t hdr = ((info >> 10) & 7) << 3;
    if (layout >= 64 || !d->layout_n[layout]) return false;
    uint32_t pos = hdr, lens_used = 0;
    *mode = 0;
    *size = 0;
    bool got_mode = false, got_size = false;
    for (int i = 0; i < d->layout_n[layout]; i++) {
        int id = d->layouts[layout][i];
        if (id >= 64) return false;
        uint32_t len = (uint32_t)d->sa_len[id];
        if (!len) { len = le16(b + 6 + lens_used * 2); lens_used++; }
        if (pos + len > bl) break;
        if (id + 1 == d->sa_mode_id && len >= 8) { *mode = (uint16_t)le64p(b + pos); got_mode = true; }
        if (id + 1 == d->sa_size_id && len >= 8) { *size = le64p(b + pos); got_size = true; }
        pos += len;
    }
    return got_mode && got_size;
}

/* ---- the archive ------------------------------------------------------------------- */

typedef struct { archive_t *a; zfs *z; dataset_t *d; int parent, ds; } dirctx;

static bool dir_cb(void *ctx, const char *name, const uint8_t *val, uint32_t intlen, uint32_t numints, bool be)
{
    dirctx *c = ctx;
    (void)numints;
    uint64_t v = zap_int(val, intlen, be);
    uint32_t type = (uint32_t)(v >> 60);
    uint64_t obj = v & 0xFFFFFFFFFFFFull;
    if (type != 4 && type != 8 && type != 10) return true;
    uint8_t dn[DN_SIZE * 4];
    uint16_t mode = 0;
    uint64_t size = 0;
    if (get_dnode(c->z, &c->d->os, obj, dn, sizeof(dn))) object_attrs(c->z, c->d, dn, &mode, &size);
    char nm[256];
    fs_name(nm, sizeof(nm), (const uint8_t *)name, strlen(name), "unnamed");
    tree_add(&c->a->tree, c->parent, nm, type == 4, type == 4 ? 0 : size, obj, 0, (uint32_t)c->ds);
    return true;
}

static bool zfs_expand(archive_t *a, int node)
{
    zfs *z = a->priv;
    if (node == 0) return true;
    tnode *n = &a->tree.n[node];
    int ds = (int)n->c;
    if (ds < 0 || ds >= z->nds) return false;
    dataset_t *d = &z->ds[ds];
    uint8_t dn[DN_SIZE * 4];
    if (!get_dnode(z, &d->os, n->a, dn, sizeof(dn))) return false;
    dirctx c = { a, z, d, node, ds };
    return zap_each(z, dn, dir_cb, &c);
}

static int32_t zfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    zfs *z = a->priv;
    int ds = (int)n->c;
    if (ds < 0 || ds >= z->nds) return -1;
    dataset_t *d = &z->ds[ds];
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    uint8_t dn[DN_SIZE * 4];
    if (!get_dnode(z, &d->os, n->a, dn, sizeof(dn))) return -1;
    return dn_read(z, dn, off, buf, len) ? (int32_t)len : -1;
}

static void zfs_destroy(archive_t *a)
{
    zfs *z = a->priv;
    if (!z) return;
    fsc_free(&z->c);
    for (int i = 0; i < BC_N; i++) free(z->bc[i].data);
    free(z->ds);
    free(z);
}

archive_t *arc_zfs_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t ub[1024];
    if (!find_uberblock(file, ub)) return NULL;
    zfs *z = calloc(1, sizeof(*z));
    if (!z) return NULL;
    z->ds = calloc(MAX_DATASETS, sizeof(dataset_t));
    archive_t *a = NULL;
    const char *err = "Can't read the ZFS pool";
    if (z->ds && fsc_init(&z->c, file)) {
        pool_name(file, z->pool, sizeof(z->pool));
        if (load_objset(z, ub + 40, &z->mos)) {
            uint64_t root_dir;
            if (zap_lookup(z, &z->mos, 1, "root_dataset", &root_dir)) {
                add_dataset(z, "", root_dir, 0);
                a = z->nds ? archive_alloc("ZFS") : NULL;
                if (!z->nds) err = "No ZFS file system was found in the pool";
            } else
                err = "The ZFS pool has no root dataset (RAID-Z, striped or encrypted pools are not supported)";
        }
    }
    if (a) {
        for (int i = 0; i < z->nds; i++) {
            dataset_t *d = &z->ds[i];
            load_master(z, d);
            if (d->bad) continue;
            char label[256];
            snprintf(label, sizeof(label), "%s", d->path[0] ? d->path : z->pool);
            tree_clean_name(label);
            int idx = tree_add(&a->tree, 0, label, true, 0, d->root_obj, 0, (uint32_t)i);
            (void)idx;
        }
        if (a->tree.count < 2) { archive_close(a); a = NULL; err = "No readable ZFS file system"; }
    }
    if (!a) {
        g_archive_err = err;
        fsc_free(&z->c);
        for (int i = 0; i < BC_N; i++) free(z->bc[i].data);
        free(z->ds);
        free(z);
        return NULL;
    }
    a->priv = z;
    a->destroy = zfs_destroy;
    a->read = zfs_read;
    a->expand = zfs_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].loaded = true;
    return a;
}

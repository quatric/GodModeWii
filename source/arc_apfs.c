/*
 * APFS, read-only, from the container (a whole disk image or the partition): the newest checkpointed
 * container superblock, the object maps, and each volume's file system B-tree (folders, files and their
 * extents). Every volume appears as a folder at the top. Encrypted volumes show as a folder that says so;
 * snapshots, clones' shared extents bookkeeping, sparse / compressed (decmpfs) files and resource forks
 * are not interpreted (compressed files read as empty).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"

#define NX_MAGIC 0x4253584Eu       /* "NXSB" */
#define APSB_MAGIC 0x42535041u     /* "APSB" */

#define T_INODE 3
#define T_DREC  9
#define T_EXTENT 8

#define BT_ROOT 1
#define BT_LEAF 2
#define BT_FIXED 4

typedef struct {
    uint64_t omap_tree;         /* physical */
    uint64_t root_oid;          /* virtual */
    uint64_t xid;
    uint32_t incompat;
    bool encrypted;
    char name[256];
} apvol;

typedef struct {
    fsc_t c;
    uint32_t bs;
    uint64_t xid;               /* of the container superblock */
    apvol *vols;
    int nvols;
    uint8_t *blk;               /* scratch for lookups that must not recurse into the same buffer */
} apfs;

static bool ap_block(apfs *f, uint64_t blk, uint8_t *buf)
{
    return fsc_read(&f->c, blk * f->bs, buf, f->bs);
}

static bool nx_ok(const uint8_t *b) { return le32(b + 32) == NX_MAGIC && le32(b + 36) >= 4096 && le32(b + 36) <= 65536; }

bool arc_apfs_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t b[64];
    if (file->size < 65536 || !src_read_full(file, 0, b, sizeof(b))) return false;
    return nx_ok(b);
}

/* ---- B-tree access ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *node;
    uint32_t flags, nkeys, bs;
    uint32_t keys_at, vals_end;
} btn;

static bool btn_open(btn *n, const uint8_t *node, uint32_t bs)
{
    n->node = node;
    n->bs = bs;
    n->flags = le16(node + 32);
    n->nkeys = le32(node + 36);
    uint32_t toc_off = le16(node + 40), toc_len = le16(node + 42);
    n->keys_at = 56 + toc_off + toc_len;
    n->vals_end = bs - ((n->flags & BT_ROOT) ? 40 : 0);
    return n->nkeys < bs / 4 && 56 + toc_off + (uint64_t)n->nkeys * ((n->flags & BT_FIXED) ? 4 : 8) <= bs;
}

/* key / value of entry i; lengths are 0 when out of range */
static bool btn_entry(const btn *n, uint32_t i, const uint8_t **k, uint32_t *kl, const uint8_t **v, uint32_t *vl, uint32_t fixed_k, uint32_t fixed_v)
{
    const uint8_t *toc = n->node + 56 + le16(n->node + 40);
    uint32_t ko, kn, vo, vn;
    if (n->flags & BT_FIXED) {
        ko = le16(toc + i * 4); vo = le16(toc + i * 4 + 2);
        kn = fixed_k; vn = fixed_v;
    } else {
        ko = le16(toc + i * 8); kn = le16(toc + i * 8 + 2);
        vo = le16(toc + i * 8 + 4); vn = le16(toc + i * 8 + 6);
    }
    if (n->keys_at + ko + kn > n->bs || vo == 0xFFFF || vo > n->vals_end || n->vals_end - vo + vn > n->bs) return false;
    *k = n->node + n->keys_at + ko;
    *kl = kn;
    *v = n->node + n->vals_end - vo;
    *vl = vn;
    return true;
}

/* ---- object maps ------------------------------------------------------------------- */

/* the physical block of virtual object `oid`, newest version not after `xid` */
static bool omap_lookup(apfs *f, uint64_t tree, uint64_t oid, uint64_t xid, uint64_t *paddr)
{
    uint8_t *node = malloc(f->bs);
    if (!node) return false;
    uint64_t cur = tree;
    bool ok = false;
    for (int depth = 0; depth < 16 && cur; depth++) {
        if (!ap_block(f, cur, node)) break;
        btn n;
        if (!btn_open(&n, node, f->bs)) break;
        int pick = -1;
        uint64_t best_xid = 0;
        for (uint32_t i = 0; i < n.nkeys; i++) {
            const uint8_t *k, *v;
            uint32_t kl, vl;
            if (!btn_entry(&n, i, &k, &kl, &v, &vl, 16, 16)) continue;
            uint64_t ko = le64(k), kx = le64(k + 8);
            if (n.flags & BT_LEAF) {
                if (ko == oid && kx <= xid && (pick < 0 || kx >= best_xid)) { pick = (int)i; best_xid = kx; }
            } else if (ko < oid || (ko == oid && kx <= xid))
                pick = (int)i;
        }
        if (pick < 0) break;
        const uint8_t *k, *v;
        uint32_t kl, vl;
        btn_entry(&n, (uint32_t)pick, &k, &kl, &v, &vl, 16, 16);
        if (n.flags & BT_LEAF) {
            if (vl >= 16) { *paddr = le64(v + 8); ok = true; }
            break;
        }
        cur = le64(v);
    }
    free(node);
    return ok;
}

/* ---- the file system tree ------------------------------------------------------------------- */

typedef struct {
    uint64_t id;
    uint32_t type;
} want_t;

/* key order: object id, then type (the high 4 bits of the 64-bit field) */
static int cmp_prefix(const uint8_t *k, const want_t *w)
{
    uint64_t v = le64(k), id = v & 0x0FFFFFFFFFFFFFFFull;
    uint32_t t = (uint32_t)(v >> 60);
    if (id != w->id) return id < w->id ? -1 : 1;
    if (t != w->type) return t < w->type ? -1 : 1;
    return 0;
}

typedef bool (*rec_fn)(void *ctx, const uint8_t *key, uint32_t kl, const uint8_t *val, uint32_t vl);

/* Calls fn for every record whose (id, type) equals w, in tree order; false from fn stops. */
static bool fs_walk(apfs *f, const apvol *vol, uint64_t vol_omap_tree, uint64_t oid_or_blk, bool virt, const want_t *w, rec_fn fn, void *ctx, int depth, bool *stop)
{
    if (depth > 12 || *stop) return true;
    uint64_t blk = oid_or_blk;
    if (virt && !omap_lookup(f, vol_omap_tree, oid_or_blk, vol->xid, &blk)) return false;
    uint8_t *node = malloc(f->bs);
    if (!node) return false;
    bool ok = ap_block(f, blk, node);
    btn n;
    if (ok) ok = btn_open(&n, node, f->bs);
    if (ok) {
        /* the keys and values of a non-fixed tree are variable; the fs tree is never fixed */
        uint32_t fk = 16, fv = 16;
        for (uint32_t i = 0; i < n.nkeys && !*stop; i++) {
            const uint8_t *k, *v;
            uint32_t kl, vl;
            if (!btn_entry(&n, i, &k, &kl, &v, &vl, fk, fv) || kl < 8) continue;
            if (n.flags & BT_LEAF) {
                if (cmp_prefix(k, w) == 0 && !fn(ctx, k, kl, v, vl)) *stop = true;
            } else {
                /* this child covers keys from k up to the next entry's key */
                if (cmp_prefix(k, w) > 0) break;
                if (i + 1 < n.nkeys) {
                    const uint8_t *k2, *v2;
                    uint32_t kl2, vl2;
                    if (btn_entry(&n, i + 1, &k2, &kl2, &v2, &vl2, fk, fv) && kl2 >= 8 && cmp_prefix(k2, w) < 0) continue;
                }
                if (vl < 8) continue;
                fs_walk(f, vol, vol_omap_tree, le64(v), true, w, fn, ctx, depth + 1, stop);
            }
        }
    }
    free(node);
    return ok;
}

typedef struct {
    apfs *f;
    const apvol *vol;
    uint64_t omap_tree;
} apctx;

static bool ap_walk(apctx *c, const want_t *w, rec_fn fn, void *ctx)
{
    bool stop = false;
    return fs_walk(c->f, c->vol, c->omap_tree, c->vol->root_oid, true, w, fn, ctx, 0, &stop);
}

/* inode: mode, size, private id */
typedef struct { bool found; uint16_t mode; uint64_t size, private_id, parent; } inode_t;

static bool inode_rec(void *ctx, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    (void)k; (void)kl;
    inode_t *in = ctx;
    if (vl < 92) return true;
    in->found = true;
    in->parent = le64(v);
    in->private_id = le64(v + 8);
    in->mode = le16(v + 80);
    in->size = le64(v + 84);                                /* uncompressed size, replaced by the dstream below */
    /* extended fields: the data stream carries the real size */
    if (vl >= 96) {
        uint32_t nx = le16(v + 92), used = le16(v + 94);
        const uint8_t *xf = v + 96, *data = xf + nx * 4;
        uint32_t off = 0;
        if (96 + nx * 4 + used <= vl)
            for (uint32_t i = 0; i < nx; i++) {
                uint8_t type = xf[i * 4];
                uint32_t sz = le16(xf + i * 4 + 2);
                if (type == 8 && sz >= 8 && data + off + 8 <= v + vl) in->size = le64(data + off);
                off += (sz + 7) & ~7u;
            }
    }
    return false;
}

static bool ap_inode(apctx *c, uint64_t id, inode_t *out)
{
    memset(out, 0, sizeof(*out));
    want_t w = { id, T_INODE };
    ap_walk(c, &w, inode_rec, out);
    return out->found;
}

typedef struct { archive_t *a; apctx *c; int parent; int vol; int count; } dirctx;

static bool drec_rec(void *ctx, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    dirctx *d = ctx;
    if (kl < 11 || vl < 18) return true;
    uint32_t nl;
    const uint8_t *nm;
    uint32_t hashed = le32(k + 8);
    if (kl == 12 + (hashed & 0x3FF)) { nl = hashed & 0x3FF; nm = k + 12; }     /* hashed key (case-insensitive volumes) */
    else { nl = le16(k + 8); nm = k + 10; if (10 + nl > kl) return true; }
    if (nl && nm[nl - 1] == 0) nl--;
    if (!nl) return true;
    uint64_t fid = le64(v);
    uint16_t dtype = le16(v + 16) & 0xF;
    if (dtype != 4 && dtype != 8 && dtype != 10) return true;
    char name[256];
    fs_name(name, sizeof(name), nm, nl, "unnamed");
    inode_t in;
    uint64_t size = 0, priv = fid;
    if (ap_inode(d->c, fid, &in)) { size = in.size; priv = in.private_id; }
    int idx = tree_add(&d->a->tree, d->parent, name, dtype == 4, dtype == 4 ? 0 : size, fid, priv, (uint32_t)d->vol);
    (void)idx;
    return ++d->count < FS_MAX_DIR_ENTRIES;
}

/* ---- volumes ------------------------------------------------------------------- */

static bool volume_ctx(apfs *f, int vi, apctx *c, uint64_t *omap_tree)
{
    c->f = f;
    c->vol = &f->vols[vi];
    c->omap_tree = *omap_tree = c->vol->omap_tree;
    return c->omap_tree != 0;
}

static bool apfs_expand(archive_t *a, int node)
{
    apfs *f = a->priv;
    tnode *n = &a->tree.n[node];
    if (node == 0) return true;                                 /* the volumes were added when the archive was opened */
    int vi = (int)n->c;
    if (vi < 0 || vi >= f->nvols || f->vols[vi].encrypted) return true;
    apctx c;
    uint64_t t;
    if (!volume_ctx(f, vi, &c, &t)) return false;
    dirctx d = { a, &c, node, vi, 0 };
    want_t w = { n->a, T_DREC };
    ap_walk(&c, &w, drec_rec, &d);
    return true;
}

typedef struct { uint64_t logical, len, phys; } ap_ext_t;
typedef struct { ap_ext_t *e; uint32_t n, cap; } ap_exts;

static bool extent_rec(void *ctx, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    ap_exts *x = ctx;
    if (kl < 16 || vl < 16) return true;
    if (x->n == x->cap) {
        uint32_t nc = x->cap ? x->cap * 2 : 16;
        ap_ext_t *ne = realloc(x->e, nc * sizeof(ap_ext_t));
        if (!ne) return false;
        x->e = ne;
        x->cap = nc;
    }
    x->e[x->n++] = (ap_ext_t){ le64(k + 8), le64(v) & 0x00FFFFFFFFFFFFFFull, le64(v + 8) };
    return true;
}

static int32_t apfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    apfs *f = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    int vi = (int)n->c;
    apctx c;
    uint64_t t;
    if (vi < 0 || vi >= f->nvols || !volume_ctx(f, vi, &c, &t)) return -1;
    ap_exts x = { 0 };
    want_t w = { n->b, T_EXTENT };
    ap_walk(&c, &w, extent_rec, &x);
    uint8_t *out = buf;
    memset(out, 0, len);                                        /* holes and unmapped ranges read as zero */
    for (uint32_t i = 0; i < x.n; i++) {
        uint64_t lo = x.e[i].logical, hi = lo + x.e[i].len;
        if (hi <= off || lo >= off + len || !x.e[i].phys) continue;
        uint64_t from = lo > off ? lo : off, to = hi < off + len ? hi : off + len;
        if (!fsc_read(&f->c, x.e[i].phys * f->bs + (from - lo), out + (from - off), (uint32_t)(to - from))) { free(x.e); return -1; }
    }
    free(x.e);
    return (int32_t)len;
}

static void apfs_destroy(archive_t *a)
{
    apfs *f = a->priv;
    if (!f) return;
    fsc_free(&f->c);
    free(f->vols);
    free(f->blk);
    free(f);
}

/* the newest valid container superblock: block 0 or one of the checkpoint descriptor blocks */
static bool find_superblock(apfs *f, uint8_t *sb)
{
    uint8_t *tmp = malloc(f->bs);
    if (!tmp) return false;
    if (!ap_block(f, 0, sb)) { free(tmp); return false; }
    uint64_t best = le64(sb + 16);
    uint32_t nblocks = le32(sb + 104);
    int64_t base = (int64_t)le64(sb + 112);
    if (base > 0 && nblocks && nblocks < 65536) {
        for (uint32_t i = 0; i < nblocks; i++) {
            if (!ap_block(f, (uint64_t)base + i, tmp)) continue;
            if (!nx_ok(tmp) || (le32(tmp + 24) & 0xFFFF) != 1) continue;
            if (le32(tmp + 36) != f->bs) continue;
            uint64_t xid = le64(tmp + 16);
            if (xid > best) { best = xid; memcpy(sb, tmp, f->bs); }
        }
    }
    free(tmp);
    return true;
}

archive_t *arc_apfs_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t head[64];
    if (!arc_apfs_probe(file, name) || !src_read_full(file, 0, head, sizeof(head))) return NULL;
    apfs *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->bs = le32(head + 36);
    uint8_t *sb = malloc(f->bs), *vb = malloc(f->bs), *ob = malloc(f->bs);
    archive_t *a = NULL;
    const char *err = "Can't read the APFS container";
    if (sb && vb && ob && fsc_init(&f->c, file) && find_superblock(f, sb)) {
        f->xid = le64(sb + 16);
        uint64_t omap_oid = le64(sb + 160);
        uint64_t omap_tree = 0;
        if (ap_block(f, omap_oid, ob)) omap_tree = le64(ob + 48);
        uint32_t maxfs = le32(sb + 180);
        if (maxfs > 100) maxfs = 100;
        f->vols = calloc(maxfs ? maxfs : 1, sizeof(apvol));
        a = f->vols ? archive_alloc("APFS") : NULL;
        for (uint32_t i = 0; a && i < maxfs && omap_tree; i++) {
            uint64_t voloid = le64(sb + 184 + i * 8), paddr;
            if (!voloid || !omap_lookup(f, omap_tree, voloid, f->xid, &paddr) || !ap_block(f, paddr, vb)) continue;
            if (le32(vb + 32) != APSB_MAGIC) continue;
            apvol *v = &f->vols[f->nvols];
            v->xid = le64(vb + 16);
            v->incompat = (uint32_t)le64(vb + 56);
            v->encrypted = !(le64(vb + 264) & 1);               /* APFS_FS_UNENCRYPTED */
            v->root_oid = le64(vb + 136);
            uint64_t vomap = le64(vb + 128);
            if (!ap_block(f, vomap, ob)) continue;
            v->omap_tree = le64(ob + 48);
            memcpy(v->name, vb + 704, 255);
            v->name[255] = 0;
            char label[300];
            snprintf(label, sizeof(label), "%s%s", v->name[0] ? v->name : "Volume", v->encrypted ? " (encrypted)" : "");
            tree_clean_name(label);
            if (!label[0]) snprintf(label, sizeof(label), "Volume %d", f->nvols + 1);
            /* two volumes can share a name: make it unique */
            if (tree_find_child(&a->tree, 0, label) >= 0) {
                size_t l = strlen(label);
                snprintf(label + (l < 280 ? l : 280), 16, " %d", f->nvols + 1);
            }
            int idx = tree_add(&a->tree, 0, label, true, 0, 2 /* root directory id */, 0, (uint32_t)f->nvols);
            if (idx >= 0) { a->tree.n[idx].loaded = v->encrypted ? true : false; f->nvols++; }
        }
        if (a && !f->nvols) { archive_close(a); a = NULL; err = "No readable APFS volume"; }
    }
    free(sb); free(vb); free(ob);
    if (!a) {
        g_archive_err = err;
        fsc_free(&f->c);
        free(f->vols);
        free(f);
        return NULL;
    }
    a->priv = f;
    a->destroy = apfs_destroy;
    a->read = apfs_read;
    a->expand = apfs_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].loaded = true;
    return a;
}

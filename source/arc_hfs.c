/*
 * HFS+ and HFSX ("Mac OS Extended", journaled or not), read-only: volume header, the catalog B-tree
 * (folders, files, data forks with their extents overflow records), UTF-16 names. The journal is
 * ignored; compressed (decmpfs) files and resource forks are not shown. HFS+ volumes wrapped in a
 * classic HFS wrapper are followed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"

#define HFS_ROOT 2

typedef struct { uint32_t start, count; } hext;

typedef struct {
    uint64_t size;
    uint32_t blocks;
    hext ext[8];
} hfork;

typedef struct {
    fsc_t c;
    uint64_t base;              /* start of the HFS+ volume inside the source (wrapper) */
    uint32_t bs;
    hfork catalog, extents;
    uint32_t cat_node, cat_root, cat_first;
    uint32_t ext_node, ext_root, ext_first;
    bool hfsx;
    hfork *forks;               /* by tree node index (files only) */
    int nforks;
} hfs;

static void fork_parse(const uint8_t *p, hfork *f)
{
    f->size = be64(p);
    f->blocks = be32(p + 12);
    for (int i = 0; i < 8; i++) {
        f->ext[i].start = be32(p + 16 + i * 8);
        f->ext[i].count = be32(p + 20 + i * 8);
    }
}

static bool hfs_header(const src_t *f, uint64_t *base, uint8_t vh[512])
{
    uint8_t h[512];
    uint64_t b = 0;
    if (f->size < 4096 || !src_read_full(f, 1024, h, 512)) return false;
    if (be16(h) == 0x4244 && be16(h + 0x7C) == 0x482B) {          /* HFS wrapper around HFS+ */
        uint32_t alblksz = be32(h + 0x14), alblkst = be16(h + 0x1C);
        uint32_t estart = be16(h + 0x7E);
        b = (uint64_t)alblkst * 512 + (uint64_t)estart * alblksz;
        if (b + 2048 > f->size || !src_read_full(f, b + 1024, h, 512)) return false;
    }
    uint16_t sig = be16(h);
    if ((sig != 0x482B && sig != 0x4858) || be16(h + 2) < 4 || be16(h + 2) > 5) return false;
    uint32_t bs = be32(h + 40);
    if (bs < 512 || bs > (1u << 24) || (bs & (bs - 1))) return false;
    *base = b;
    memcpy(vh, h, 512);
    return true;
}

bool arc_hfs_probe(const src_t *file, const char *name)
{
    (void)name;
    uint64_t b;
    uint8_t vh[512];
    return hfs_header(file, &b, vh);
}

/* physical byte of allocation block `blk` of a fork whose first 8 extents are in `f`; the rest come from the
 * extents overflow tree (file `cnid`, fork 0 = data / 0xFF = resource). */
static bool node_read(hfs *h, const hfork *tree, uint32_t node, uint32_t node_size, uint8_t *buf);
static bool fork_read_raw(hfs *h, const hfork *f, uint64_t off, uint8_t *buf, uint32_t len);

static bool overflow_find(hfs *h, uint32_t cnid, uint32_t startblock, hext out[8])
{
    uint32_t ns = h->ext_node;
    if (!ns) return false;
    uint8_t *node = malloc(ns);
    if (!node) return false;
    bool found = false;
    uint32_t cur = h->ext_first, guard = 0;
    while (cur && guard++ < 1000000 && !found) {
        if (!node_read(h, &h->extents, cur, ns, node)) break;
        uint32_t nrec = be16(node + 10);
        for (uint32_t r = 0; r < nrec; r++) {
            uint32_t ro = be16(node + ns - 2 * (r + 1));
            if (ro + 12 + 64 > ns) continue;
            const uint8_t *k = node + ro;
            if (be16(k) < 10 || k[2] != 0 || be32(k + 4) != cnid) continue;
            if (be32(k + 8) == startblock) {
                for (int i = 0; i < 8; i++) { out[i].start = be32(k + 12 + i * 8); out[i].count = be32(k + 16 + i * 8); }
                found = true;
                break;
            }
        }
        cur = be32(node);                           /* fLink */
    }
    free(node);
    return found;
}

static bool fork_block(hfs *h, const hfork *f, uint32_t cnid, uint32_t blk, uint64_t *phys, uint32_t *run)
{
    uint32_t pos = 0;
    hext more[8];
    const hext *e = f->ext;
    int guard = 0;
    for (;;) {
        for (int i = 0; i < 8; i++) {
            if (!e[i].count) { if (pos >= f->blocks) return false; continue; }
            if (blk < pos + e[i].count) {
                *phys = h->base + ((uint64_t)e[i].start + (blk - pos)) * h->bs;
                *run = pos + e[i].count - blk;
                return true;
            }
            pos += e[i].count;
        }
        if (pos >= f->blocks || guard++ > 100000 || !cnid) return false;
        if (!overflow_find(h, cnid, pos, more)) return false;
        e = more;
    }
}

static bool fork_read(hfs *h, const hfork *f, uint32_t cnid, uint64_t off, uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t blk = (uint32_t)(off / h->bs), run, skew = (uint32_t)(off % h->bs);
        uint64_t phys;
        if (!fork_block(h, f, cnid, blk, &phys, &run)) return false;
        uint32_t n = run * h->bs - skew;
        if (n > len) n = len;
        if (!fsc_read(&h->c, phys + skew, buf, n)) return false;
        buf += n; off += n; len -= n;
    }
    return true;
}

/* the catalog / extents files only have their 8 inline extents (plus overflow for the catalog is not
 * needed by real volumes: they are kept contiguous) */
static bool fork_read_raw(hfs *h, const hfork *f, uint64_t off, uint8_t *buf, uint32_t len)
{
    return fork_read(h, f, f == &h->catalog ? 4 : 0, off, buf, len);     /* CNID 4: the catalog may have overflow extents */
}

static bool node_read(hfs *h, const hfork *tree, uint32_t node, uint32_t node_size, uint8_t *buf)
{
    return fork_read_raw(h, tree, (uint64_t)node * node_size, buf, node_size);
}

static void utf16_name(const uint8_t *p, uint32_t n, char *out, size_t outsz)
{
    uint16_t u[256];
    if (n > 255) n = 255;
    for (uint32_t i = 0; i < n; i++) u[i] = be16(p + 2 * i);
    fs_utf16_to_utf8(u, n, out, outsz);
    tree_clean_name(out);
    if (!out[0]) snprintf(out, outsz, "unnamed");
}

/* every catalog record whose key has parent `dir`, in order */
static bool list_dir(archive_t *a, int parent_node, uint32_t dir)
{
    hfs *h = a->priv;
    uint32_t ns = h->cat_node;
    uint8_t *node = malloc(ns);
    if (!node) return false;
    /* descend to the leaf that may hold the first record of this parent */
    uint32_t cur = h->cat_root, depth = 0;
    bool ok = true;
    while (cur && depth++ < 16) {
        if (!node_read(h, &h->catalog, cur, ns, node)) { ok = false; break; }
        int kind = (int8_t)node[8];
        if (kind == -1) break;                                  /* leaf */
        if (kind != 0) { ok = false; break; }
        uint32_t nrec = be16(node + 10), pick = 0;
        for (uint32_t r = 0; r < nrec; r++) {
            uint32_t ro = be16(node + ns - 2 * (r + 1));
            if (ro + 8 > ns) continue;
            if (be32(node + ro + 2) < dir) pick = r;
        }
        uint32_t ro = be16(node + ns - 2 * (pick + 1));
        uint32_t klen = be16(node + ro), po = (ro + 2 + klen + 1) & ~1u;
        if (po + 4 > ns) { ok = false; break; }
        cur = be32(node + po);
    }
    int count = 0;
    uint32_t guard = 0;
    bool past = false;
    while (ok && cur && !past && guard++ < 1000000) {
        if (!node_read(h, &h->catalog, cur, ns, node) || (int8_t)node[8] != -1) break;
        uint32_t nrec = be16(node + 10);
        for (uint32_t r = 0; r < nrec; r++) {
            uint32_t ro = be16(node + ns - 2 * (r + 1));
            if (ro + 8 > ns) continue;
            uint32_t klen = be16(node + ro), parent = be32(node + ro + 2);
            if (parent < dir) continue;
            if (parent > dir) { past = true; break; }
            uint32_t nlen = be16(node + ro + 6);
            uint32_t rec = ro + 2 + klen;
            rec = (rec + 1) & ~1u;
            if (rec + 8 > ns || ro + 8 + nlen * 2 > ns) continue;
            int16_t type = (int16_t)be16(node + rec);
            char name[256];
            utf16_name(node + ro + 8, nlen, name, sizeof(name));
            if (!strncmp(name, "HFS+ Private", 12) || !strncmp(name, ".HFS+ Private", 13) || !strncmp(name, "\xE2\x90\x80\xE2\x90\x80\xE2\x90\x80\xE2\x90\x80HFS+", 15))
                continue;
            if (type == 1 && rec + 12 <= ns) {
                tree_add(&a->tree, parent_node, name, true, 0, be32(node + rec + 8), 0, 0);
                count++;
            } else if (type == 2 && rec + 0x58 + 80 <= ns) {
                hfork f;
                fork_parse(node + rec + 0x58, &f);
                int idx = tree_add(&a->tree, parent_node, name, false, f.size, be32(node + rec + 8), 0, 0);
                if (idx >= 0) {
                    if (idx >= h->nforks) {
                        int nn = idx + 64;
                        hfork *nf = realloc(h->forks, nn * sizeof(hfork));
                        if (!nf) continue;
                        memset(nf + h->nforks, 0, (nn - h->nforks) * sizeof(hfork));
                        h->forks = nf;
                        h->nforks = nn;
                    }
                    h->forks[idx] = f;
                }
                count++;
            }
            if (count > FS_MAX_DIR_ENTRIES) { past = true; break; }
        }
        cur = be32(node);
    }
    free(node);
    return ok;
}

static int32_t hfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    hfs *h = a->priv;
    int idx = (int)(n - a->tree.n);
    if (idx < 0 || idx >= h->nforks) return -1;
    const hfork *f = &h->forks[idx];
    if (off >= f->size) return 0;
    if (len > f->size - off) len = (uint32_t)(f->size - off);
    return fork_read(h, f, (uint32_t)n->a, off, buf, len) ? (int32_t)len : -1;
}

static bool hfs_expand(archive_t *a, int node) { return list_dir(a, node, (uint32_t)a->tree.n[node].a); }

static void hfs_destroy(archive_t *a)
{
    hfs *h = a->priv;
    if (!h) return;
    fsc_free(&h->c);
    free(h->forks);
    free(h);
}

archive_t *arc_hfs_open(const src_t *file, const char *name)
{
    (void)name;
    uint64_t base;
    uint8_t vh[512];
    if (!hfs_header(file, &base, vh)) return NULL;
    hfs *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->base = base;
    h->bs = be32(vh + 40);
    h->hfsx = be16(vh) == 0x4858;
    /* the volume header layout: allocation file 0x70, extents overflow 0xC0, catalog 0x110 */
    fork_parse(vh + 0xC0, &h->extents);
    fork_parse(vh + 0x110, &h->catalog);
    archive_t *a = NULL;
    uint8_t node0[512];
    bool ok = fsc_init(&h->c, file);
    ok = ok && fork_read_raw(h, &h->catalog, 0, node0, sizeof(node0));
    if (ok) {
        h->cat_node = be16(node0 + 14 + 18);
        h->cat_root = be32(node0 + 14 + 2);
        h->cat_first = be32(node0 + 14 + 10);
        ok = h->cat_node >= 512 && h->cat_node <= 32768 && !(h->cat_node & (h->cat_node - 1)) && h->cat_root;
    }
    if (ok && h->extents.size >= 512 && fork_read_raw(h, &h->extents, 0, node0, sizeof(node0))) {
        h->ext_node = be16(node0 + 14 + 18);
        h->ext_root = be32(node0 + 14 + 2);
        h->ext_first = be32(node0 + 14 + 10);
        if (h->ext_node < 512 || h->ext_node > 32768 || (h->ext_node & (h->ext_node - 1))) h->ext_node = 0;
    }
    if (ok) a = archive_alloc(h->hfsx ? "HFSX" : "HFS+");
    if (!a) {
        g_archive_err = ok ? NULL : "Can't read the HFS+ catalog";
        fsc_free(&h->c);
        free(h);
        return NULL;
    }
    a->priv = h;
    a->destroy = hfs_destroy;
    a->read = hfs_read;
    a->expand = hfs_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].a = HFS_ROOT;
    return a;
}

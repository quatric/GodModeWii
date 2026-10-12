/*
 * WFS: the file system of the Wii U's USB drives and MLC, read-only. The layout follows koolkdev's
 * wfslib / wfs-tools: a device header and area header in block 0, areas ("quota" folders) with their
 * own block numbering and IV, directories stored as prefix tries in 0x1000 / 0x2000 byte blocks, and
 * files stored inline, as block lists, large block lists, clusters or lists of cluster metadata blocks.
 * Everything is AES-128-CBC encrypted per block with a key derived from the console's OTP / SEEPROM
 * (or the plain, already decrypted image). Hashes are not checked, except to recover the IV parameters
 * of block 0 the way wfslib's recovery code does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archive.h"
#include "sha1.h"

#define WFS_VERSION 0x01010800u
#define MIN_SIZE 0x40000u
#define CACHE_N 16
#define MAX_AREAS 2048
#define MAX_ENTRIES 400000

#define F_UNENCRYPTED 0x02000000u
#define F_LINK        0x04000000u
#define F_BASIC       0x10000000u
#define F_REGULAR     0x20000000u
#define F_QUOTA       0x40000000u
#define F_DIR         0x80000000u

#define BLOCK_LEAF_TREE 0x20000000u

typedef struct {
    uint32_t phys;          /* header block, in 0x1000 units */
    uint32_t ivx;           /* area iv ^ device iv */
    uint8_t bs;             /* log2 block size: 12 / 13 */
    uint32_t root_dir;      /* area block number */
} warea;

typedef struct {
    uint64_t addr;
    uint32_t size;
    uint32_t stamp;
    uint8_t *data;
} wcache;

typedef struct {
    bool enc;
    aes128_t aes;
    uint32_t sectors;
    uint32_t log2ss;
    uint32_t dev_iv;
    warea *areas;
    int narea;
    wcache cache[CACHE_N];
    uint32_t tick;
    int entries;
} wfspriv;

static uint32_t pow2_ceil(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* ---------------------------------------------------------------- blocks */

static void make_iv(const wfspriv *w, uint32_t size, uint32_t iv1, uint8_t iv[16])
{
    uint32_t v[4] = { size, iv1, w->sectors, 1u << w->log2ss };
    for (int i = 0; i < 4; i++) {
        iv[i * 4] = (uint8_t)(v[i] >> 24); iv[i * 4 + 1] = (uint8_t)(v[i] >> 16);
        iv[i * 4 + 2] = (uint8_t)(v[i] >> 8); iv[i * 4 + 3] = (uint8_t)v[i];
    }
}

static uint64_t block_addr(const warea *ar, uint32_t abn)
{
    return ((uint64_t)ar->phys + ((uint64_t)abn << (ar->bs - 12))) << 12;
}

static uint32_t block_iv1(const wfspriv *w, const warea *ar, uint32_t abn)
{
    int sh = (int)ar->bs - (int)w->log2ss;
    uint32_t off = sh >= 0 ? (abn << sh) : (abn >> -sh);
    return ar->ivx + off;
}

/* A metadata block of `size` bytes; valid until a few more blocks have been loaded. */
static const uint8_t *meta(archive_t *a, const warea *ar, uint32_t abn, uint32_t size)
{
    wfspriv *w = a->priv;
    uint64_t addr = block_addr(ar, abn);
    if (addr + size > a->src.size) return NULL;
    wcache *free_slot = NULL, *lru = &w->cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        wcache *c = &w->cache[i];
        if (c->data && c->addr == addr && c->size == size) { c->stamp = ++w->tick; return c->data; }
        if (!c->data) free_slot = c;
        else if (c->stamp < lru->stamp || !lru->data) lru = c;
    }
    wcache *slot = free_slot ? free_slot : lru;
    if (!slot->data || slot->size != size) {
        free(slot->data);
        slot->data = malloc(size);
        if (!slot->data) { slot->size = 0; return NULL; }
    }
    slot->addr = (uint64_t)-1;
    slot->size = size;
    if (!src_read_full(&a->src, addr, slot->data, size)) return NULL;
    if (w->enc) {
        uint8_t iv[16];
        make_iv(w, size, block_iv1(w, ar, abn), iv);
        aes128_cbc_decrypt(&w->aes, iv, slot->data, slot->data, size);
    }
    slot->addr = addr;
    slot->stamp = ++w->tick;
    return slot->data;
}

/* ---------------------------------------------------------------- areas */

static int area_add(wfspriv *w, uint32_t phys, uint32_t ivx, uint8_t bs, uint32_t root_dir)
{
    for (int i = 0; i < w->narea; i++)
        if (w->areas[i].phys == phys) return i;
    if (w->narea >= MAX_AREAS) return -1;
    warea *n = realloc(w->areas, (size_t)(w->narea + 1) * sizeof(warea));
    if (!n) return -1;
    w->areas = n;
    w->areas[w->narea] = (warea){ phys, ivx, bs, root_dir };
    return w->narea++;
}

/* The quota area whose header is block abn of area `parent`. */
static int open_area(archive_t *a, int parent, uint32_t abn, uint32_t flags)
{
    wfspriv *w = a->priv;
    warea pa = w->areas[parent];
    uint8_t bs = (!(flags & F_BASIC) && (flags & F_REGULAR)) ? 13 : 12;
    const uint8_t *hdr = meta(a, &pa, abn, 1u << bs);
    if (!hdr) return -1;
    uint8_t hbs = hdr[0x18 + 21];
    if (hbs != 12 && hbs != 13) return -1;
    uint32_t phys = pa.phys + (abn << (pa.bs - 12));
    uint32_t ivx = be32(hdr + 0x18) ^ w->dev_iv;
    return area_add(w, phys, ivx, hbs, be32(hdr + 0x18 + 8));
}

/* ---------------------------------------------------------------- directories */

typedef struct {
    archive_t *a;
    wfspriv *w;
    int area;
    int parent;
    bool ok;
} walk_t;

typedef bool (*leaf_fn)(walk_t *wk, const uint8_t *blk, uint32_t blksz, uint32_t abn, const char *key, int keylen, uint32_t value);

static bool entry_add(walk_t *wk, const uint8_t *blk, uint32_t blksz, uint32_t abn, const char *key, int keylen, uint32_t off)
{
    if (off + 0x2C > blksz) return true;                    /* damaged entry: skip it */
    const uint8_t *e = blk + off;
    uint32_t flags = be32(e);
    if (flags & F_LINK) return true;
    int nlen = e[0x2A];
    if (nlen != keylen) nlen = keylen;
    int bm = (nlen + 7) / 8;
    if (off + 0x2B + (uint32_t)bm > blksz) return true;
    char name[256];
    for (int i = 0; i < nlen; i++) {
        char ch = key[i];
        bool up = (e[0x2B + i / 8] >> (i % 8)) & 1;
        name[i] = up ? (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch) : ch;
    }
    name[nlen] = '\0';
    if (!name[0]) return true;
    if (++wk->w->entries > MAX_ENTRIES) return false;
    if (flags & F_DIR) {
        uint32_t dirblk = be32(e + 0x18);
        int idx = tree_add(&wk->a->tree, wk->parent, name, true, 0, (uint64_t)wk->area, dirblk, flags);
        return idx >= 0;
    }
    uint32_t fsize = be32(e + 0x14);
    int idx = tree_add(&wk->a->tree, wk->parent, name, false, fsize, (uint64_t)wk->area, ((uint64_t)abn << 16) | off, flags);
    return idx >= 0;
}

static bool dir_block(walk_t *wk, uint32_t abn, int depth);

static bool parent_leaf(walk_t *wk, const uint8_t *blk, uint32_t blksz, uint32_t abn, const char *key, int keylen, uint32_t value)
{
    (void)blk; (void)blksz; (void)abn; (void)key; (void)keylen;
    return dir_block(wk, value, 1);
}

static bool entry_leaf(walk_t *wk, const uint8_t *blk, uint32_t blksz, uint32_t abn, const char *key, int keylen, uint32_t value)
{
    return entry_add(wk, blk, blksz, abn, key, keylen, value);
}

typedef struct {
    const uint8_t *blk;
    uint32_t blksz, abn;
    int leaf_size;                  /* 2: entry offsets, 4: child block numbers */
    leaf_fn fn;
    int budget;
    char path[260];
} trie_t;

static bool trie_node(walk_t *wk, trie_t *t, uint32_t off, int plen_so_far, int depth)
{
    if (depth > 255 || --t->budget < 0) return true;
    const uint8_t *blk = t->blk;
    if (off + 2 > t->blksz) return true;
    int prefix = blk[off];
    int kc = blk[off + 1];
    if (kc == 0 && prefix == 0) return true;
    if (off + 2 + (uint32_t)prefix + (uint32_t)kc > t->blksz) return true;
    const uint8_t *keys = blk + off + 2 + prefix;
    bool has_leaf = kc > 0 && keys[0] == 0;
    uint32_t size = 2 + (uint32_t)prefix + (uint32_t)kc * 3 + ((t->leaf_size == 4 && has_leaf) ? 2 : 0);
    uint32_t nsize = pow2_ceil(size);
    if (off + nsize > t->blksz) return true;
    uint32_t end = off + nsize - 2;
    if (t->leaf_size != 2 && has_leaf) end -= (uint32_t)(t->leaf_size - 2);
    if (plen_so_far + prefix > 255) return true;
    memcpy(t->path + plen_so_far, blk + off + 2, (size_t)prefix);
    int plen = plen_so_far + prefix;
    for (int i = 0; i < kc; i++) {
        uint32_t vpos = end - 2u * (uint32_t)i;
        if (vpos + 2 > t->blksz || vpos < off) return true;
        if (i == 0 && has_leaf) {
            if (t->leaf_size == 4 && vpos + 4 > t->blksz) return true;
            uint32_t v = t->leaf_size == 4 ? be32(blk + vpos) : be16(blk + vpos);
            if (!t->fn(wk, blk, t->blksz, t->abn, t->path, plen, v)) return false;
            continue;
        }
        if (plen >= 255) continue;
        t->path[plen] = (char)keys[i];
        if (!trie_node(wk, t, be16(blk + vpos), plen + 1, depth + 1)) return false;
    }
    return true;
}

static bool dir_block(walk_t *wk, uint32_t abn, int depth)
{
    if (depth > 8) return true;
    wfspriv *w = wk->w;
    warea *ar = &w->areas[wk->area];
    uint32_t blksz = 1u << ar->bs;
    const uint8_t *b = meta(wk->a, ar, abn, blksz);
    if (!b) return false;
    bool leaf = (be32(b) & BLOCK_LEAF_TREE) != 0;
    uint32_t root = be16(b + 0x38), records = be16(b + 0x3A);
    if (!records || root + 2 > blksz) return true;
    trie_t *t = malloc(sizeof(*t));
    if (!t) return false;
    t->blk = b;
    t->blksz = blksz;
    t->abn = abn;
    t->leaf_size = leaf ? 2 : 4;
    t->fn = leaf ? entry_leaf : parent_leaf;
    t->budget = (int)(blksz / 4);
    /* the parent tree recurses into other blocks, which could push this one out of the cache: work on a copy */
    uint8_t *copy = NULL;
    if (!leaf) {
        copy = malloc(blksz);
        if (!copy) { free(t); return false; }
        memcpy(copy, b, blksz);
        t->blk = copy;
    }
    bool ok = trie_node(wk, t, root, 0, 0);
    free(copy);
    free(t);
    return ok;
}

static bool wfs_expand(archive_t *a, int node)
{
    wfspriv *w = a->priv;
    tnode *n = &a->tree.n[node];
    walk_t wk = { a, w, 0, node, true };
    uint32_t dirblk;
    if (node == 0) {
        wk.area = 0;
        dirblk = w->areas[0].root_dir;
    } else if (n->c & F_QUOTA) {
        int idx = open_area(a, (int)n->a, (uint32_t)n->b, n->c);
        if (idx < 0) return false;
        wk.area = idx;
        dirblk = w->areas[idx].root_dir;
    } else {
        wk.area = (int)n->a;
        dirblk = (uint32_t)n->b;
    }
    return dir_block(&wk, dirblk, 0);
}

/* ---------------------------------------------------------------- file data */

/* Where data block `idx` (in units of the block type) of the entry lives. `e` is a copy of the entry (elen
 * bytes); its lists of blocks sit at the end of the entry's allocation, last block first. */
static bool data_block(archive_t *a, const warea *ar, const uint8_t *e, uint32_t elen, uint32_t sod, int cat,
                       uint32_t idx, uint32_t *abn)
{
    uint32_t bs = ar->bs;
    uint32_t base = 0x2B + (uint32_t)((e[0x2A] + 7) / 8);
    if (cat == 1 || cat == 2) {
        uint32_t unit = 1u << (cat == 1 ? bs : bs + 3);
        uint32_t count = (sod + unit - 1) / unit;
        if (idx >= count) return false;
        uint32_t end = pow2_ceil(base + count * 0x18);
        if (end > elen) return false;
        *abn = be32(e + end - (idx + 1) * 0x18);
        return true;
    }
    uint32_t cl = 1u << (bs + 6);
    uint32_t clusters = (sod + cl - 1) / cl;
    uint32_t cluster = idx / 8, j = idx % 8;
    if (cat == 3) {
        if (cluster >= clusters) return false;
        uint32_t end = pow2_ceil(base + clusters * 0xA4);
        if (end > elen) return false;
        *abn = be32(e + end - (cluster + 1) * 0xA4) + (j << 3);
        return true;
    }
    if (cat == 4) {
        uint32_t per = ((1u << bs) - 0x18) / 0xA4;
        if (per > 48) per = 48;
        uint32_t nmb = (clusters + per - 1) / per;
        uint32_t mbi = cluster / per, inmb = cluster % per;
        if (mbi >= nmb) return false;
        uint32_t end = pow2_ceil(base + nmb * 4);
        if (end > elen) return false;
        const uint8_t *mb = meta(a, ar, be32(e + end - (mbi + 1) * 4), 1u << bs);
        if (!mb) return false;
        *abn = be32(mb + 0x18 + inmb * 0xA4) + (j << 3);
        return true;
    }
    return false;
}

static int32_t wfs_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    wfspriv *w = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    const warea *ar = &w->areas[n->a];
    uint32_t eabn = (uint32_t)(n->b >> 16), eoff = (uint32_t)(n->b & 0xFFFF);
    uint32_t bsz = 1u << ar->bs;
    /* the entry must stay valid while data blocks are read, so keep a copy (entries are at most 1 KiB) */
    uint8_t e[1024];
    const uint8_t *blk = meta(a, ar, eabn, bsz);
    if (!blk || eoff + 0x2C > bsz) return -1;
    uint32_t elen = bsz - eoff;
    if (elen > sizeof(e)) elen = sizeof(e);
    memcpy(e, blk + eoff, elen);
    uint32_t flags = be32(e);
    uint32_t sod = be32(e + 4);
    int cat = e[0x29];
    uint32_t base = 0x2B + (uint32_t)((e[0x2A] + 7) / 8);
    uint8_t *dst = buf;

    if (cat == 0) {
        if (base + off + len > elen) return -1;
        memcpy(dst, e + base + off, len);
        return (int32_t)len;
    }
    if (cat > 4) return -1;
    uint32_t L = ar->bs + (cat == 1 ? 0 : 3);               /* data block size: 1 << L */
    uint32_t fsize = (uint32_t)n->size;
    bool enc = w->enc && !(flags & F_UNENCRYPTED);
    uint32_t ss = 1u << w->log2ss;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos >> L);
        uint32_t in = (uint32_t)(pos & ((1u << L) - 1));
        uint64_t blk_off = (uint64_t)idx << L;
        uint64_t left = (uint64_t)fsize - blk_off;
        uint32_t dsize = left < (1u << L) ? (uint32_t)left : (1u << L);
        uint32_t aligned = (dsize + ss - 1) & ~(ss - 1);    /* the last block is stored (and was encrypted) as whole sectors */
        uint32_t abn;
        if (!data_block(a, ar, e, elen, sod, cat, idx, &abn)) return done ? (int32_t)done : -1;
        uint64_t addr = block_addr(ar, abn);
        if (addr + aligned > a->src.size) return done ? (int32_t)done : -1;
        uint32_t take = dsize - in;
        if (take > len - done) take = len - done;
        if (enc) {
            uint8_t iv[16];
            make_iv(w, aligned, block_iv1(w, ar, abn), iv);
            int32_t r = cbc_stream_read(&a->src, &w->aes, iv, addr, aligned, in, dst + done, take);
            if (r <= 0) return done ? (int32_t)done : -1;
            take = (uint32_t)r;
        } else if (!src_read_full(&a->src, addr + in, dst + done, take)) {
            return done ? (int32_t)done : -1;
        }
        done += take;
    }
    return (int32_t)done;
}

/* ---------------------------------------------------------------- open */

static void wfs_destroy(archive_t *a)
{
    wfspriv *w = a->priv;
    if (!w) return;
    for (int i = 0; i < CACHE_N; i++) free(w->cache[i].data);
    free(w->areas);
    memset(&w->aes, 0, sizeof(w->aes));
    free(w);
}

/* Is `key` (NULL = plain) the key of this volume? Block 1 of the CBC stream decrypts without knowing the IV. */
static bool check_key(const uint8_t head[0x40], const uint8_t *key, uint8_t plain[0x30])
{
    if (!key) {
        memcpy(plain, head + 0x10, 0x30);
    } else {
        aes128_t k;
        aes128_init_dec(&k, key);
        aes128_cbc_decrypt(&k, head, head + 0x10, plain, 0x30);
        memset(&k, 0, sizeof(k));
    }
    /* plain = bytes 0x10..0x3F: version at 0x1C, device type at 0x20 */
    uint16_t dt = be16(plain + 0x10);
    return be32(plain + 0x0C) == WFS_VERSION && (dt == 0x16A2 || dt == 0x136A);
}

bool arc_wfs_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t head[0x40], plain[0x30];
    if (file->size < MIN_SIZE || !src_read_full(file, 0, head, sizeof(head))) return false;
    if (check_key(head, NULL, plain)) return true;
    uint8_t key[16];
    for (int i = 0; arc_wfs_key_at(i, key); i++)
        if (check_key(head, key, plain)) { memset(key, 0, sizeof(key)); return true; }
    memset(key, 0, sizeof(key));
    return false;
}

archive_t *arc_wfs_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t head[0x40], plain[0x30];
    if (file->size < MIN_SIZE || !src_read_full(file, 0, head, sizeof(head))) return NULL;

    uint8_t key[16];
    bool enc = false, found = check_key(head, NULL, plain);
    if (!found) {
        for (int i = 0; arc_wfs_key_at(i, key); i++)
            if (check_key(head, key, plain)) { found = true; enc = true; break; }
    }
    if (!found) {
        g_archive_err = "No key for this WFS volume (put otp.bin + seeprom.bin or usb.key / mlc.key in sd:/wfs/)";
        memset(key, 0, sizeof(key));
        return NULL;
    }
    uint32_t rflags = be32(plain + 0x14);                 /* root_quota_metadata.flags at 0x24 */
    uint32_t root_size = (!(rflags & F_BASIC) && (rflags & F_REGULAR)) ? 0x2000 : 0x1000;

    wfspriv *w = calloc(1, sizeof(*w));
    archive_t *a = archive_alloc("WFS");
    uint8_t *root = malloc(root_size);
    if (!w || !a || !root) { free(w); archive_close(a); free(root); memset(key, 0, sizeof(key)); return NULL; }
    a->priv = w;
    a->destroy = wfs_destroy;
    a->read = wfs_read;
    a->expand = wfs_expand;
    a->base = *file;
    a->src = a->base;
    w->enc = enc;
    w->log2ss = 9;
    bool ok = src_read_full(file, 0, root, root_size);
    uint32_t ivx = 0;
    if (ok && enc) {
        aes128_init_dec(&w->aes, key);
        /* Recover the IV: only its first 16 bytes matter, and those hold the block flags and the start of the
         * block's own SHA-1. Decrypt with a zero IV (apart from the size), hash the block, and what is left
         * over is the real IV (iv1 = area ^ device iv, iv2 = sectors, iv3 = sector size). */
        uint8_t iv[16] = { 0 }, *tmp = malloc(root_size);
        if (!tmp) ok = false;
        else {
            iv[0] = (uint8_t)(root_size >> 24); iv[1] = (uint8_t)(root_size >> 16); iv[2] = (uint8_t)(root_size >> 8); iv[3] = (uint8_t)root_size;
            aes128_cbc_decrypt(&w->aes, iv, root, tmp, root_size);
            uint8_t first[16];
            memcpy(first, tmp, 16);
            memset(tmp + 4, 0xFF, 20);
            sha1_t c; uint8_t h[20];
            sha1_init(&c); sha1_update(&c, tmp, root_size); sha1_final(&c, h);
            uint32_t t1 = be32(first + 4) ^ be32(h), t2 = be32(first + 8) ^ be32(h + 4), t3 = be32(first + 12) ^ be32(h + 8);
            if (t3 < 512 || t3 > 0x10000 || (t3 & (t3 - 1)) || t3 > root_size) ok = false;
            else {
                w->sectors = t2;
                w->log2ss = 0;
                while ((1u << w->log2ss) < t3) w->log2ss++;
                ivx = t1;
                make_iv(w, root_size, t1, iv);
                aes128_cbc_decrypt(&w->aes, iv, root, tmp, root_size);
                uint8_t stored[20];
                memcpy(stored, tmp + 4, 20);
                memset(tmp + 4, 0xFF, 20);
                sha1_init(&c); sha1_update(&c, tmp, root_size); sha1_final(&c, h);
                if (memcmp(stored, h, 20)) ok = false;
                else memcpy(root, tmp, root_size);
            }
            free(tmp);
        }
    }
    memset(key, 0, sizeof(key));
    if (ok) {
        uint8_t bs = root[0x60 + 21];
        uint32_t root_dir = be32(root + 0x60 + 8);
        if (!enc) ivx = 0;
        w->dev_iv = be32(root + 0x18);
        if ((bs != 12 && bs != 13) || !root_dir ||
            area_add(w, 0, ivx, bs, root_dir) != 0)
            ok = false;
    }
    free(root);
    if (!ok) {
        memset(&a->base, 0, sizeof(a->base));
        memset(&a->src, 0, sizeof(a->src));
        archive_close(a);
        g_archive_err = "Can't read this WFS volume (damaged, or the key doesn't belong to this drive)";
        return NULL;
    }
    return a;
}

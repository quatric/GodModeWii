/*
 * GameCube and Wii disc images, read-only, for browsing:
 *   .iso / .gcm   plain disc image (GameCube, or Wii with encrypted partitions)
 *   .wbfs         a single-game WBFS file (USB loader format; split .wbf1 parts are not joined)
 *   .ciso         compact ISO (block map, 0x8000 byte header)
 *   .rvz / .wia   Dolphin / wit compressed images (rvz.c): Wii partition data is stored decrypted
 *
 * GameCube discs show sys/ and files/. Wii discs show one folder per
 * partition (DATA, UPDATE, CHANNEL, ...) holding ticket.bin, tmd.bin, sys/
 * and files/; partition data is decrypted on the fly with the title key
 * (common key 0, 1 or 2 from the ticket). Nothing is ever written.
 * Disc layout: big endian throughout, Wii file offsets stored >> 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"
#include "aes.h"
#include "rvz.h"

bool arc_common_key(int index, uint8_t key[16]);

#define WII_MAGIC   0x5D1C9EA3u
#define GC_MAGIC    0xC2339F3Du
#define CLUSTER     0x8000u
#define CL_DATA     0x7C00u
#define MAX_PARTS   8
#define FST_MAX     (16u * 1024 * 1024)

enum { C_RAW, C_WBFS, C_CISO, C_RVZ };

typedef struct {
    uint64_t data_off, data_size;   /* raw disc offset / size of the cluster area */
    uint64_t raw_off;               /* partition start (ticket) */
    bool clustered;                 /* Wii: 0x8000 byte clusters with a 0x400 byte hash block */
    bool encrypted;
    int rvz;                        /* RVZ / WIA partition whose stored (decrypted) data this is, or -1 */
    aes128_t key;
} part_t;

typedef struct {
    int kind;
    uint32_t shift;                 /* WBFS sector size (log2) */
    uint16_t *wlba;
    uint32_t nwlba;
    uint32_t bs;                    /* CISO block size */
    uint32_t *cmap;                 /* CISO: block -> stored index, or ~0 */
    uint32_t ncmap;
    rvz_t *rvz;
    uint64_t disc_size;
    part_t part[MAX_PARTS];
    int nparts;
    uint8_t *enc, *dec;             /* one cluster, read / decrypted */
    int cache_part;
    uint64_t cache_cl;
} disc_t;

/* ---------------------------------------------------------------- container: disc offset -> file */

static bool disc_read(const src_t *s, disc_t *d, uint64_t off, uint8_t *buf, uint32_t len)
{
    if (d->kind == C_RVZ) return rvz_read_raw(d->rvz, off, buf, len);
    while (len) {
        uint64_t unit, phys;
        bool hole = false;
        if (d->kind == C_RAW) {
            unit = len;
            phys = off;
        } else if (d->kind == C_WBFS) {
            uint64_t i = off >> d->shift, m = (1ull << d->shift) - 1;
            unit = (1ull << d->shift) - (off & m);
            if (i >= d->nwlba || !d->wlba[i]) { hole = true; phys = 0; }
            else phys = ((uint64_t)d->wlba[i] << d->shift) + (off & m);
        } else {
            uint64_t i = off / d->bs;
            unit = d->bs - off % d->bs;
            if (i >= d->ncmap || d->cmap[i] == 0xFFFFFFFFu) { hole = true; phys = 0; }
            else phys = 0x8000ull + (uint64_t)d->cmap[i] * d->bs + off % d->bs;
        }
        uint32_t n = unit < len ? (uint32_t)unit : len;
        if (hole) memset(buf, 0, n);
        else if (!src_read_full(s, phys, buf, n)) return false;
        buf += n;
        off += n;
        len -= n;
    }
    return true;
}

/* ---------------------------------------------------------------- partition space */

static bool part_read(const src_t *s, disc_t *d, int pi, uint64_t off, uint8_t *buf, uint32_t len)
{
    part_t *p = &d->part[pi];
    if (p->rvz >= 0) return rvz_read_part(d->rvz, p->rvz, off, buf, len);
    if (!p->clustered) return disc_read(s, d, p->data_off + off, buf, len);
    while (len) {
        uint64_t cl = off / CL_DATA;
        uint32_t in = (uint32_t)(off % CL_DATA), n = CL_DATA - in;
        if (n > len) n = len;
        if (d->cache_part != pi || d->cache_cl != cl) {
            if (!disc_read(s, d, p->data_off + cl * CLUSTER, d->enc, CLUSTER)) return false;
            if (p->encrypted) aes128_cbc_decrypt(&p->key, d->enc + 0x3D0, d->enc + 0x400, d->dec, CL_DATA);
            else memcpy(d->dec, d->enc + 0x400, CL_DATA);
            d->cache_part = pi;
            d->cache_cl = cl;
        }
        memcpy(buf, d->dec + in, n);
        buf += n;
        off += n;
        len -= n;
    }
    return true;
}

/* tnode: a = offset, b = partition, c = 0 partition data / 1 raw disc */
static int32_t disc_node_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    disc_t *d = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    bool ok = n->c ? disc_read(&a->src, d, n->a + off, buf, len)
                   : part_read(&a->src, d, (int)n->b, n->a + off, buf, len);
    return ok ? (int32_t)len : -1;
}

static void disc_destroy(archive_t *a)
{
    disc_t *d = a->priv;
    if (!d) return;
    free(d->wlba);
    free(d->cmap);
    free(d->enc);
    free(d->dec);
    rvz_close(d->rvz);
    free(d);
}

/* ---------------------------------------------------------------- probe */

static int container_of(const uint8_t *h, uint64_t size)
{
    if (!memcmp(h, "WBFS", 4)) return C_WBFS;
    if (!memcmp(h, "CISO", 4)) return C_CISO;
    if (rvz_probe(h)) return C_RVZ;
    if (be32(h + 0x18) == WII_MAGIC || be32(h + 0x1C) == GC_MAGIC) return C_RAW;
    (void)size;
    return -1;
}

bool arc_disc_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[0x20];
    if (f->size < 0x440 || !src_read_full(f, 0, h, sizeof(h))) return false;
    return container_of(h, f->size) >= 0;
}

/* ---------------------------------------------------------------- open */

static bool setup_container(const src_t *f, disc_t *d)
{
    uint8_t h[0x20];
    if (!src_read_full(f, 0, h, sizeof(h))) return false;
    d->kind = container_of(h, f->size);
    if (d->kind == C_RAW) { d->disc_size = f->size; return true; }
    if (d->kind == C_RVZ) {
        const char *err;
        d->rvz = rvz_open(f, &err);
        if (!d->rvz) { g_archive_err = err; return false; }
        d->disc_size = rvz_iso_size(d->rvz);
        return true;
    }
    if (d->kind == C_WBFS) {
        uint32_t hd_shift = h[8], wb_shift = h[9];
        if (hd_shift < 9 || hd_shift > 16 || wb_shift < 15 || wb_shift > 30) return false;
        /* Room for a dual layer disc: 143432 * 2 Wii sectors of 0x8000. */
        uint64_t wii_secs = 143432ull * 2;
        d->nwlba = (uint32_t)((wii_secs * CLUSTER) >> wb_shift);
        d->shift = wb_shift;
        d->wlba = malloc(d->nwlba * 2ull);
        if (!d->wlba) return false;
        uint64_t info = 1ull << hd_shift;           /* first disc's info block */
        uint8_t *raw = malloc(d->nwlba * 2ull);
        if (!raw || !src_read_full(f, info + 0x100, raw, d->nwlba * 2)) { free(raw); return false; }
        for (uint32_t i = 0; i < d->nwlba; i++) d->wlba[i] = be16(raw + i * 2);
        free(raw);
        d->disc_size = wii_secs * CLUSTER;
        return true;
    }
    /* CISO: little endian block size, then one presence byte per block */
    d->bs = le32(h + 4);
    if (d->bs < 0x8000 || d->bs > 0x1000000 || (d->bs & (d->bs - 1))) return false;
    d->ncmap = 0x8000 - 8;
    d->cmap = malloc(d->ncmap * 4ull);
    uint8_t *m = malloc(d->ncmap);
    if (!d->cmap || !m || !src_read_full(f, 8, m, d->ncmap)) { free(m); return false; }
    uint32_t k = 0, last = 0;
    for (uint32_t i = 0; i < d->ncmap; i++) {
        d->cmap[i] = m[i] ? k++ : 0xFFFFFFFFu;
        if (m[i]) last = i + 1;
    }
    free(m);
    d->ncmap = last;
    d->disc_size = (uint64_t)last * d->bs;
    return true;
}

static void add_part(disc_t *d, uint64_t data_off, uint64_t data_size, uint64_t raw_off, bool clustered)
{
    part_t *p = &d->part[d->nparts++];
    memset(p, 0, sizeof(*p));
    p->data_off = data_off;
    p->data_size = data_size;
    p->raw_off = raw_off;
    p->clustered = clustered;
    p->rvz = -1;
}

/* sys/: boot.bin, bi2.bin, apploader.img, main.dol and fst.bin, sized from their headers. */
static void add_sys(archive_t *a, disc_t *d, int pi, int parent, int shift)
{
    int sys = tree_add(&a->tree, parent, "sys", true, 0, 0, 0, 0);
    if (sys < 0) return;
    uint8_t boot[0x440], al[0x20], dh[0x100];
    if (!part_read(&a->src, d, pi, 0, boot, sizeof(boot))) return;
    tree_add(&a->tree, sys, "boot.bin", false, 0x440, 0, pi, 0);
    tree_add(&a->tree, sys, "bi2.bin", false, 0x2000, 0x440, pi, 0);
    if (part_read(&a->src, d, pi, 0x2440, al, sizeof(al))) {
        uint64_t sz = 0x20ull + be32(al + 0x14) + be32(al + 0x18);
        if (sz < (64u << 20)) tree_add(&a->tree, sys, "apploader.img", false, sz, 0x2440, pi, 0);
    }
    uint64_t dol = (uint64_t)be32(boot + 0x420) << shift;
    if (dol && part_read(&a->src, d, pi, dol, dh, sizeof(dh))) {
        uint64_t end = 0x100;
        for (int i = 0; i < 18; i++) {
            uint64_t e = (uint64_t)be32(dh + i * 4) + be32(dh + 0x90 + i * 4);
            if (be32(dh + i * 4) && e > end) end = e;
        }
        if (end < (64u << 20)) tree_add(&a->tree, sys, "main.dol", false, end, dol, pi, 0);
    }
    uint64_t fo = (uint64_t)be32(boot + 0x424) << shift, fs = (uint64_t)be32(boot + 0x428) << shift;
    if (fo && fs >= 12 && fs <= FST_MAX) tree_add(&a->tree, sys, "fst.bin", false, fs, fo, pi, 0);
}

/* files/ from the FST: 12 byte entries, then the name table. */
static bool add_files(archive_t *a, disc_t *d, int pi, int parent, int shift)
{
    uint8_t boot[0x440];
    if (!part_read(&a->src, d, pi, 0, boot, sizeof(boot))) return false;
    uint64_t fo = (uint64_t)be32(boot + 0x424) << shift;
    uint32_t fs = (uint32_t)((uint64_t)be32(boot + 0x428) << shift);
    if (!fo || fs < 12 || fs > FST_MAX) return false;
    uint8_t *fst = malloc(fs + 1);
    if (!fst || !part_read(&a->src, d, pi, fo, fst, fs)) { free(fst); return false; }
    fst[fs] = 0;
    uint32_t count = be32(fst + 8);
    if (!count || count > fs / 12) { free(fst); return false; }
    const char *names = (const char *)fst + count * 12;
    uint32_t nlen = fs - count * 12;
    int files = tree_add(&a->tree, parent, "files", true, 0, 0, 0, 0);
    /* stack of open folders: (index after the folder's last entry, tree node) */
    uint32_t end[64];
    int node[64], depth = 0;
    end[0] = count;
    node[0] = files;
    bool ok = files >= 0;
    for (uint32_t i = 1; ok && i < count; i++) {
        while (depth > 0 && i >= end[depth]) depth--;
        const uint8_t *e = fst + i * 12;
        uint32_t no = be32(e) & 0x00FFFFFF;
        char nm[256];
        if (no < nlen) snprintf(nm, sizeof(nm), "%s", names + no);
        else nm[0] = 0;
        tree_clean_name(nm);
        if (!nm[0]) snprintf(nm, sizeof(nm), "entry%u", (unsigned)i);
        if (e[0]) {
            uint32_t next = be32(e + 8);
            int idx = tree_add(&a->tree, node[depth], nm, true, 0, 0, 0, 0);
            if (idx < 0) { ok = false; break; }
            if (next > i + 1 && next <= count && depth < 63) {
                depth++;
                end[depth] = next;
                node[depth] = idx;
            }
        } else {
            uint64_t off = (uint64_t)be32(e + 4) << shift;
            if (tree_add(&a->tree, node[depth], nm, false, be32(e + 8), off, pi, 0) < 0) ok = false;
        }
    }
    free(fst);
    return ok;
}

static bool open_wii(archive_t *a, disc_t *d)
{
    uint8_t pt[0x20];
    if (!disc_read(&a->src, d, 0x40000, pt, sizeof(pt))) return false;
    for (int g = 0; g < 4; g++) {
        uint32_t n = be32(pt + g * 8);
        uint64_t to = (uint64_t)be32(pt + g * 8 + 4) << 2;
        if (!n || n > MAX_PARTS) continue;
        uint8_t ent[MAX_PARTS * 8];
        if (!disc_read(&a->src, d, to, ent, n * 8)) continue;
        for (uint32_t i = 0; i < n && d->nparts < MAX_PARTS; i++) {
            uint64_t po = (uint64_t)be32(ent + i * 8) << 2;
            uint32_t type = be32(ent + i * 8 + 4);
            uint8_t ph[0x2C0];
            if (!disc_read(&a->src, d, po, ph, sizeof(ph))) continue;
            uint64_t doff = po + ((uint64_t)be32(ph + 0x2B8) << 2), dsz = (uint64_t)be32(ph + 0x2BC) << 2;
            int pi = d->nparts;
            add_part(d, doff, dsz, po, true);
            part_t *p = &d->part[pi];
            /* Hash and encryption can be turned off on development discs (header 0x60 / 0x61). */
            uint8_t flags[2];
            p->encrypted = !(disc_read(&a->src, d, 0x60, flags, 2) && flags[1]);
            if (d->kind == C_RVZ) {             /* stored decrypted, without hashes */
                p->encrypted = false;
                for (int k = 0; k < rvz_partitions(d->rvz); k++)
                    if (rvz_part_disc_offset(d->rvz, k) == doff) { p->rvz = k; p->data_size = rvz_part_size(d->rvz, k); }
            } else if (p->encrypted) {
                uint8_t ck[16], iv[16] = { 0 }, tk[16];
                if (!arc_common_key(ph[0x1F1], ck)) { d->nparts--; continue; }
                aes128_t c;
                aes128_init_dec(&c, ck);
                memcpy(iv, ph + 0x1DC, 8);
                aes128_cbc_decrypt(&c, iv, ph + 0x1BF, tk, 16);
                aes128_init_dec(&p->key, tk);
            }
            char pn[24];
            const char *tn = type == 0 ? "DATA" : type == 1 ? "UPDATE" : type == 2 ? "CHANNEL" : NULL;
            if (tn) snprintf(pn, sizeof(pn), "%s", tn);
            else snprintf(pn, sizeof(pn), "P%u-%c%c%c%c", (unsigned)pi, type >> 24 & 0xFF, type >> 16 & 0xFF,
                          type >> 8 & 0xFF, type & 0xFF);
            tree_clean_name(pn);
            if (tree_find_child(&a->tree, 0, pn) >= 0) snprintf(pn + strlen(pn), sizeof(pn) - strlen(pn), "-%d", pi);
            int dir = tree_add(&a->tree, 0, pn, true, 0, 0, 0, 0);
            if (dir < 0) return false;
            tree_add(&a->tree, dir, "ticket.bin", false, 0x2A4, po, pi, 1);
            uint32_t tmd_sz = be32(ph + 0x2A4);
            uint64_t tmd_off = (uint64_t)be32(ph + 0x2A8) << 2;
            if (tmd_sz && tmd_sz < 0x10000) tree_add(&a->tree, dir, "tmd.bin", false, tmd_sz, po + tmd_off, pi, 1);
            uint8_t chk[0x20];
            if (d->kind == C_RVZ && p->rvz < 0) continue;                     /* no data stored for it */
            if (!part_read(&a->src, d, pi, 0, chk, sizeof(chk))) continue;   /* unreadable: show the ticket only */
            add_sys(a, d, pi, dir, 2);
            add_files(a, d, pi, dir, 2);
        }
    }
    return d->nparts > 0;
}

archive_t *arc_disc_open(const src_t *f, const char *name)
{
    (void)name;
    disc_t *d = calloc(1, sizeof(*d));
    archive_t *a = NULL;
    if (!d) goto fail;
    if (!setup_container(f, d)) goto fail;
    d->enc = malloc(CLUSTER);
    d->dec = malloc(CL_DATA);
    d->cache_part = -1;
    if (!d->enc || !d->dec) goto fail;
    a = archive_alloc("disc image");
    if (!a) goto fail;
    a->base = *f;
    a->src = a->base;
    a->read = disc_node_read;
    a->destroy = disc_destroy;
    a->priv = d;
    uint8_t h[0x20];
    if (!disc_read(f, d, 0, h, sizeof(h))) goto fail_a;
    bool ok;
    if (be32(h + 0x18) == WII_MAGIC) {
        ok = open_wii(a, d);
    } else if (be32(h + 0x1C) == GC_MAGIC) {
        add_part(d, 0, d->disc_size, 0, false);
        add_sys(a, d, 0, 0, 0);
        ok = add_files(a, d, 0, 0, 0);
    } else ok = false;
    if (!ok || a->tree.count <= 1) goto fail_a;
    return a;
fail_a:
    memset(&a->base, 0, sizeof(a->base));      /* the caller keeps the file on failure */
    memset(&a->src, 0, sizeof(a->src));
    archive_close(a);
    return NULL;
fail:
    if (d) { free(d->wlba); free(d->cmap); free(d->enc); free(d->dec); rvz_close(d->rvz); free(d); }
    return NULL;
}

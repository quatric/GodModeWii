/*
 * BootMii NAND dump (nand.bin), read-only: the raw 4096 blocks x 64 pages x (2048 + 64 spare) bytes of a
 * Wii's NAND (optionally followed by BootMii's 0x400 byte keys section, or a dump without the spare
 * areas), the newest of the 16 SFFS superblocks, and the cluster chains of its file system. File data is
 * AES-128-CBC encrypted with the console's NAND key (OTP 0x58); the key comes from the keys section of the
 * dump, from a keys.bin / otp.bin next to it, or from the console running this program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"

#define NAND_RAW 0x21000000ull          /* with spare areas */
#define NAND_PLAIN 0x20000000ull        /* data only */
#define CLUSTER 0x4000u
#define SB_FIRST 0x7F00u
#define SB_CLUSTERS 16u
#define SB_SIZE (SB_CLUSTERS * CLUSTER)
#define FAT_OFF 0x0C
#define FST_OFF 0x1000C
#define FST_COUNT 0x17FF

static archive_key_fn nand_key_provider;
void archive_set_nand_key_provider(archive_key_fn fn) { nand_key_provider = fn; }

typedef struct {
    bool spare;                 /* pages are 2112 bytes apart */
    bool have_key;
    aes128_t aes;
    uint8_t *sb;                /* the chosen superblock */
    uint32_t cached;
    uint8_t cl[CLUSTER];
} nandfs;

static uint64_t cluster_pos(const nandfs *n, uint32_t c, uint32_t page) { return ((uint64_t)c * 8 + page) * (n->spare ? 2112 : 2048); }

static bool read_cluster_raw(const src_t *s, const nandfs *n, uint32_t c, uint8_t *out)
{
    for (uint32_t p = 0; p < 8; p++)
        if (!src_read_full(s, cluster_pos(n, c, p), out + p * 2048, 2048)) return false;
    return true;
}

static bool layout(const src_t *f, bool *spare)
{
    uint64_t s = f->size;
    if (s == NAND_RAW || s == NAND_RAW + 0x400) { *spare = true; return true; }
    if (s == NAND_PLAIN || s == NAND_PLAIN + 0x400) { *spare = false; return true; }
    return false;
}

/* the best superblock copy: its index, or -1 */
static int pick_sb(const src_t *f, const nandfs *n, uint32_t *version)
{
    int best = -1;
    uint32_t bv = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t h[16];
        if (!src_read_full(f, cluster_pos(n, SB_FIRST + i * SB_CLUSTERS, 0), h, sizeof(h))) continue;
        if (memcmp(h, "SFFS", 4)) continue;
        uint32_t v = be32(h + 4);
        if (best < 0 || v > bv) { best = (int)i; bv = v; }
    }
    *version = bv;
    return best;
}

bool arc_nand_probe(const src_t *file, const char *name)
{
    (void)name;
    nandfs n = { 0 };
    uint32_t v;
    return layout(file, &n.spare) && pick_sb(file, &n, &v) >= 0;
}

static bool fst_entry(const nandfs *n, uint32_t i, uint8_t *e)
{
    if (i >= FST_COUNT) return false;
    memcpy(e, n->sb + FST_OFF + i * 0x20, 0x20);
    return true;
}

static uint16_t fat_next(const nandfs *n, uint32_t c) { return be16(n->sb + FAT_OFF + c * 2); }

static int32_t nand_read(archive_t *a, const tnode *t, uint64_t off, void *buf, uint32_t len)
{
    nandfs *n = a->priv;
    if (off >= t->size) return 0;
    if (len > t->size - off) len = (uint32_t)(t->size - off);
    if (!n->have_key) { g_archive_err = "The NAND key is needed: put keys.bin (or otp.bin) next to nand.bin"; return -1; }
    uint32_t skip = (uint32_t)(off / CLUSTER), done = 0;
    uint32_t c = (uint32_t)t->a;
    for (uint32_t i = 0; i < skip; i++) {
        c = fat_next(n, c);
        if (c >= 0x8000) return -1;
    }
    uint8_t raw[CLUSTER];
    while (done < len) {
        if (c >= 0x8000) return done ? (int32_t)done : -1;
        if (n->cached != c) {
            if (!read_cluster_raw(&a->src, n, c, raw)) return done ? (int32_t)done : -1;
            uint8_t iv[16] = { 0 };
            aes128_cbc_decrypt(&n->aes, iv, raw, n->cl, CLUSTER);
            n->cached = c;
        }
        uint32_t o = (uint32_t)((off + done) % CLUSTER), take = CLUSTER - o;
        if (take > len - done) take = len - done;
        memcpy((uint8_t *)buf + done, n->cl + o, take);
        done += take;
        if (o + take == CLUSTER && done < len) c = fat_next(n, c);
    }
    return (int32_t)done;
}

static bool nand_expand(archive_t *a, int node)
{
    nandfs *n = a->priv;
    uint8_t e[0x20];
    if (!fst_entry(n, (uint32_t)a->tree.n[node].a, e)) return false;
    uint32_t child = be16(e + 14);
    for (int guard = 0; child != 0xFFFF && guard < FST_COUNT; guard++) {
        uint8_t c[0x20];
        if (!fst_entry(n, child, c)) break;
        char name[16];
        memcpy(name, c, 12);
        name[12] = 0;
        tree_clean_name(name);
        if (!name[0]) snprintf(name, sizeof(name), "entry%u", (unsigned)child);
        int type = c[12] & 3;
        if (type == 1)
            tree_add(&a->tree, node, name, false, be32(c + 18), be16(c + 14), 0, 0);   /* file: first cluster */
        else if (type == 2)
            tree_add(&a->tree, node, name, true, 0, child, 0, 0);
        child = be16(c + 16);
    }
    return true;
}

static void nand_destroy(archive_t *a)
{
    nandfs *n = a->priv;
    if (!n) return;
    memset(&n->aes, 0, sizeof(n->aes));
    free(n->sb);
    free(n);
}

/* the NAND key out of a BootMii keys.bin (OTP at 0x100), a bare OTP dump (0x80), or the keys at the end of nand.bin */
static bool key_from(const src_t *s, uint64_t at, uint8_t key[16])
{
    return s->size >= at + 0x80 && src_read_full(s, at + 0x58, key, 16);
}

archive_t *arc_nand_open(const src_t *file, const char *name)
{
    (void)name;
    nandfs *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->cached = 0xFFFFFFFFu;
    uint32_t version;
    int best;
    if (!layout(file, &n->spare) || (best = pick_sb(file, n, &version)) < 0) { free(n); return NULL; }
    n->sb = malloc(SB_SIZE);
    if (!n->sb) { free(n); return NULL; }
    uint32_t first = SB_FIRST + (uint32_t)best * SB_CLUSTERS;
    for (uint32_t i = 0; i < SB_CLUSTERS; i++)
        if (!read_cluster_raw(file, n, first + i, n->sb + i * CLUSTER)) { g_archive_err = "Can't read the NAND superblock"; free(n->sb); free(n); return NULL; }
    uint8_t key[16];
    uint64_t body = n->spare ? NAND_RAW : NAND_PLAIN;
    if (file->size >= body + 0x400 && key_from(file, body + 0x100, key)) n->have_key = true;
    if (!n->have_key) {
        src_t sib;
        if (archive_open_sibling("keys.bin", &sib)) {
            n->have_key = key_from(&sib, 0x100, key);
            if (sib.close && sib.ctx) sib.close(sib.ctx);
        }
    }
    if (!n->have_key) {
        src_t sib;
        if (archive_open_sibling("otp.bin", &sib)) {
            n->have_key = key_from(&sib, 0, key);
            if (sib.close && sib.ctx) sib.close(sib.ctx);
        }
    }
    if (!n->have_key && nand_key_provider) n->have_key = nand_key_provider(key);
    if (n->have_key) { aes128_init_dec(&n->aes, key); memset(key, 0, sizeof(key)); }
    archive_t *a = archive_alloc("BootMii NAND");
    if (!a) { memset(&n->aes, 0, sizeof(n->aes)); free(n->sb); free(n); return NULL; }
    a->priv = n;
    a->destroy = nand_destroy;
    a->read = nand_read;
    a->expand = nand_expand;
    a->base = *file;
    a->src = a->base;
    a->tree.n[0].a = 0;                                          /* the root entry */
    return a;
}

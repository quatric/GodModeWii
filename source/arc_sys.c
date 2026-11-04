/*
 * HAL Laboratory "sysdolphin" (HSD) .dat archives (Super Smash Bros. Melee, Kirby Air Ride, Pokemon XD, ...),
 * read-only, after Ploaj's HSDLib and nintoolbox's lib-hsd.c: a 0x20 byte header { file size, relocation
 * table offset, relocation count, root count, reference count, version }, the data section, the relocation
 * table, the root / reference table and its string table. The archive shows
 *   roots/<name>.bin    the data each root node points to (up to the next object)
 *   refs/<name>.bin     the same for the external references
 *   textures/tex_<offset>.tpl   every plain GX image (an HSD_Image found through the relocation table), each
 *                       wrapped in a one-image TPL so the image viewer opens it
 * and the whole data section as data.bin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"

#define HSD_BASE 0x20
#define MAX_ITEMS 20000

typedef struct {
    uint8_t *data;
    uint32_t size, reloc_off, n_reloc, n_root, n_ref;
    uint32_t *targets;          /* sorted relocation destinations, to bound each blob */
    uint32_t n_targets;
} hsd;

static bool hsd_header(const uint8_t *d, uint64_t size, uint32_t *reloc_off, uint32_t *n_reloc, uint32_t *n_root, uint32_t *n_ref)
{
    if (size < 0x40 || size > ARC_MAX_DECOMPRESSED) return false;
    uint32_t fsize = be32(d), ro = be32(d + 4) + HSD_BASE, nr = be32(d + 8), nroot = be32(d + 12), nref = be32(d + 16);
    if (fsize != size || ro < HSD_BASE || ro > size || nroot < 1 || nroot > 0x10000 || nref > 0x10000 || nr > 0x400000) return false;
    if ((uint64_t)ro + 4ull * nr + 8ull * ((uint64_t)nroot + nref) > size) return false;
    for (int i = 0x14; i < 0x18; i++)
        if (d[i] && (d[i] < 0x20 || d[i] > 0x7E)) return false;
    *reloc_off = ro; *n_reloc = nr; *n_root = nroot; *n_ref = nref;
    return true;
}

bool arc_hsd_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x40];
    if (file->size < 0x40 || file->size > ARC_MAX_DECOMPRESSED || !src_read_full(file, 0, h, sizeof(h))) return false;
    uint32_t a, b, c, d;
    /* the header alone has to be plausible before the tables (which need the whole file) are checked */
    if (be32(h) != file->size) return false;
    if (!hsd_header(h, file->size, &a, &b, &c, &d)) {
        /* hsd_header looks at the table bounds only, which the header-only read can't see: check them by size */
        uint32_t ro = be32(h + 4) + HSD_BASE, nr = be32(h + 8), nroot = be32(h + 12), nref = be32(h + 16);
        if (ro < HSD_BASE || ro > file->size || nroot < 1 || nroot > 0x10000 || nref > 0x10000 || nr > 0x400000) return false;
        if ((uint64_t)ro + 4ull * nr + 8ull * ((uint64_t)nroot + nref) > file->size) return false;
        for (int i = 0x14; i < 0x18; i++)
            if (h[i] && (h[i] < 0x20 || h[i] > 0x7E)) return false;
    }
    return true;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* bytes from `off` to the next relocation target (or the relocation table) */
static uint32_t blob_size(const hsd *h, uint32_t off)
{
    uint32_t lo = 0, hi = h->n_targets;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (h->targets[mid] <= off) lo = mid + 1; else hi = mid;
    }
    uint32_t end = lo < h->n_targets ? h->targets[lo] : h->reloc_off;
    return end > off ? end - off : 0;
}

static uint32_t gx_size(uint32_t fmt, uint32_t w, uint32_t hh)
{
    uint32_t bw, bh, bpp;
    switch (fmt) {
    case 0: case 14: bw = 8; bh = 8; bpp = 4; break;
    case 1: case 2: bw = 8; bh = 4; bpp = 8; break;
    case 3: case 4: case 5: bw = 4; bh = 4; bpp = 16; break;
    case 6: bw = 4; bh = 4; bpp = 32; break;
    default: return 0;
    }
    return ((w + bw - 1) / bw) * ((hh + bh - 1) / bh) * bw * bh * bpp / 8;
}

/* a TPL around one image: header, table, image header (0x14) and pixels at 0x40 */
#define TPL_HDR 0x40

static int32_t hsd_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    hsd *h = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    uint8_t *out = buf;
    if (n->c & 0x100) {                                          /* a texture: build the TPL header, then the pixels */
        uint8_t tpl[TPL_HDR];
        memset(tpl, 0, sizeof(tpl));
        tpl[0] = 0x00; tpl[1] = 0x20; tpl[2] = 0xAF; tpl[3] = 0x30;
        tpl[7] = 1;                                              /* one image */
        tpl[11] = 0x0C;                                          /* image table at 0x0C */
        tpl[15] = 0x14;                                          /* image header at 0x14 */
        uint32_t w = (uint32_t)(n->b >> 16), hh = (uint32_t)(n->b & 0xFFFF), fmt = n->c & 0xFF;
        tpl[0x14] = (uint8_t)(hh >> 8); tpl[0x15] = (uint8_t)hh;
        tpl[0x16] = (uint8_t)(w >> 8); tpl[0x17] = (uint8_t)w;
        tpl[0x1B] = (uint8_t)fmt;
        tpl[0x1F] = TPL_HDR;                                     /* data offset */
        tpl[0x23] = 1; tpl[0x27] = 1;                            /* wrap: repeat */
        uint32_t done = 0;
        if (off < TPL_HDR) {
            uint32_t take = TPL_HDR - (uint32_t)off;
            if (take > len) take = len;
            memcpy(out, tpl + off, take);
            done = take;
        }
        if (done < len) {
            uint64_t at = n->a + (off + done - TPL_HDR);
            memcpy(out + done, h->data + at, len - done);
        }
        return (int32_t)len;
    }
    memcpy(out, h->data + n->a + off, len);
    return (int32_t)len;
}

static void hsd_destroy(archive_t *a)
{
    hsd *h = a->priv;
    if (!h) return;
    free(h->data);
    free(h->targets);
    free(h);
}

static void add_blob(archive_t *a, int dir, const hsd *h, const char *prefix, uint32_t k, const char *name, uint32_t off)
{
    char nm[96];
    if (off < HSD_BASE || off >= h->reloc_off) return;
    uint32_t sz = blob_size(h, off);
    if (!sz) return;
    snprintf(nm, sizeof(nm), "%.80s.bin", name[0] ? name : prefix);
    tree_clean_name(nm);
    if (tree_find_child(&a->tree, dir, nm) >= 0) snprintf(nm, sizeof(nm), "%.70s_%u.bin", name[0] ? name : prefix, (unsigned)k);
    tree_add(&a->tree, dir, nm, false, sz, off, 0, 0);
}

archive_t *arc_hsd_open(const src_t *file, const char *name)
{
    (void)name;
    if (!arc_hsd_probe(file, name)) return NULL;
    uint8_t *d = malloc((size_t)file->size);
    if (!d || !src_read_full(file, 0, d, (uint32_t)file->size)) { free(d); g_archive_err = "Read error"; return NULL; }
    hsd *h = calloc(1, sizeof(*h));
    if (!h) { free(d); return NULL; }
    h->data = d;
    h->size = (uint32_t)file->size;
    if (!hsd_header(d, h->size, &h->reloc_off, &h->n_reloc, &h->n_root, &h->n_ref)) { free(d); free(h); return NULL; }
    /* relocation destinations, and (for the textures) which locations hold pointers */
    uint32_t *dest = h->n_reloc ? malloc(h->n_reloc * 4) : NULL;
    uint32_t *loc = h->n_reloc ? malloc(h->n_reloc * 4) : NULL;
    uint32_t nrel = 0;
    for (uint32_t i = 0; dest && loc && i < h->n_reloc; i++) {
        uint32_t l = be32(d + h->reloc_off + 4 * i) + HSD_BASE;
        if (l < HSD_BASE || l + 4 > h->reloc_off) continue;
        uint32_t t = be32(d + l) + HSD_BASE;
        if (t < HSD_BASE || t >= h->reloc_off) continue;
        loc[nrel] = l;
        dest[nrel++] = t;
    }
    h->targets = malloc((nrel ? nrel : 1) * 4);
    if (!h->targets) { free(dest); free(loc); free(d); free(h); return NULL; }
    memcpy(h->targets, dest, nrel * 4);
    qsort(h->targets, nrel, 4, cmp_u32);
    uint32_t nt = 0;
    for (uint32_t i = 0; i < nrel; i++)
        if (!nt || h->targets[i] != h->targets[nt - 1]) h->targets[nt++] = h->targets[i];
    h->n_targets = nt;

    archive_t *a = archive_alloc("HSD");
    if (!a) { free(dest); free(loc); free(h->targets); free(d); free(h); return NULL; }
    a->priv = h;
    a->destroy = hsd_destroy;
    a->read = hsd_read;
    a->base = *file;
    a->src = a->base;

    uint32_t table = h->reloc_off + 4 * h->n_reloc, strs = table + 8 * (h->n_root + h->n_ref);
    int roots = tree_add(&a->tree, 0, "roots", true, 0, 0, 0, 0);
    int refs = h->n_ref ? tree_add(&a->tree, 0, "refs", true, 0, 0, 0, 0) : -1;
    for (uint32_t i = 0; i < h->n_root + h->n_ref && i < MAX_ITEMS; i++) {
        uint32_t off = be32(d + table + 8 * i) + HSD_BASE, no = be32(d + table + 8 * i + 4);
        char nm[80] = "";
        if (strs + no < h->size) {
            size_t room = h->size - (strs + no);
            if (room > sizeof(nm) - 1) room = sizeof(nm) - 1;
            memcpy(nm, d + strs + no, room);
            nm[sizeof(nm) - 1] = 0;
        }
        bool is_root = i < h->n_root;
        add_blob(a, is_root ? roots : refs, h, is_root ? "root" : "ref", i, nm, off);
    }
    /* HSD_Image: { pixel pointer, u16 width, u16 height, u32 format, u32 mipmaps, f32, f32 } at a relocated location */
    int texdir = -1, ntex = 0;
    for (uint32_t i = 0; i < nrel && ntex < MAX_ITEMS; i++) {
        uint32_t l = loc[i], pix = dest[i];
        if (l + 0x14 > h->reloc_off) continue;
        uint32_t w = be16(d + l + 4), hh = be16(d + l + 6), fmt = be32(d + l + 8), mips = be32(d + l + 12);
        if (!w || !hh || w > 1024 || hh > 1024 || fmt > 14 || mips > 16) continue;
        uint32_t need = gx_size(fmt, w, hh);
        if (!need || pix + need > h->reloc_off) continue;
        if (texdir < 0) texdir = tree_add(&a->tree, 0, "textures", true, 0, 0, 0, 0);
        char nm[40];
        snprintf(nm, sizeof(nm), "tex_%06X.tpl", (unsigned)pix);
        if (tree_find_child(&a->tree, texdir, nm) >= 0) continue;
        tree_add(&a->tree, texdir, nm, false, TPL_HDR + need, pix, (uint64_t)w << 16 | hh, 0x100 | fmt);
        ntex++;
    }
    tree_add(&a->tree, 0, "data.bin", false, h->reloc_off - HSD_BASE, HSD_BASE, 0, 0);
    free(dest);
    free(loc);
    return a;
}

/*
 * WAD packages, content.bin and data.bin, read-only.
 * Layouts follow Sharpii / libWiiSharp (WAD), DarkMatterCore's wad2bin (content.bin,
 * "Bk" header) and Dolphin's WiiSave (data.bin). Everything that is encrypted is
 * decrypted on the fly while reading, so nothing is ever unpacked to disk first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "archive.h"

enum { K_RAW = 0, K_MEM, K_CBC };

typedef struct {
    aes128_t ctx;
    uint8_t *blob;              /* small decrypted pieces kept in memory */
    uint32_t blob_len;
    uint8_t (*ivs)[16];
    int niv, iv_cap;
} encpriv;

static int32_t enc_read(archive_t *a, const tnode *n, uint64_t off, void *buf, uint32_t len)
{
    encpriv *p = a->priv;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (uint32_t)(n->size - off);
    switch (n->c & 0xFF) {
    case K_MEM:
        memcpy(buf, p->blob + n->a + off, len);
        return (int32_t)len;
    case K_CBC:
        return cbc_stream_read(&a->src, &p->ctx, p->ivs[n->c >> 8], n->a, n->size, off, buf, len);
    default:
        return a->src.read(a->src.ctx, n->a + off, buf, len);
    }
}

static void enc_destroy(archive_t *a)
{
    encpriv *p = a->priv;
    if (!p) return;
    memset(&p->ctx, 0, sizeof(p->ctx));         /* keys don't stay in memory */
    free(p->blob);
    free(p->ivs);
    free(p);
}

static archive_t *enc_new(const char *kind, const src_t *file)
{
    archive_t *a = archive_alloc(kind);
    encpriv *p = calloc(1, sizeof(*p));
    if (!a || !p) { free(p); archive_close(a); return NULL; }
    a->priv = p;
    a->read = enc_read;
    a->destroy = enc_destroy;
    a->base = *file;
    a->src = a->base;
    return a;
}

static int add_iv(encpriv *p, const uint8_t iv[16])
{
    if (p->niv == p->iv_cap) {
        int nc = p->iv_cap ? p->iv_cap * 2 : 16;
        void *n = realloc(p->ivs, (size_t)nc * 16);
        if (!n) return -1;
        p->ivs = n;
        p->iv_cap = nc;
    }
    memcpy(p->ivs[p->niv], iv, 16);
    return p->niv++;
}

/* Detaches the file again when opening fails, so the caller still owns it. */
static archive_t *fail(archive_t *a)
{
    if (a) {
        memset(&a->base, 0, sizeof(a->base));
        memset(&a->src, 0, sizeof(a->src));
        archive_close(a);
    }
    return NULL;
}

static uint64_t align64(uint64_t v) { return (v + 63) & ~(uint64_t)63; }

/* ---------------------------------------------------------------- TMD */

/* Offset of the TMD / ticket body for the signature type. */
static uint32_t sig_body(uint32_t sig_type)
{
    switch (sig_type) {
    case 0x00010000: return 0x240;      /* RSA-4096 */
    case 0x00010001: return 0x140;      /* RSA-2048 */
    case 0x00010002: return 0x80;       /* ECDSA */
    default: return 0;
    }
}

typedef struct {
    uint64_t title_id;
    int count;
    const uint8_t *records;             /* 0x24 bytes each */
} tmd_t;

static bool parse_tmd(const uint8_t *t, uint32_t len, tmd_t *out)
{
    if (len < 8) return false;
    uint32_t b = sig_body(be32(t));
    if (!b || len < b + 0xA4) return false;
    out->title_id = be64(t + b + 0x4C);
    out->count = be16(t + b + 0x9E);
    out->records = t + b + 0xA4;
    if (out->count < 0 || out->count > 512 || (uint64_t)b + 0xA4 + (uint64_t)out->count * 0x24 > len) return false;
    return true;
}

static void content_iv(uint16_t index, uint8_t iv[16])
{
    memset(iv, 0, 16);
    iv[0] = (uint8_t)(index >> 8);
    iv[1] = (uint8_t)index;
}

/* ---------------------------------------------------------------- WAD */

bool arc_wad_probe(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x20];
    if (file->size < 0x40 || !src_read_full(file, 0, h, sizeof(h))) return false;
    if (be32(h) != 0x20 || be16(h + 6) != 0) return false;
    uint16_t type = be16(h + 4);
    return type == 0x4973 || type == 0x6962;        /* "Is" / "ib" */
}

archive_t *arc_wad_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t h[0x20];
    if (!src_read_full(file, 0, h, sizeof(h))) return NULL;
    uint32_t cert_sz = be32(h + 8), tik_sz = be32(h + 0x10), tmd_sz = be32(h + 0x14);
    uint32_t data_sz = be32(h + 0x18), footer_sz = be32(h + 0x1C);
    if (tik_sz < 0x2A4 || tmd_sz < 0x1E4 || tmd_sz > 1024 * 1024 || cert_sz > 1024 * 1024) return NULL;

    uint64_t cert_off = 0x40;
    uint64_t tik_off = cert_off + align64(cert_sz);
    uint64_t tmd_off = tik_off + align64(tik_sz);
    uint64_t data_off = tmd_off + align64(tmd_sz);
    uint64_t foot_off = data_off + align64(data_sz);
    if (data_off > file->size) return NULL;

    uint8_t *tik = malloc(tik_sz), *tmd = malloc(tmd_sz);
    archive_t *a = NULL;
    if (!tik || !tmd || !src_read_full(file, tik_off, tik, tik_sz) || !src_read_full(file, tmd_off, tmd, tmd_sz))
        goto out;
    tmd_t t;
    if (!parse_tmd(tmd, tmd_sz, &t)) goto out;

    a = enc_new("WAD", file);
    if (!a) goto out;
    encpriv *p = a->priv;

    /* title key: ticket key decrypted with the common key, IV = title id */
    bool have_key = false;
    uint32_t tb = sig_body(be32(tik));
    if (tb && tik_sz >= tb + 0xB2) {
        uint8_t ck[16], tk[16], iv[16];
        int idx = tik[tb + 0xB1];
        if (arc_common_key(idx, ck)) {
            aes128_t c;
            aes128_init_dec(&c, ck);
            memset(iv, 0, 16);
            memcpy(iv, tik + tb + 0x9C, 8);
            aes128_cbc_decrypt(&c, iv, tik + tb + 0x7F, tk, 16);
            aes128_init_dec(&p->ctx, tk);
            have_key = true;
            memset(ck, 0, 16); memset(tk, 0, 16); memset(&c, 0, sizeof(c));
        }
    }

    char nm[64];
    snprintf(nm, sizeof(nm), "%016llx.cert", (unsigned long long)t.title_id);
    if (cert_sz && tree_add(&a->tree, 0, nm, false, cert_sz, cert_off, 0, K_RAW) < 0) goto bad;
    snprintf(nm, sizeof(nm), "%016llx.tik", (unsigned long long)t.title_id);
    if (tree_add(&a->tree, 0, nm, false, tik_sz, tik_off, 0, K_RAW) < 0) goto bad;
    snprintf(nm, sizeof(nm), "%016llx.tmd", (unsigned long long)t.title_id);
    if (tree_add(&a->tree, 0, nm, false, tmd_sz, tmd_off, 0, K_RAW) < 0) goto bad;
    if (footer_sz) {
        snprintf(nm, sizeof(nm), "%016llx.footer", (unsigned long long)t.title_id);
        if (tree_add(&a->tree, 0, nm, false, footer_sz, foot_off, 0, K_RAW) < 0) goto bad;
    }

    uint64_t pos = data_off;
    for (int i = 0; i < t.count; i++) {
        const uint8_t *r = t.records + (size_t)i * 0x24;
        uint32_t cid = be32(r);
        uint16_t index = be16(r + 4);
        uint64_t size = be64(r + 8);
        uint64_t enc = align64(size);
        if (pos + enc > file->size + 63) break;             /* truncated package */
        if (have_key) {
            uint8_t iv[16];
            content_iv(index, iv);
            int ivi = add_iv(p, iv);
            if (ivi < 0) goto bad;
            snprintf(nm, sizeof(nm), "%08x.app", (unsigned)cid);
            if (tree_add(&a->tree, 0, nm, false, size, pos, enc, K_CBC | ((uint32_t)ivi << 8)) < 0) goto bad;
        } else {
            snprintf(nm, sizeof(nm), "%08x.app.enc", (unsigned)cid);
            if (tree_add(&a->tree, 0, nm, false, enc, pos, enc, K_RAW) < 0) goto bad;
        }
        pos += enc;
    }
    free(tik);
    free(tmd);
    return a;
bad:
    fail(a);
    a = NULL;
out:
    free(tik);
    free(tmd);
    return a;
}

/* ---------------------------------------------------------------- content.bin */

#define CB_HEADER   0x640u          /* part A, encrypted with the SD key */
#define CB_BK_SIZE  0x80u
#define BK_MAGIC    0x426B0001u

bool arc_contentbin_probe(const src_t *file, const char *name)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    if (strcasecmp(base, "content.bin") || file->size < CB_HEADER + CB_BK_SIZE) return false;
    uint8_t enc[0x90], plain[0x90];
    if (!src_read_full(file, 0, enc, sizeof(enc))) return false;
    aes128_t c;
    aes128_init_dec(&c, ARC_SD_KEY);
    aes128_cbc_decrypt(&c, ARC_SD_IV, enc, plain, sizeof(enc));
    return !memcmp(plain + 0x80, "IMET", 4);
}

archive_t *arc_contentbin_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t *hdr = malloc(CB_HEADER);
    if (!hdr || !src_read_full(file, 0, hdr, CB_HEADER)) { free(hdr); return NULL; }
    aes128_t sd;
    aes128_init_dec(&sd, ARC_SD_KEY);
    aes128_cbc_decrypt(&sd, ARC_SD_IV, hdr, hdr, CB_HEADER);
    uint64_t title_id = be64(hdr);
    uint32_t icon_size = be32(hdr + 8);
    if (icon_size > 8 * 1024 * 1024) { free(hdr); return NULL; }
    uint64_t icon_alloc = align64(icon_size);

    uint64_t c_off = CB_HEADER + icon_alloc;
    uint8_t bk[CB_BK_SIZE];
    if (c_off + CB_BK_SIZE > file->size || !src_read_full(file, c_off, bk, sizeof(bk)) || be32(bk) != 0x70 || be32(bk + 4) != BK_MAGIC) {
        free(hdr);
        return NULL;
    }
    uint32_t tmd_sz = be32(bk + 0x14);
    uint64_t d_off = c_off + CB_BK_SIZE;
    uint64_t e_off = d_off + align64(tmd_sz);
    if (tmd_sz < 0x1E4 || tmd_sz > 1024 * 1024 || e_off > file->size) { free(hdr); return NULL; }
    uint8_t *tmd = malloc(tmd_sz);
    if (!tmd || !src_read_full(file, d_off, tmd, tmd_sz)) { free(hdr); free(tmd); return NULL; }
    tmd_t t;
    if (!parse_tmd(tmd, tmd_sz, &t)) { free(hdr); free(tmd); return NULL; }

    archive_t *a = enc_new("content.bin", file);
    if (!a) { free(hdr); free(tmd); return NULL; }
    encpriv *p = a->priv;

    /* blob = decrypted header (part A) followed by decrypted icon.bin (part B) */
    p->blob = realloc(hdr, CB_HEADER + icon_alloc);
    if (!p->blob) { free(hdr); free(tmd); return fail(a); }
    hdr = NULL;
    p->blob_len = (uint32_t)(CB_HEADER + icon_alloc);
    if (icon_alloc) {
        if (!src_read_full(file, CB_HEADER, p->blob + CB_HEADER, (uint32_t)icon_alloc)) { free(tmd); return fail(a); }
        aes128_cbc_decrypt(&sd, ARC_SD_IV, p->blob + CB_HEADER, p->blob + CB_HEADER, (size_t)icon_alloc);
    }
    memset(&sd, 0, sizeof(sd));

    uint8_t prng[16];
    bool have_key = arc_prng_key(prng);
    if (have_key) { aes128_init_dec(&p->ctx, prng); memset(prng, 0, sizeof(prng)); }

    char nm[64];
    if (tree_add(&a->tree, 0, "header.bin", false, CB_HEADER, 0, 0, K_MEM) < 0) goto bad;
    if (icon_size && tree_add(&a->tree, 0, "icon.bin", false, icon_size, CB_HEADER, 0, K_MEM) < 0) goto bad;
    snprintf(nm, sizeof(nm), "%016llx.tmd", (unsigned long long)title_id);
    if (tree_add(&a->tree, 0, nm, false, tmd_sz, d_off, 0, K_RAW) < 0) goto bad;

    uint64_t pos = e_off;
    for (int i = 0; i < t.count; i++) {
        if (!(bk[0x20 + i / 8] & (1 << (i % 8))))
            continue;                                       /* not included in this backup */
        const uint8_t *r = t.records + (size_t)i * 0x24;
        uint32_t cid = be32(r);
        uint16_t index = be16(r + 4);
        uint64_t size = be64(r + 8);
        uint64_t enc = align64(size);
        if (pos + enc > file->size + 63) break;
        if (have_key) {
            uint8_t iv[16];
            content_iv(index, iv);
            int ivi = add_iv(p, iv);
            if (ivi < 0) goto bad;
            snprintf(nm, sizeof(nm), "%08x.app", (unsigned)cid);
            if (tree_add(&a->tree, 0, nm, false, size, pos, enc, K_CBC | ((uint32_t)ivi << 8)) < 0) goto bad;
        } else {
            snprintf(nm, sizeof(nm), "%08x.app.enc", (unsigned)cid);
            if (tree_add(&a->tree, 0, nm, false, enc, pos, enc, K_RAW) < 0) goto bad;
        }
        pos += enc;
    }
    free(tmd);
    return a;
bad:
    free(tmd);
    return fail(a);
}

/* ---------------------------------------------------------------- data.bin */

#define DB_HEADER  0xF0C0u          /* encrypted header + banner */
#define DB_BK      0x80u
#define DB_CERTS   0x3C0u
#define FILE_MAGIC 0x03ADF17Eu

bool arc_databin_probe(const src_t *file, const char *name)
{
    const char *base = strrchr(name ? name : "", '/');
    base = base ? base + 1 : (name ? name : "");
    if (strcasecmp(base, "data.bin") || file->size < DB_HEADER + DB_BK) return false;
    uint8_t bk[DB_BK];
    if (!src_read_full(file, DB_HEADER, bk, sizeof(bk))) return false;
    return be32(bk) == 0x70 && be32(bk + 4) == BK_MAGIC && be32(bk + 0x10) + DB_CERTS == be32(bk + 0x1C);
}

archive_t *arc_databin_open(const src_t *file, const char *name)
{
    (void)name;
    uint8_t bk[DB_BK];
    if (!src_read_full(file, DB_HEADER, bk, sizeof(bk))) return NULL;
    uint32_t nfiles = be32(bk + 0x0C);
    if (nfiles > 20000) return NULL;

    archive_t *a = enc_new("data.bin", file);
    if (!a) return NULL;
    encpriv *p = a->priv;
    aes128_init_dec(&p->ctx, ARC_SD_KEY);

    /* banner.bin: the decrypted banner out of the header (tid 8, size 4, perms 1, ?, md5 16, ? 2 = 0x20) */
    uint8_t *hdr = malloc(DB_HEADER);
    if (!hdr || !src_read_full(file, 0, hdr, DB_HEADER)) { free(hdr); return fail(a); }
    aes128_cbc_decrypt(&p->ctx, ARC_SD_IV, hdr, hdr, DB_HEADER);
    uint32_t banner = be32(hdr + 8);
    if (banner < 0x72A0 || banner > 0xF0A0) { free(hdr); return fail(a); }
    memmove(hdr, hdr + 0x20, banner);
    p->blob = realloc(hdr, banner);
    if (!p->blob) { free(hdr); return fail(a); }
    p->blob_len = banner;
    if (tree_add(&a->tree, 0, "banner.bin", false, banner, 0, 0, K_MEM) < 0) return fail(a);

    uint64_t pos = DB_HEADER + DB_BK;
    for (uint32_t i = 0; i < nfiles; i++) {
        uint8_t fh[0x80];
        if (pos + sizeof(fh) > file->size || !src_read_full(file, pos, fh, sizeof(fh)) || be32(fh) != FILE_MAGIC)
            return fail(a);
        uint32_t size = be32(fh + 4);
        uint8_t type = fh[10];
        char path[0x41];
        memcpy(path, fh + 11, 0x40);
        path[0x40] = '\0';
        pos += sizeof(fh);
        if (type == 2) {
            if (tree_add_path(&a->tree, path, true, 0, 0, 0, 0) < 0) return fail(a);
        } else if (type == 1) {
            uint64_t enc = align64(size);
            if (pos + enc > file->size) return fail(a);
            int ivi = add_iv(p, fh + 0x50);
            if (ivi < 0) return fail(a);
            if (tree_add_path(&a->tree, path, false, size, pos, enc, K_CBC | ((uint32_t)ivi << 8)) < 0) return fail(a);
            pos += enc;
        } else {
            return fail(a);
        }
    }
    return a;
}

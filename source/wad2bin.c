/*
 * Installable WAD -> SD card backup package ("wad2bin"), so a channel runs from the SD card menu
 * (System Menu 4.0 and later) and DLC loads from the card, without its contents taking NAND space.
 * Output is what DarkMatterCore's wad2bin writes (its layout, read from its documentation and
 * sources, is what this follows; the code is GodModeWii's own):
 *
 *   channels  private/wii/title/<ABCD>/content.bin
 *     Part A  0x640 bytes, AES-CBC with the SD key / IV: title ID, icon.bin size, MD5 of this header
 *             (taken with the MD5 "blanker" in its place), MD5 of the padded icon.bin, three fixed
 *             title references, then the banner's IMET header without its hash
 *     Part B  the banner's meta/icon.bin, padded to 64 bytes, AES-CBC with the SD key / IV
 *     Part C  the "Bk" header (0x80 bytes): console ID, TMD size, content size, a bit per content
 *     Part D  the TMD, padded to 64 bytes
 *     Part E  every content, padded to 64 bytes, AES-CBC with this console's PRNG key, IV = index
 *     Part F  ECDSA signature of SHA-1(C..E) by an AP key, this console's device certificate, and an
 *             AP certificate (issued by the device, named AP0000000100000002) signed with the
 *             console's own NG key from the OTP
 *   DLC       private/wii/data/<abcd>/<index>.bin for each content in the WAD: "Bk" header
 *             (holding the parent title ID), the TMD, the content under the PRNG key or a null key
 *
 * Both signatures are checked again before the file is kept, and every content is checked against
 * the TMD's SHA-1 as it goes, so nothing half-right is left on the card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aes.h"
#include "archive.h"
#include "ecc233.h"
#include "md5.h"
#include "sha1.h"
#include "wad2bin.h"

#define CHUNK 0x10000u

static const uint8_t MD5_BLANKER[16] = { 0x0E, 0x65, 0x37, 0x81, 0x99, 0xBE, 0x45, 0x17,
                                         0xAB, 0x06, 0xEC, 0x22, 0x45, 0x1A, 0x57, 0x93 };

static uint32_t rb32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rb16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t rb64(const uint8_t *p) { return (uint64_t)rb32(p) << 32 | rb32(p + 4); }
static void wb32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void wb16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void wb64(uint8_t *p, uint64_t v) { wb32(p, (uint32_t)(v >> 32)), wb32(p + 4, (uint32_t)v); }
static uint64_t a64(uint64_t v) { return (v + 63) & ~(uint64_t)63; }

/* signed blob: where the body starts after the signature */
static uint32_t sig_len(uint32_t type)
{
    switch (type) {
    case 0x00010000: return 0x240;      /* RSA-4096 */
    case 0x00010001: return 0x140;      /* RSA-2048 */
    case 0x00010002: return 0x80;       /* ECDSA */
    default: return 0;
    }
}

/* the signature (after its 4-byte type) is all zero: a homebrew blob */
static bool zero_sig(const uint8_t *blob, uint32_t body)
{
    for (uint32_t i = 4; i < body - 0x3C; i++)
        if (blob[i]) return false;
    return true;
}

/* Try values in the 2 bytes at `field` until SHA-1(body) starts with 00 (the "signing bug"). */
static void fakesign(const uint8_t *body, uint32_t n, uint8_t *field)
{
    for (uint32_t v = 0; v < 0xFFFF; v++) {
        uint8_t sum[20];
        sha1_t c;
        field[0] = (uint8_t)(v >> 8), field[1] = (uint8_t)v;
        sha1_init(&c);
        sha1_update(&c, body, n);
        sha1_final(&c, sum);
        if (!sum[0]) return;
    }
}

static void err(w2b_result_t *r, const char *m) { snprintf(r->err, sizeof(r->err), "%s", m); }

void w2b_tid_ascii(uint64_t tid, char out[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(uint8_t)(tid >> (24 - 8 * i));
        out[i] = (c < 0x20 || c > 0x7E) ? '.' : c;
    }
    out[4] = 0;
}

uint64_t w2b_parent_tid(uint64_t dlc_tid)
{
    uint32_t lo = (uint32_t)dlc_tid;
    uint8_t c = (uint8_t)(lo >> 24);
    if (c >= 'a' && c <= 'z') c -= 32;                  /* sZAE -> SZAE */
    return (uint64_t)0x00010000 << 32 | ((uint32_t)c << 24) | (lo & 0xFFFFFF);
}

/* ---------------------------------------------------------------- the WAD */

typedef struct {
    const src_t *f;
    uint32_t cert_sz, tik_sz, tmd_sz;
    uint64_t cert_off, tik_off, tmd_off, data_off;
    uint8_t *cert, *tik, *tmd;
    uint32_t tmd_body;                  /* where the TMD's fields start */
    uint16_t ncont;
    uint64_t tid;
    aes128_t title_key;
} wad_t;

static void wad_free(wad_t *w)
{
    free(w->cert);
    free(w->tik);
    free(w->tmd);
    memset(&w->title_key, 0, sizeof(w->title_key));
}

static uint8_t *rd(const src_t *f, uint64_t off, uint32_t n)
{
    uint8_t *p = malloc(n ? n : 1);
    if (p && !src_read_full(f, off, p, n)) {
        free(p);
        p = NULL;
    }
    return p;
}

static bool wad_open(wad_t *w, const src_t *f, w2b_result_t *r)
{
    memset(w, 0, sizeof(*w));
    w->f = f;
    uint8_t h[0x20];
    if (f->size < 0x40 || !src_read_full(f, 0, h, sizeof(h)) || rb32(h) != 0x20 || rb16(h + 4) != 0x4973) {
        err(r, "Not an installable WAD");
        return false;
    }
    w->cert_sz = rb32(h + 8);
    uint32_t crl = rb32(h + 0xC);
    w->tik_sz = rb32(h + 0x10);
    w->tmd_sz = rb32(h + 0x14);
    if (w->cert_sz > 0x10000 || crl > 0x10000 || w->tik_sz < 0x2A4 || w->tik_sz > 0x10000 || w->tmd_sz < 0x1E4 ||
        w->tmd_sz > 0x40000) {
        err(r, "The WAD's header is damaged");
        return false;
    }
    w->cert_off = 0x40;
    w->tik_off = w->cert_off + a64(w->cert_sz) + a64(crl);
    w->tmd_off = w->tik_off + a64(w->tik_sz);
    w->data_off = w->tmd_off + a64(w->tmd_sz);
    w->cert = rd(f, w->cert_off, w->cert_sz);
    w->tik = rd(f, w->tik_off, w->tik_sz);
    w->tmd = rd(f, w->tmd_off, w->tmd_sz);
    if (!w->cert || !w->tik || !w->tmd) {
        err(r, "Couldn't read the WAD");
        return false;
    }
    uint32_t tb = sig_len(rb32(w->tik)), mb = sig_len(rb32(w->tmd));
    if (!tb || !mb || w->tik_sz < tb + 0xB2 || w->tmd_sz < mb + 0xA4) {
        err(r, "The WAD's ticket or TMD is damaged");
        return false;
    }
    w->tmd_body = mb;
    w->ncont = rb16(w->tmd + mb + 0x9E);
    w->tid = rb64(w->tmd + mb + 0x4C);
    if (mb + 0xA4 + (uint32_t)w->ncont * 0x24 > w->tmd_sz || !w->ncont) {
        err(r, "The WAD's TMD is damaged");
        return false;
    }
    /* A homebrew TMD (signature zeroed) is fakesigned again, as wad2bin does it: the first value of the
     * two unused bytes before the content records that makes its SHA-1 start with 00. Signed TMDs stay. */
    if (zero_sig(w->tmd, mb)) fakesign(w->tmd + mb, w->tmd_sz - mb, w->tmd + mb + 0xA2);
    if (rb64(w->tik + tb + 0x9C) != w->tid) {
        err(r, "The ticket and the TMD are for different titles");
        return false;
    }
    uint8_t ck[16], tk[16], iv[16] = { 0 };
    int idx = w->tik[tb + 0xB1];
    if (!arc_common_key(idx, ck)) {
        err(r, "The ticket uses an unknown common key");
        return false;
    }
    aes128_t c;
    aes128_init_dec(&c, ck);
    memcpy(iv, w->tik + tb + 0x9C, 8);
    aes128_cbc_decrypt(&c, iv, w->tik + tb + 0x7F, tk, 16);
    aes128_init_dec(&w->title_key, tk);
    memset(ck, 0, 16), memset(tk, 0, 16), memset(&c, 0, sizeof(c));
    return true;
}

static const uint8_t *rec(const wad_t *w, int i) { return w->tmd + w->tmd_body + 0xA4 + (size_t)i * 0x24; }

static void content_iv(const uint8_t *record, uint8_t iv[16])
{
    memset(iv, 0, 16);
    iv[0] = record[4];
    iv[1] = record[5];
}

bool w2b_peek(const src_t *wad, uint64_t *tid)
{
    wad_t w;
    w2b_result_t r;
    bool ok = wad_open(&w, wad, &r);
    if (ok) *tid = w.tid;
    wad_free(&w);
    return ok;
}

/* ---------------------------------------------------------------- output */

typedef struct {
    const w2b_io_t *io;
    void *fh;
    char path[320];
    sha1_t *hash;                       /* the signed area, as it is written */
    bool ok;
} out_t;

static bool o_open(out_t *o, const w2b_io_t *io, const char *path, sha1_t *hash)
{
    memset(o, 0, sizeof(*o));
    o->io = io;
    snprintf(o->path, sizeof(o->path), "%s", path);
    o->hash = hash;
    o->fh = io->open(io->ctx, path);
    o->ok = o->fh != NULL;
    return o->ok;
}
static void o_put(out_t *o, const void *p, uint32_t n, bool hashed)
{
    if (!o->ok || !n) return;
    if (hashed && o->hash) sha1_update(o->hash, p, n);
    if (!o->io->write(o->io->ctx, o->fh, p, n)) o->ok = false;
}
static bool o_close(out_t *o, bool keep)                /* removes the file unless keep and all went well */
{
    bool ok = o->fh && o->io->close(o->io->ctx, o->fh) && o->ok && keep;
    if (o->fh && !ok) o->io->remove(o->io->ctx, o->path);
    o->fh = NULL;
    return ok;
}

/* One content: decrypted with the title key, checked against the TMD's SHA-1, zero padded to 64 bytes
 * and encrypted with `key` (IV = its index) into `o`. *match is false when the data at `off` isn't
 * this content (a DLC WAD that leaves some out). */
static bool copy_content(const wad_t *w, int i, uint64_t off, const uint8_t key[16], out_t *o, uint8_t *buf,
                         bool *match, const w2b_io_t *io, uint64_t *done, uint64_t total, w2b_result_t *r)
{
    const uint8_t *rc = rec(w, i);
    uint64_t size = rb64(rc + 8), padded = a64(size);
    uint8_t div[16], eiv[16];
    content_iv(rc, div);
    content_iv(rc, eiv);
    aes128_enc_t enc;
    aes128_init_enc(&enc, key);
    sha1_t check;
    sha1_init(&check);
    *match = false;
    for (uint64_t pos = 0; pos < padded; pos += CHUNK) {
        uint32_t n = (uint32_t)(padded - pos < CHUNK ? padded - pos : CHUNK);
        if (!src_read_full(w->f, off + pos, buf, n)) {
            err(r, "Couldn't read the WAD (is it complete?)");
            return false;
        }
        uint8_t next[16];
        memcpy(next, buf + n - 16, 16);
        aes128_cbc_decrypt(&w->title_key, div, buf, buf, n);
        memcpy(div, next, 16);
        uint32_t real = (uint32_t)(size - pos < n ? size - pos : n);
        sha1_update(&check, buf, real);
        memset(buf + real, 0, n - real);
        aes128_cbc_encrypt(&enc, eiv, buf, buf, n);
        o_put(o, buf, n, true);
        *done += n;
        if (io->progress && !io->progress(io->ctx, "Converting", *done, total)) {
            err(r, "Canceled");
            return false;
        }
        if (!o->ok) {
            err(r, "Couldn't write to the SD card (is it full?)");
            return false;
        }
    }
    uint8_t got[20];
    sha1_final(&check, got);
    *match = !memcmp(got, rc + 0x10, 20);
    return true;
}

/* The banner (content index 0), decrypted, for its IMET header and icon.bin. */
static uint8_t *load_banner(const wad_t *w, uint32_t *len, w2b_result_t *r)
{
    uint64_t off = w->data_off;
    for (int i = 0; i < w->ncont; i++) {
        const uint8_t *rc = rec(w, i);
        uint64_t size = rb64(rc + 8);
        if (rb16(rc + 4) == 0) {
            if (size < 0x640 + 0x20 || size > 16u * 1024 * 1024) break;
            uint8_t *b = rd(w->f, off, (uint32_t)a64(size));
            if (!b) break;
            uint8_t iv[16], sum[20];
            content_iv(rc, iv);
            aes128_cbc_decrypt(&w->title_key, iv, b, b, (size_t)a64(size));
            sha1_t c;
            sha1_init(&c);
            sha1_update(&c, b, (size_t)size);
            sha1_final(&c, sum);
            if (memcmp(sum, rc + 0x10, 20)) {
                free(b);
                err(r, "The banner (content 0) doesn't match its TMD hash: the WAD is damaged");
                return NULL;
            }
            *len = (uint32_t)size;
            return b;
        }
        off += a64(size);
    }
    err(r, "This WAD has no banner (content 0), so it can't run from the SD card");
    return NULL;
}

/* meta/icon.bin inside the banner's U8 archive (at 0x640) */
static const uint8_t *find_icon(const uint8_t *b, uint32_t n, uint32_t *len)
{
    const uint32_t base = 0x640;
    if (n < base + 0x20 || rb32(b + base) != 0x55AA382D) return NULL;
    const uint8_t *u = b + base;
    uint32_t root = rb32(u + 4);
    if (base + root + 12 > n) return NULL;
    uint32_t count = rb32(u + root + 8);
    uint64_t strs = (uint64_t)root + (uint64_t)count * 12;
    if (!count || base + strs > n) return NULL;
    /* walk the tree: find directory "meta", then "icon.bin" inside it */
    uint32_t meta_end = 0;
    for (uint32_t i = 1; i < count; i++) {
        const uint8_t *nd = u + root + i * 12;
        uint32_t name_off = rb32(nd) & 0xFFFFFF;
        if (base + strs + name_off >= n) return NULL;
        const char *nm = (const char *)u + strs + name_off;
        bool dir = nd[0] == 1;
        if (dir && !strcmp(nm, "meta") && rb32(nd + 4) == 0) meta_end = rb32(nd + 8);
        else if (!dir && meta_end && i < meta_end && !strcmp(nm, "icon.bin")) {
            uint32_t o = rb32(nd + 4), l = rb32(nd + 8);
            if ((uint64_t)base + o + l > n) return NULL;
            *len = l;
            return u + o;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- channels: content.bin */

static int make_content_bin(const wad_t *w, const char *root, const w2b_keys_t *k, const w2b_io_t *io,
                            w2b_result_t *r)
{
    uint32_t blen = 0, ilen = 0;
    uint8_t *banner = load_banner(w, &blen, r);
    if (!banner) return -1;
    const uint8_t *icon = find_icon(banner, blen, &ilen);
    const uint8_t *imet = banner + 0x40;
    if (rb32(imet + 0x40) != 0x494D4554 || rb32(imet + 0x44) != 0x600 || !icon || ilen <= 0x20 ||
        rb32(icon) != 0x494D4435) {
        free(banner);
        err(r, "The banner has no IMET header or no meta/icon.bin");
        return -1;
    }

    /* Part A */
    uint8_t a[0x640];
    memcpy(a, banner, sizeof(a));
    memset(a + 0x40 + 0x5F0, 0, 16);                     /* the IMET hash isn't kept */
    wb64(a, w->tid);
    wb32(a + 8, ilen);
    memcpy(a + 0x0C, MD5_BLANKER, 16);
    uint32_t ipad = (uint32_t)a64(ilen);
    uint8_t *b = calloc(1, ipad);
    if (!b) {
        free(banner);
        err(r, "Out of memory");
        return -1;
    }
    memcpy(b, icon, ilen);
    free(banner);
    md5(b, ipad, a + 0x1C);
    wb32(a + 0x2C, 0x57444645);                          /* the three references every wad2bin file has */
    wb64(a + 0x30, 0x0001000157424D45ull);
    wb64(a + 0x38, 0x000100014E414A4Eull);
    uint8_t hh[16];
    md5(a, sizeof(a), hh);
    memcpy(a + 0x0C, hh, 16);

    aes128_enc_t sd;
    uint8_t iv[16];
    aes128_init_enc(&sd, ARC_SD_KEY);
    memcpy(iv, ARC_SD_IV, 16);
    aes128_cbc_encrypt(&sd, iv, a, a, sizeof(a));
    memcpy(iv, ARC_SD_IV, 16);
    aes128_cbc_encrypt(&sd, iv, b, b, ipad);

    /* Part C */
    uint64_t data = 0;
    uint8_t bk[0x80] = { 0 };
    for (int i = 0; i < w->ncont; i++) {
        data += a64(rb64(rec(w, i) + 8));
        if (i < 0x200) bk[0x20 + i / 8] |= (uint8_t)(1u << (i & 7));
    }
    uint32_t tmdpad = (uint32_t)a64(w->tmd_sz);
    wb32(bk, 0x70);
    wb16(bk + 4, 0x426B);
    wb16(bk + 6, 1);
    wb32(bk + 8, k->console_id);
    wb32(bk + 0x14, w->tmd_sz);
    wb32(bk + 0x18, (uint32_t)data);
    wb32(bk + 0x1C, (uint32_t)(0x80 + tmdpad + data + 0x340));

    char tdir[256], path[300], tid4[5];
    w2b_tid_ascii(w->tid, tid4);
    snprintf(tdir, sizeof(tdir), "%sprivate/wii/title/%s", root, tid4);
    snprintf(path, sizeof(path), "%s/content.bin", tdir);
    if (!io->mkdirs(io->ctx, tdir)) {
        free(b);
        err(r, "Couldn't create the folder on the SD card");
        return -1;
    }
    sha1_t signed_area;
    sha1_init(&signed_area);
    out_t o;
    if (!o_open(&o, io, path, &signed_area)) {
        free(b);
        err(r, "Couldn't create content.bin on the SD card");
        return -1;
    }
    o_put(&o, a, sizeof(a), false);
    o_put(&o, b, ipad, false);
    free(b);
    o_put(&o, bk, sizeof(bk), true);
    uint8_t *tp = calloc(1, tmdpad);
    if (!tp) {
        o_close(&o, false);
        err(r, "Out of memory");
        return -1;
    }
    memcpy(tp, w->tmd, w->tmd_sz);
    o_put(&o, tp, tmdpad, true);
    free(tp);

    /* Part E */
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        o_close(&o, false);
        err(r, "Out of memory");
        return -1;
    }
    uint64_t off = w->data_off, done = 0;
    for (int i = 0; i < w->ncont; i++) {
        bool match;
        if (!copy_content(w, i, off, k->prng_key, &o, buf, &match, io, &done, data, r)) {
            free(buf);
            o_close(&o, false);
            return -1;
        }
        if (!match) {
            free(buf);
            o_close(&o, false);
            snprintf(r->err, sizeof(r->err), "Content %d doesn't match its TMD hash: the WAD is damaged", i);
            return -1;
        }
        off += a64(rb64(rec(w, i) + 8));
    }
    free(buf);

    /* Part F */
    uint8_t digest[20], f[0x340] = { 0 };
    sha1_final(&signed_area, digest);
    uint8_t *dev = f + 0x40, *ap = f + 0x1C0;
    memcpy(dev, k->device_cert, 0x180);
    uint8_t ap_priv[30], seed[20];
    {
        sha1_t c;                                         /* an AP key of its own, made from the NG key */
        sha1_init(&c);
        sha1_update(&c, "GodModeWii AP key", 17);
        sha1_update(&c, k->ng_priv, 30);
        sha1_final(&c, seed);
        memset(ap_priv, 0, sizeof(ap_priv));
        memcpy(ap_priv + 10, seed, 20);
        ap_priv[10] &= 0x7F;
    }
    wb32(ap, 0x00010002);
    snprintf((char *)ap + 0x80, 0x40, "%.26s-%.10s", (const char *)dev + 0x80, (const char *)dev + 0xC4);
    wb32(ap + 0xC0, 2);
    snprintf((char *)ap + 0xC4, 0x40, "AP%08x%08x", 1u, 2u);
    ecc233_public_key(ap_priv, ap + 0x108);
    uint8_t apdig[20];
    {
        sha1_t c;
        sha1_init(&c);
        sha1_update(&c, ap + 0x80, 0x100);
        sha1_final(&c, apdig);
    }
    ecc233_sign(k->ng_priv, apdig, ap + 4, ap + 4 + 30);
    ecc233_sign(ap_priv, digest, f + 2, f + 0x22);       /* r and s, each in 32 bytes */
    memset(ap_priv, 0, sizeof(ap_priv));
    memset(seed, 0, sizeof(seed));
    if (!ecc233_verify(dev + 0x108, apdig, ap + 4, ap + 34) || !ecc233_verify(ap + 0x108, digest, f + 2, f + 0x22)) {
        o_close(&o, false);
        err(r, "The console's key doesn't match its device certificate: content.bin not written");
        return -1;
    }
    o_put(&o, f, sizeof(f), false);
    if (!o_close(&o, true)) {
        err(r, "Couldn't write content.bin (is the SD card full?)");
        return -1;
    }
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->files = 1;
    return 0;
}

/* ---------------------------------------------------------------- DLC: <index>.bin */

static int make_dlc_bins(const wad_t *w, const char *root, const w2b_keys_t *k, const w2b_opts_t *opt,
                         const w2b_io_t *io, w2b_result_t *r)
{
    static const uint8_t null_key[16];
    uint64_t parent = opt && opt->parent_tid ? opt->parent_tid : w2b_parent_tid(w->tid);
    char dir[256], tid4[5];
    w2b_tid_ascii(w->tid, tid4);                          /* the folder is the DLC's own; the parent goes in the header */
    snprintf(dir, sizeof(dir), "%sprivate/wii/data/%s", root, tid4);
    if (!io->mkdirs(io->ctx, dir)) {
        err(r, "Couldn't create the folder on the SD card");
        return -1;
    }
    uint32_t tmdpad = (uint32_t)a64(w->tmd_sz);
    uint8_t *tp = calloc(1, tmdpad), *buf = malloc(CHUNK);
    if (!tp || !buf) {
        free(tp), free(buf);
        err(r, "Out of memory");
        return -1;
    }
    memcpy(tp, w->tmd, w->tmd_sz);
    uint64_t off = w->data_off, end = w->f->size, done = 0;
    int files = 0;
    for (int i = 0; i < w->ncont && off < end; i++) {
        const uint8_t *rc = rec(w, i);
        uint64_t size = rb64(rc + 8);
        uint16_t index = rb16(rc + 4);
        if (end - off < size) continue;                   /* not in this WAD */
        uint8_t bk[0x80] = { 0 };
        wb32(bk, 0x70);
        wb16(bk + 4, 0x426B);
        wb16(bk + 6, 1);
        wb32(bk + 8, k->console_id);
        wb32(bk + 0x14, w->tmd_sz);
        wb32(bk + 0x18, (uint32_t)a64(size));
        wb32(bk + 0x1C, (uint32_t)(0x80 + tmdpad + a64(size)));
        if (index < 0x200) bk[0x20 + index / 8] |= (uint8_t)(1u << (index & 7));
        wb64(bk + 0x60, parent);
        char path[300];
        snprintf(path, sizeof(path), "%s/%03u.bin", dir, (unsigned)index);
        out_t o;
        if (!o_open(&o, io, path, NULL)) {
            free(tp), free(buf);
            err(r, "Couldn't create a file on the SD card");
            return -1;
        }
        o_put(&o, bk, sizeof(bk), false);
        o_put(&o, tp, tmdpad, false);
        bool match;
        if (!copy_content(w, i, off, opt && opt->dlc_null_key ? null_key : k->prng_key, &o, buf, &match, io, &done,
                          end - w->data_off, r)) {
            o_close(&o, false);
            free(tp), free(buf);
            return -1;
        }
        if (!match) {                                     /* that was another content: try the next record here */
            o_close(&o, false);
            continue;
        }
        if (!o_close(&o, true)) {
            free(tp), free(buf);
            err(r, "Couldn't write to the SD card (is it full?)");
            return -1;
        }
        files++;
        off += a64(size);
    }
    free(tp), free(buf);
    if (!files) {
        err(r, "None of the DLC's contents are in this WAD");
        return -1;
    }
    snprintf(r->path, sizeof(r->path), "%s", dir);
    r->files = files;
    return 0;
}

/* ---------------------------------------------------------------- */

/* Ticket and TMD only, as wad2bin's "<title id>_bogus.wad": what a WAD manager needs to put them on
 * the NAND (the contents stay on the SD card). */
static void make_bogus(const wad_t *w, const char *root, uint32_t console_id, const w2b_io_t *io, w2b_result_t *r)
{
    char dir[256], path[300];
    snprintf(dir, sizeof(dir), "%swad", root);
    snprintf(path, sizeof(path), "%s/%016llx_bogus.wad", dir, (unsigned long long)w->tid);
    if (!io->mkdirs(io->ctx, dir)) return;
    uint8_t h[0x40] = { 0 };
    wb32(h, 0x20);
    wb16(h + 4, 0x4973);
    wb32(h + 8, w->cert_sz);
    wb32(h + 0x10, w->tik_sz);
    wb32(h + 0x14, w->tmd_sz);
    out_t o;
    if (!o_open(&o, io, path, NULL)) return;
    o_put(&o, h, sizeof(h), false);
    /* the ticket, fakesigned when it is homebrew or was issued to another console (as wad2bin does) */
    uint8_t *tik = malloc(w->tik_sz);
    if (!tik) {
        o_close(&o, false);
        return;
    }
    memcpy(tik, w->tik, w->tik_sz);
    uint32_t tb = sig_len(rb32(tik)), owner = rb32(tik + tb + 0x98);
    if (zero_sig(tik, tb) || (owner && owner != console_id)) {
        memset(tik + 4, 0, tb - 0x3C - 4);
        memset(tik + tb + 0x40, 0, 0x3C);                 /* ECDH data */
        wb32(tik + tb + 0x98, 0);                         /* console ID */
        fakesign(tik + tb, w->tik_sz - tb, tik + tb + 0x122);
    }
    const uint8_t *parts[3] = { w->cert, tik, w->tmd };
    uint32_t sizes[3] = { w->cert_sz, w->tik_sz, w->tmd_sz };
    static const uint8_t zeros[64];
    for (int i = 0; i < 3; i++) {
        o_put(&o, parts[i], sizes[i], false);
        o_put(&o, zeros, (uint32_t)(a64(sizes[i]) - sizes[i]), false);
    }
    free(tik);
    if (o_close(&o, true)) snprintf(r->bogus, sizeof(r->bogus), "%s", path);
}

int w2b_convert(const src_t *wad, const char *root, const w2b_keys_t *keys, const w2b_opts_t *opt,
                const w2b_io_t *io, w2b_result_t *r)
{
    memset(r, 0, sizeof(*r));
    wad_t w;
    if (!wad_open(&w, wad, r)) {
        wad_free(&w);
        return -1;
    }
    r->tid = w.tid;
    r->dlc = (w.tid >> 32) == 0x00010005;
    int ret = r->dlc ? make_dlc_bins(&w, root, keys, opt, io, r) : make_content_bin(&w, root, keys, io, r);
    if (!ret) make_bogus(&w, root, keys->console_id, io, r);
    wad_free(&w);
    return ret;
}

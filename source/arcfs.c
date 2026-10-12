/*
 * Glue between the archive readers and the VFS: opens the container file through
 * the VFS (so archives work on SD, USB, NAND, the disc, ... and inside other
 * archives), keeps a few parsed archives around while browsing them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <strings.h>

#include "arcfs.h"
#include "archive.h"
#include "otp.h"

#define KEY_MAX 512
#define CACHE_N 4
#define BOUNCE 0x4000

typedef struct entry entry;
struct entry {
    char key[KEY_MAX];
    archive_t *a;
    int refs;
    u32 stamp;
};

static entry cache[CACHE_N];
static entry pins[USB_SLOTS];       /* USB drives read through the WFS reader, one archive per drive */
static u32 clock_tick;
static bool provider_set;

/* ---------------------------------------------------------------- sources */

typedef struct {
    vfs_file f;
    u64 pos;
    u8 *bounce;
} filesrc;

static int32_t filesrc_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    filesrc *s = ctx;
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < len) {
        if (s->pos != off + done) {
            if (!vfs_seek(&s->f, off + done))
                return done ? (int32_t)done : -1;
            s->pos = off + done;
        }
        uint32_t n = len - done;
        if (n > BOUNCE) n = BOUNCE;
        s32 r = vfs_read(&s->f, s->bounce, n);
        if (r < 0) { s->pos = (u64)-1; return done ? (int32_t)done : -1; }
        if (r == 0) break;
        memcpy(dst + done, s->bounce, r);
        done += (uint32_t)r;
        s->pos += (u64)r;
        if ((uint32_t)r < n) break;
    }
    return (int32_t)done;
}

static void filesrc_close(void *ctx)
{
    filesrc *s = ctx;
    vfs_close(&s->f);
    free(s->bounce);
    free(s);
}

static bool filesrc_make(src_t *out, const char *path)
{
    filesrc *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->bounce = memalign(32, BOUNCE);
    if (!s->bounce || !vfs_open_read(&s->f, path)) {
        free(s->bounce);
        free(s);
        return false;
    }
    out->read = filesrc_read;
    out->close = filesrc_close;
    out->ctx = s;
    out->size = s->f.size;
    return true;
}

/* an entry of an already open archive, used as the source of a nested archive */
typedef struct {
    entry *parent;
    int node;
} nestsrc;

static void entry_release(entry *e) { if (e && e->refs > 0) e->refs--; }

static int32_t nestsrc_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    nestsrc *s = ctx;
    archive_t *a = s->parent->a;
    return a->read(a, &a->tree.n[s->node], off, buf, len);
}

static void nestsrc_close(void *ctx)
{
    nestsrc *s = ctx;
    entry_release(s->parent);
    free(s);
}

/* ---------------------------------------------------------------- cache */

/* ---------------------------------------------------------------- WFS keys */

/* Keys for Wii U volumes come from files on the SD card:
 *   sd:/wfs/usb.key, mlc.key   the 16 byte AES keys themselves
 *   sd:/wfs/otp.bin (+ seeprom.bin)  a Wii U's OTP / SEEPROM dump; the USB key is derived from both, the MLC key from the OTP
 * (the same files are looked for in the root of the card). */
#define WFS_KEYS_MAX 8
static uint8_t wfs_keys[WFS_KEYS_MAX][16];
static int wfs_key_count;
static bool wfs_keys_loaded;

static int read_small(const char *path, uint8_t *out, uint32_t max)
{
    vfs_file f;
    if (!vfs_open_read(&f, path))
        return -1;
    uint8_t *b = memalign(32, (max + 31) & ~31u);
    if (!b) { vfs_close(&f); return -1; }
    uint32_t got = 0;
    while (got < max) {
        s32 r = vfs_read(&f, b + got, max - got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    vfs_close(&f);
    memcpy(out, b, got);
    memset(b, 0, max);
    free(b);
    return (int)got;
}

static void add_key(const uint8_t key[16])
{
    for (int i = 0; i < wfs_key_count; i++)
        if (!memcmp(wfs_keys[i], key, 16)) return;
    if (wfs_key_count < WFS_KEYS_MAX)
        memcpy(wfs_keys[wfs_key_count++], key, 16);
}

static void load_wfs_keys(void)
{
    static const char *dirs[] = { "sd:/wfs/", "sd:/" };
    wfs_key_count = 0;
    wfs_keys_loaded = true;
    if (!g_devs[DEV_SD].mounted)
        return;
    for (int d = 0; d < 2; d++) {
        char path[64];
        uint8_t k[16];
        snprintf(path, sizeof(path), "%susb.key", dirs[d]);
        if (read_small(path, k, 16) == 16) add_key(k);
        snprintf(path, sizeof(path), "%smlc.key", dirs[d]);
        if (read_small(path, k, 16) == 16) add_key(k);

        static uint8_t otp[0x400], seeprom[0x200];
        snprintf(path, sizeof(path), "%sotp.bin", dirs[d]);
        int on = read_small(path, otp, sizeof(otp));
        if (on >= 0x190) {
            add_key(otp + 0x180);                                           /* MLC */
            snprintf(path, sizeof(path), "%sseeprom.bin", dirs[d]);
            int sn = read_small(path, seeprom, sizeof(seeprom));
            if (sn >= 0xC0) {
                uint8_t usb[16];
                aes128_ecb_encrypt(otp + 0x130, seeprom + 0xB0, usb);      /* USB: seed encrypted with the OTP key */
                add_key(usb);
                memset(usb, 0, sizeof(usb));
            }
        }
        memset(otp, 0, sizeof(otp));
        memset(seeprom, 0, sizeof(seeprom));
    }
}

static bool wfs_key_provider(int index, uint8_t key[16])
{
    if (!wfs_keys_loaded)
        load_wfs_keys();
    if (index < 0 || index >= wfs_key_count)
        return false;
    memcpy(key, wfs_keys[index], 16);
    return true;
}

static bool prng_provider(uint8_t key[16]);

static void init_providers(void)
{
    if (provider_set)
        return;
    archive_set_prng_key_provider(prng_provider);
    archive_set_wfs_key_provider(wfs_key_provider);
    provider_set = true;
}

/* ---------------------------------------------------------------- raw USB source */

typedef struct { int slot; } rawsrc;

static int32_t rawsrc_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    rawsrc *r = ctx;
    uint64_t size = vfs_usb_raw_size(r->slot);
    if (off >= size) return 0;
    if (len > size - off) len = (uint32_t)(size - off);
    return vfs_usb_raw_read(r->slot, off, buf, len) ? (int32_t)len : -1;
}

static void rawsrc_close(void *ctx) { free(ctx); }

static void entry_free(entry *e)
{
    if (e->a) archive_close(e->a);
    memset(e, 0, sizeof(*e));
}

void arcfs_flush(void)
{
    wfs_keys_loaded = false;
    for (int pass = 0; pass < CACHE_N; pass++)          /* freeing a nested archive releases its parent */
        for (int i = 0; i < CACHE_N; i++)
            if (cache[i].a && cache[i].refs == 0)
                entry_free(&cache[i]);
}

static bool prng_provider(uint8_t key[16])
{
    u8 *otp = memalign(32, OTP_SIZE);
    if (!otp) return false;
    bool ok = otp_read_all(otp);
    if (ok) memcpy(key, ((otp_t *)otp)->rng_key, 16);
    memset(otp, 0, OTP_SIZE);
    free(otp);
    return ok;
}

static entry *cache_lookup(const char *key)
{
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].a && strcmp(cache[i].key, key) == 0) {
            cache[i].stamp = ++clock_tick;
            return &cache[i];
        }
    return NULL;
}

static entry *cache_slot(void)
{
    entry *best = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        if (!cache[i].a) return &cache[i];
        if (cache[i].refs == 0 && (!best || cache[i].stamp < best->stamp)) best = &cache[i];
    }
    if (best) entry_free(best);
    return best;
}

static entry *get_archive(const char *spec, size_t n);

/* spec[0..n) names a file: a real path, or inner of another archive */
static bool open_src(const char *spec, size_t n, src_t *out, char *name, size_t namesz)
{
    char buf[KEY_MAX];
    if (n >= sizeof(buf)) return false;
    memcpy(buf, spec, n);
    buf[n] = '\0';
    const char *bar = NULL;
    for (const char *p = buf; *p; p++)
        if (*p == '|') bar = p;
    if (!bar) {
        snprintf(name, namesz, "%s", vfs_basename(buf));
        return filesrc_make(out, buf);
    }
    entry *parent = get_archive(spec, (size_t)(bar - buf));
    if (!parent) return false;
    const char *inner = bar + 1;
    while (*inner == '/') inner++;
    int node = archive_find(parent->a, inner);
    if (node < 0 || parent->a->tree.n[node].is_dir) return false;
    nestsrc *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->parent = parent;
    s->node = node;
    parent->refs++;
    out->read = nestsrc_read;
    out->close = nestsrc_close;
    out->ctx = s;
    out->size = parent->a->tree.n[node].size;
    const char *base = strrchr(inner, '/');
    snprintf(name, namesz, "%s", base ? base + 1 : inner);
    return true;
}

static entry *get_archive(const char *spec, size_t n)
{
    char key[KEY_MAX];
    if (n >= sizeof(key)) { g_archive_err = "Path is too long"; return NULL; }
    memcpy(key, spec, n);
    key[n] = '\0';
    entry *e = cache_lookup(key);
    if (e) return e;

    init_providers();
    g_archive_err = NULL;
    src_t src;
    char name[VFS_NAME_MAX];
    if (!open_src(spec, n, &src, name, sizeof(name))) {
        if (!g_archive_err) g_archive_err = "Can't read that file";
        return NULL;
    }
    archive_t *a = archive_open(&src, name);
    if (!a) {
        if (src.close) src.close(src.ctx);
        if (!g_archive_err) g_archive_err = "Not a supported archive";
        return NULL;
    }
    e = cache_slot();
    if (!e) { archive_close(a); g_archive_err = "Too many archives open"; return NULL; }
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->a = a;
    e->refs = 0;
    e->stamp = ++clock_tick;
    return e;
}

/* "x.wad|/a/b" -> archive of "x.wad" and node of "a/b" */
static bool resolve(const char *path, entry **e, int *node)
{
    const char *bar = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '|') bar = p;
    if (!bar) {                                     /* a path on a WFS volume: "usb1:/usr/title" */
        int dev = vfs_dev_of(path);
        if (!arcfs_dev_pinned(dev)) return false;
        *e = &pins[dev - DEV_USB0];
        const char *rest = path + strlen(g_devs[dev].prefix);
        char rel[KEY_MAX];
        snprintf(rel, sizeof(rel), "%s", rest);
        size_t l = strlen(rel);
        while (l && rel[l - 1] == '/') rel[--l] = '\0';
        *node = archive_find((*e)->a, rel);
        if (*node < 0) { g_archive_err = "Not found on the volume"; return false; }
        return true;
    }
    *e = get_archive(path, (size_t)(bar - path));
    if (!*e) return false;
    const char *inner = bar + 1;
    while (*inner == '/') inner++;
    char rel[KEY_MAX];
    snprintf(rel, sizeof(rel), "%s", inner);
    size_t l = strlen(rel);
    while (l && rel[l - 1] == '/') rel[--l] = '\0';
    *node = archive_find((*e)->a, rel);
    if (*node < 0) { g_archive_err = "Not found in the archive"; return false; }
    return true;
}

/* ---------------------------------------------------------------- public */

bool arcfs_dev_pinned(int dev)
{
    return dev >= DEV_USB0 && dev < DEV_USB0 + USB_SLOTS && pins[dev - DEV_USB0].a != NULL;
}

bool arcfs_is_path(const char *path)
{
    return strchr(path, '|') != NULL || arcfs_dev_pinned(vfs_dev_of(path));
}

bool arcfs_mount_wfs(int dev, int usb_slot)
{
    if (dev < DEV_USB0 || dev >= DEV_USB0 + USB_SLOTS || pins[dev - DEV_USB0].a)
        return false;
    init_providers();
    load_wfs_keys();
    if (!wfs_key_count && vfs_usb_raw_size(usb_slot) == 0)
        return false;
    rawsrc *r = calloc(1, sizeof(*r));
    if (!r) return false;
    r->slot = usb_slot;
    src_t src = { rawsrc_read, rawsrc_close, r, vfs_usb_raw_size(usb_slot) };
    if (!arc_wfs_probe(&src, "")) {
        rawsrc_close(r);
        return false;
    }
    g_archive_err = NULL;
    archive_t *a = arc_wfs_open(&src, "");
    if (!a) {
        rawsrc_close(r);
        return false;
    }
    entry *e = &pins[dev - DEV_USB0];
    snprintf(e->key, sizeof(e->key), "%s", g_devs[dev].prefix);
    e->a = a;
    e->refs = 0;
    return true;
}

void arcfs_unpin_all(void)
{
    arcfs_flush();
    for (int i = 0; i < USB_SLOTS; i++)
        entry_free(&pins[i]);
    memset(wfs_keys, 0, sizeof(wfs_keys));
    wfs_key_count = 0;
    wfs_keys_loaded = false;
}

bool arcfs_container(const char *path, char *out, size_t outsz)
{
    const char *bar = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '|') bar = p;
    if (!bar) return false;
    size_t n = (size_t)(bar - path);
    if (n >= outsz) return false;
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

const char *arcfs_error(void)
{
    return g_archive_err ? g_archive_err : "Can't open that archive";
}

bool arcfs_can_open(const char *path)
{
    init_providers();
    entry *e = cache_lookup(path);
    if (e) return true;
    src_t src;
    char name[VFS_NAME_MAX];
    if (!open_src(path, strlen(path), &src, name, sizeof(name)))
        return false;
    bool ok = src.size >= 8 && archive_probe(&src, name);
    if (src.close) src.close(src.ctx);
    return ok;
}

bool arcfs_list(const char *path, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;
    entry *e;
    int node;
    if (!resolve(path, &e, &node) || !e->a->tree.n[node].is_dir)
        return false;
    archive_expand(e->a, node);
    const tree_t *t = &e->a->tree;
    int n = 0;
    for (int i = 1; i < t->count; i++)
        if (t->n[i].parent == node) n++;
    if (n) {
        *out = calloc(n, sizeof(vfs_entry));
        if (!*out) return false;
    }
    int k = 0;
    for (int i = 1; i < t->count && k < n; i++) {
        if (t->n[i].parent != node) continue;
        vfs_entry *v = &(*out)[k++];
        snprintf(v->name, sizeof(v->name), "%s", t->n[i].name);
        v->is_dir = t->n[i].is_dir;
        v->size = t->n[i].is_dir ? 0 : t->n[i].size;
    }
    *count = k;
    return true;
}

bool arcfs_stat(const char *path, bool *is_dir, u64 *size)
{
    entry *e;
    int node;
    if (!resolve(path, &e, &node))
        return false;
    const tnode *t = &e->a->tree.n[node];
    if (is_dir) *is_dir = t->is_dir;
    if (size) *size = t->is_dir ? 0 : t->size;
    return true;
}

typedef struct {
    entry *e;
    int node;
} arcfile;

bool arcfs_open(vfs_file *f, const char *path)
{
    entry *e;
    int node;
    if (!resolve(path, &e, &node) || e->a->tree.n[node].is_dir)
        return false;
    arcfile *h = calloc(1, sizeof(*h));
    if (!h) return false;
    h->e = e;
    h->node = node;
    e->refs++;
    f->arc = true;
    f->ctx = h;
    f->size = e->a->tree.n[node].size;
    f->pos = 0;
    return true;
}

s32 arcfs_read(vfs_file *f, void *buf, u32 len)
{
    arcfile *h = f->ctx;
    if (!h || f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = (u32)(f->size - f->pos);
    archive_t *a = h->e->a;
    int32_t r = a->read(a, &a->tree.n[h->node], f->pos, buf, len);
    if (r > 0) f->pos += (u64)r;
    return r;
}

bool arcfs_seek(vfs_file *f, u64 off)
{
    if (off > f->size) return false;
    f->pos = off;
    return true;
}

void arcfs_close(vfs_file *f)
{
    arcfile *h = f->ctx;
    if (h) {
        entry_release(h->e);
        free(h);
    }
    f->ctx = NULL;
    f->arc = false;
}

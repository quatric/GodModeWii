/*
 * Image viewer. See media.h.
 *
 * GX texture formats (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2,
 * CMPR) in the first-party GameCube / Wii containers nintoolbox reads:
 *   TPL, BTI, TEX0 (.tex0 / .brtex, with its PLT0 when inside a BRRES),
 *   BRFNT font sheets, BREFT effect textures, J3D BMD / BDL (TEX1)
 * plus PNG, JPEG and WebP through devkitPro's portlibs (libpng, libjpeg,
 * libwebp). View only: nothing is converted or written.
 *
 * Everything is read with explicit big-endian accessors and pixels stay byte
 * arrays (R, G, B, A), so nothing depends on the host's byte order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <gccore.h>
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include "archive.h"
#include "arcfs.h"

#define FILE_MAX (32u * 1024 * 1024)
#define MAX_IMAGES 512

enum { TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
       TF_C4 = 8, TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14, IMG_RGBA = 0x100 };

typedef struct {
    int fmt, w, h;
    const u8 *data;
    u32 avail;                  /* bytes from data to the end of the buffer */
    const u8 *pal;
    int palfmt, palcount;
    char label[64];
    u8 *rgba;                   /* IMG_RGBA: already decoded, owned */
} img_t;

typedef struct {
    u8 *buf;
    u32 len;
    u8 *pal_buf;                /* PLT0 loaded from a sibling file */
    img_t img[MAX_IMAGES];
    int n;
    const char *kind;
} set_t;

/* ---------------------------------------------------------------- GX decoding */

static const char *fmt_name(int f)
{
    switch (f) {
    case TF_I4: return "I4"; case TF_I8: return "I8"; case TF_IA4: return "IA4"; case TF_IA8: return "IA8";
    case TF_RGB565: return "RGB565"; case TF_RGB5A3: return "RGB5A3"; case TF_RGBA8: return "RGBA8";
    case TF_C4: return "C4"; case TF_C8: return "C8"; case TF_C14X2: return "C14X2"; case TF_CMPR: return "CMPR";
    case IMG_RGBA: return "RGBA";
    default: return "?";
    }
}

static int bpp(int f)
{
    switch (f) {
    case TF_I4: case TF_C4: case TF_CMPR: return 4;
    case TF_I8: case TF_IA4: case TF_C8: return 8;
    case TF_RGBA8: return 32;
    default: return 16;
    }
}

static void blk(int f, int *bw, int *bh)
{
    switch (f) {
    case TF_I4: case TF_C4: case TF_CMPR: *bw = 8; *bh = 8; break;
    case TF_I8: case TF_IA4: case TF_C8: *bw = 8; *bh = 4; break;
    default: *bw = 4; *bh = 4; break;
    }
}

static u32 tex_size(int f, int w, int h)
{
    int bw, bh;
    blk(f, &bw, &bh);
    u32 W = (w + bw - 1) / bw * bw, H = (h + bh - 1) / bh * bh;
    return W * H * bpp(f) / 8;
}

static void c565(u16 v, u8 *o) { o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; }

static void c5a3(u16 v, u8 *o)
{
    if (v & 0x8000) { o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = 255; }
    else { o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = ((v >> 12) & 7) * 255 / 7; }
}

static void pal_color(const img_t *im, u32 idx, u8 *o)
{
    if (!im->pal || idx >= (u32)im->palcount) { o[0] = o[1] = o[2] = (u8)idx; o[3] = 255; return; }
    u16 v = be16(im->pal + idx * 2);
    if (im->palfmt == 0) { o[0] = o[1] = o[2] = v & 0xFF; o[3] = v >> 8; }
    else if (im->palfmt == 1) c565(v, o);
    else c5a3(v, o);
}

/* Fills out (w*h*4, RGBA bytes). False if the data is too short. */
static bool gx_decode(const img_t *im, u8 *out)
{
    int f = im->fmt, w = im->w, h = im->h, bw, bh;
    if (tex_size(f, w, h) > im->avail) return false;
    blk(f, &bw, &bh);
    const u8 *s = im->data;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (f == TF_CMPR) {
                for (int sub = 0; sub < 4; sub++, s += 8) {
                    u16 c0 = be16(s), c1 = be16(s + 2);
                    u8 pal[4][4];
                    c565(c0, pal[0]);
                    c565(c1, pal[1]);
                    for (int k = 0; k < 3; k++) {
                        if (c0 > c1) {
                            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
                            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
                        } else {
                            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
                            pal[3][k] = pal[2][k];
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = c0 > c1 ? 255 : 0;
                    int ox = bx + (sub & 1) * 4, oy = by + (sub >> 1) * 4;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            int px = ox + x, py = oy + y;
                            if (px >= w || py >= h) continue;
                            int idx = (s[4 + y] >> (6 - x * 2)) & 3;
                            memcpy(out + (py * w + px) * 4, pal[idx], 4);
                        }
                }
                continue;
            }
            if (f == TF_RGBA8) {
                for (int i = 0; i < 16; i++) {
                    int px = bx + (i & 3), py = by + (i >> 2);
                    if (px >= w || py >= h) continue;
                    u8 *o = out + (py * w + px) * 4;
                    o[3] = s[i * 2]; o[0] = s[i * 2 + 1];
                    o[1] = s[32 + i * 2]; o[2] = s[32 + i * 2 + 1];
                }
                s += 64;
                continue;
            }
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int px = bx + x, py = by + y, i = y * bw + x;
                    u8 c[4];
                    switch (f) {
                    case TF_I4: { u8 v = (s[i / 2] >> (i & 1 ? 0 : 4)) & 15; c[0] = c[1] = c[2] = v * 17; c[3] = 255; break; }
                    case TF_I8: c[0] = c[1] = c[2] = s[i]; c[3] = 255; break;
                    case TF_IA4: c[0] = c[1] = c[2] = (s[i] & 15) * 17; c[3] = (s[i] >> 4) * 17; break;
                    case TF_IA8: c[0] = c[1] = c[2] = s[i * 2 + 1]; c[3] = s[i * 2]; break;
                    case TF_RGB565: c565(be16(s + i * 2), c); break;
                    case TF_RGB5A3: c5a3(be16(s + i * 2), c); break;
                    case TF_C4: pal_color(im, (s[i / 2] >> (i & 1 ? 0 : 4)) & 15, c); break;
                    case TF_C8: pal_color(im, s[i], c); break;
                    case TF_C14X2: pal_color(im, be16(s + i * 2) & 0x3FFF, c); break;
                    default: c[0] = c[1] = c[2] = 0; c[3] = 255; break;
                    }
                    if (px < w && py < h) memcpy(out + (py * w + px) * 4, c, 4);
                }
            s += bw * bh * bpp(f) / 8;
        }
    return true;
}

u32 image_gx_size(int fmt, int w, int h)
{
    return tex_size(fmt, w, h);
}

u8 *image_decode_gx(const u8 *data, u32 avail, int fmt, int w, int h, const u8 *pal, int palfmt, int palcount)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    img_t im = { .fmt = fmt, .w = w, .h = h, .data = data, .avail = avail, .pal = pal, .palfmt = palfmt, .palcount = palcount };
    u8 *out = malloc(w * h * 4);
    if (out && !gx_decode(&im, out)) { free(out); out = NULL; }
    return out;
}

/* ---------------------------------------------------------------- containers */

static bool valid_fmt(int f) { return (f >= 0 && f <= 6) || (f >= 8 && f <= 10) || f == 14; }

static img_t *add(set_t *s, int fmt, int w, int h, u32 off, const char *label)
{
    if (s->n >= MAX_IMAGES || !valid_fmt(fmt) || w <= 0 || h <= 0 || w > 4096 || h > 4096 || off >= s->len) return NULL;
    img_t *im = &s->img[s->n];
    memset(im, 0, sizeof(*im));
    im->fmt = fmt; im->w = w; im->h = h;
    im->data = s->buf + off;
    im->avail = s->len - off;
    snprintf(im->label, sizeof(im->label), "%s", label);
    if (tex_size(fmt, w, h) > im->avail) return NULL;
    s->n++;
    return im;
}

static void set_pal(set_t *s, img_t *im, int fmt, int count, u32 off)
{
    if (!im || off >= s->len || (u64)off + count * 2ull > s->len) return;
    im->pal = s->buf + off;
    im->palfmt = fmt;
    im->palcount = count;
}

static void parse_tpl(set_t *s)
{
    s->kind = "TPL";
    u32 n = be32(s->buf + 4), tab = be32(s->buf + 8);
    for (u32 i = 0; i < n && tab + i * 8 + 8 <= s->len; i++) {
        u32 ih = be32(s->buf + tab + i * 8), ph = be32(s->buf + tab + i * 8 + 4);
        if (ih + 0x24 > s->len) continue;
        const u8 *h = s->buf + ih;
        char lab[32];
        snprintf(lab, sizeof(lab), "image %u", (unsigned)i + 1);
        img_t *im = add(s, be32(h + 4), be16(h + 2), be16(h), be32(h + 8), lab);
        if (im && ph && ph + 12 <= s->len)
            set_pal(s, im, be32(s->buf + ph + 4), be16(s->buf + ph), be32(s->buf + ph + 8));
    }
}

/* A BTI header at `at` (0x20 bytes); offsets are relative to it. */
static void parse_bti_at(set_t *s, u32 at, const char *label)
{
    if (at + 0x20 > s->len) return;
    const u8 *h = s->buf + at;
    img_t *im = add(s, h[0], be16(h + 2), be16(h + 4), at + be32(h + 0x1C), label);
    if (im && be16(h + 0x0A))
        set_pal(s, im, h[9], be16(h + 0x0A), at + be32(h + 0x0C));
}

static void parse_j3d(set_t *s)
{
    s->kind = "J3D model";
    u32 nsec = be32(s->buf + 0x0C), pos = 0x20;
    for (u32 i = 0; i < nsec && pos + 8 <= s->len; i++) {
        u32 sz = be32(s->buf + pos + 4);
        if (!memcmp(s->buf + pos, "TEX1", 4) && pos + 0x14 <= s->len) {
            u32 cnt = be16(s->buf + pos + 8), hdr = pos + be32(s->buf + pos + 0x0C);
            for (u32 k = 0; k < cnt; k++) {
                char lab[32];
                snprintf(lab, sizeof(lab), "texture %u", (unsigned)k + 1);
                parse_bti_at(s, hdr + k * 0x20, lab);
            }
        }
        if (sz < 8) break;
        pos += sz;
    }
}

static void parse_tex0(set_t *s, u32 at, const char *label)
{
    if (at + 0x40 > s->len) return;
    const u8 *t = s->buf + at;
    img_t *im = add(s, be32(t + 0x20), be16(t + 0x1C), be16(t + 0x1E), at + be32(t + 0x10), label);
    if (im && (im->fmt == TF_C4 || im->fmt == TF_C8 || im->fmt == TF_C14X2) && s->pal_buf) {
        const u8 *p = s->pal_buf;            /* a PLT0: data offset at +0x10, format +0x18, count +0x1C */
        u32 plen = be32(p + 4);
        u32 doff = be32(p + 0x10);
        int cnt = be16(p + 0x1C);
        if (doff + cnt * 2u <= plen) { im->pal = p + doff; im->palfmt = be32(p + 0x18); im->palcount = cnt; }
    }
}

static void parse_brfnt(set_t *s)
{
    s->kind = "BRFNT font";
    u32 nsec = be16(s->buf + 0x0E), pos = be16(s->buf + 0x0C);
    for (u32 i = 0; i < nsec && pos + 0x20 <= s->len; i++) {
        u32 sz = be32(s->buf + pos + 4);
        if (!memcmp(s->buf + pos, "TGLP", 4)) {
            const u8 *g = s->buf + pos;
            u32 sheet = be32(g + 0x0C), cnt = be16(g + 0x10), fmt = be16(g + 0x12);
            if (fmt & 0x8000) break;                /* compressed sheets (.brfna): not shown */
            u32 data = be32(g + 0x1C);
            for (u32 k = 0; k < cnt && data + (u64)sheet * (k + 1) <= s->len; k++) {
                char lab[32];
                snprintf(lab, sizeof(lab), "sheet %u", (unsigned)k + 1);
                add(s, fmt & 0xFF, be16(g + 0x18), be16(g + 0x1A), data + sheet * k, lab);
            }
            break;
        }
        if (sz < 8) break;
        pos += sz;
    }
}

static void parse_breft(set_t *s)
{
    s->kind = "BREFT";
    u32 root = 0x18;                                /* file header 0x10, then the REFT block's 8-byte header */
    if (root + 0x10 > s->len) return;
    u32 list = root + be32(s->buf + root);
    if (list + 8 > s->len) return;
    u32 n = be16(s->buf + list + 4), pos = list + 8;
    for (u32 i = 0; i < n && pos + 2 <= s->len; i++) {
        u32 nl = be16(s->buf + pos);
        if (pos + 2 + nl + 8 > s->len) break;
        char lab[64];
        snprintf(lab, sizeof(lab), "%.*s", (int)(nl ? nl - 1 : 0), (const char *)s->buf + pos + 2);
        u32 off = list + be32(s->buf + pos + 2 + nl);
        pos += 2 + nl + 8;
        if (off + 0x20 > s->len) continue;
        const u8 *h = s->buf + off;
        img_t *im = add(s, h[0x0C], be16(h + 4), be16(h + 6), off + 0x20, lab);
        if (im && be16(h + 0x0E))
            set_pal(s, im, h[0x0D], be16(h + 0x0E), off + 0x20 + be32(h + 8));
    }
}

/* ---------------------------------------------------------------- PNG / JPEG / WebP */

static img_t *add_rgba(set_t *s, int w, int h, u8 *rgba, const char *kind)
{
    if (!rgba) return NULL;
    img_t *im = &s->img[0];
    memset(im, 0, sizeof(*im));
    im->fmt = IMG_RGBA; im->w = w; im->h = h; im->rgba = rgba;
    snprintf(im->label, sizeof(im->label), "%s", kind);
    s->kind = kind;
    s->n = 1;
    return im;
}

typedef struct { const u8 *p; u32 left; } pngsrc;
static void png_read_mem(png_structp png, png_bytep out, png_size_t n)
{
    pngsrc *r = png_get_io_ptr(png);
    if (n > r->left) png_error(png, "short");
    memcpy(out, r->p, n);
    r->p += n;
    r->left -= n;
}

static void load_png(set_t *s)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    u8 *rgba = NULL;
    png_bytep *rows = NULL;
    if (!info) goto out;
    if (setjmp(png_jmpbuf(png))) { free(rgba); rgba = NULL; goto out; }
    pngsrc r = { s->buf, s->len };
    png_set_read_fn(png, &r, png_read_mem);
    png_read_info(png, info);
    u32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    if (!w || !h || w > 4096 || h > 4096) goto out;
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    rgba = malloc(w * h * 4);
    rows = malloc(h * sizeof(png_bytep));
    if (!rgba || !rows) { free(rgba); rgba = NULL; goto out; }
    for (u32 y = 0; y < h; y++) rows[y] = rgba + y * w * 4;
    png_read_image(png, rows);
    add_rgba(s, w, h, rgba, "PNG");
out:
    free(rows);
    png_destroy_read_struct(&png, info ? &info : NULL, NULL);
}

struct jerr { struct jpeg_error_mgr mgr; jmp_buf jb; };
static void jpeg_fail(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static void load_jpeg(set_t *s)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    u8 *rgba = NULL, *row = NULL;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jb)) { jpeg_destroy_decompress(&cinfo); free(rgba); free(row); return; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, s->buf, s->len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    while (cinfo.image_width / cinfo.scale_denom > 4096 || cinfo.image_height / cinfo.scale_denom > 4096) {
        if (cinfo.scale_denom >= 8) { jpeg_destroy_decompress(&cinfo); return; }
        cinfo.scale_denom *= 2;
    }
    jpeg_start_decompress(&cinfo);
    u32 w = cinfo.output_width, h = cinfo.output_height;
    rgba = malloc(w * h * 4);
    row = malloc(w * 3);
    if (!rgba || !row) longjmp(err.jb, 1);
    while (cinfo.output_scanline < h) {
        u32 y = cinfo.output_scanline;
        JSAMPROW rp = row;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        for (u32 x = 0; x < w; x++) {
            u8 *o = rgba + (y * w + x) * 4;
            o[0] = row[x * 3]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3 + 2]; o[3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);
    add_rgba(s, w, h, rgba, "JPEG");
}

static void load_webp(set_t *s)
{
    int w, h;
    if (!WebPGetInfo(s->buf, s->len, &w, &h) || w > 4096 || h > 4096) return;
    u8 *px = WebPDecodeRGBA(s->buf, s->len, &w, &h);
    if (!px) return;
    u8 *copy = malloc(w * h * 4);
    if (copy) memcpy(copy, px, w * h * 4);
    WebPFree(px);
    add_rgba(s, w, h, copy, "WebP");
}

/* ---------------------------------------------------------------- loading */

static u8 *load_file(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > FILE_MAX || f.size < 8) { vfs_close(&f); return NULL; }
    u32 n = (u32)f.size, got = 0;
    u8 *b = memalign(32, (n + 31) & ~31u);
    while (b && got < n) {
        s32 r = vfs_read(&f, b + got, n - got > 0x40000 ? 0x40000 : n - got);
        if (r <= 0) break;
        got += r;
    }
    vfs_close(&f);
    if (b && got != n) { free(b); return NULL; }
    *len = n;
    return b;
}

/* For "x.brres|/Textures(NW4R)/name.tex0": the matching "Palettes(NW4R)/name.plt0". */
static u8 *sibling_palette(const char *path)
{
    const char *tex = strstr(path, "/Textures(NW4R)/");
    if (!tex) return NULL;
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%.*s/Palettes(NW4R)/%s", (int)(tex - path), path, tex + 16);
    size_t l = strlen(p);
    if (l > 5 && !strcasecmp(p + l - 5, ".tex0")) memcpy(p + l - 5, ".plt0", 5);
    u32 len;
    u8 *b = load_file(p, &len);
    if (b && (len < 0x20 || memcmp(b, "PLT0", 4))) { free(b); b = NULL; }
    return b;
}

/* Retro Studios TXTR (Metroid Prime 1-3, Donkey Kong Country Returns): no
 * magic, big endian format / width / height / mip count, then for C4 / C8 a
 * palette (format, width x height entries), then GX tiled mips. */
static void parse_txtr(set_t *s)
{
    static const int gx[] = { TF_I4, TF_I8, TF_IA4, TF_IA8, TF_C4, TF_C8, TF_C14X2, TF_RGB565, TF_RGB5A3, TF_RGBA8, TF_CMPR };
    if (s->len < 12) return;
    u32 f = be32(s->buf), mips = be32(s->buf + 8);
    u32 w = be16(s->buf + 4), h = be16(s->buf + 6), off = 12;
    if (f > 10 || f == 6 || !w || !h || w > 4096 || h > 4096 || !mips || mips > 11) return;
    s->kind = "Retro TXTR";
    u32 pf = 0, pc = 0, po = 0;
    if (f == 4 || f == 5) {
        if (off + 8 > s->len) return;
        pf = be32(s->buf + off);
        pc = (u32)be16(s->buf + off + 4) * be16(s->buf + off + 6);
        po = off + 8;
        if (pf > 2 || !pc || pc > (f == 4 ? 16u : 256u)) return;
        off = po + pc * 2;
    }
    img_t *im = add(s, gx[f], w, h, off, "texture");
    if (im && pc) set_pal(s, im, pf, pc, po);
}

static bool load_set(set_t *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->buf = load_file(path, &s->len);
    if (!s->buf) return false;
    const u8 *b = s->buf;
    if (be32(b) == 0x0020AF30) parse_tpl(s);
    else if (!memcmp(b, "TEX0", 4)) { s->kind = "TEX0"; s->pal_buf = sibling_palette(path); parse_tex0(s, 0, vfs_basename(path)); }
    else if (!memcmp(b, "RFNT", 4) || !memcmp(b, "RFNU", 4)) parse_brfnt(s);
    else if (!memcmp(b, "REFT", 4)) parse_breft(s);
    else if (!memcmp(b, "J3D2", 4) || !memcmp(b, "J3D1", 4)) parse_j3d(s);
    else if (!memcmp(b, "\x89PNG", 4)) load_png(s);
    else if (b[0] == 0xFF && b[1] == 0xD8) load_jpeg(s);
    else if (!memcmp(b, "RIFF", 4) && s->len > 12 && !memcmp(b + 8, "WEBP", 4)) load_webp(s);
    else if ((!memcmp(b, "BNR1", 4) || !memcmp(b, "BNR2", 4)) && s->len >= 0x20 + 96 * 32 * 2) {
        s->kind = "GameCube banner";                    /* 96x32 RGB5A3 after a 0x20 byte header */
        add(s, TF_RGB5A3, 96, 32, 0x20, "banner");
    }
    else if (!memcmp(b, "WIBN", 4) && s->len >= 0xA0 + 192 * 64 * 2) {
        s->kind = "Wii save banner";                    /* banner.bin: 192x64 banner, then 48x48 icon frames */
        add(s, TF_RGB5A3, 192, 64, 0xA0, "banner");
        for (u32 off = 0xA0 + 192 * 64 * 2, k = 1; off + 48 * 48 * 2 <= s->len && k <= 8; off += 48 * 48 * 2, k++) {
            char lab[16];
            snprintf(lab, sizeof(lab), "icon %u", (unsigned)k);
            add(s, TF_RGB5A3, 48, 48, off, lab);
        }
    }
    else if (arc_name_has_ext(path, ".txtr")) parse_txtr(s);
    else if (arc_name_has_ext(path, ".bti")) { s->kind = "BTI"; parse_bti_at(s, 0, vfs_basename(path)); }
    return s->n > 0;
}

static void free_set(set_t *s)
{
    for (int i = 0; i < s->n; i++)
        if (s->img[i].fmt == IMG_RGBA) free(s->img[i].rgba);
    free(s->buf);
    free(s->pal_buf);
}

/* ---------------------------------------------------------------- drawing */

/* ITU-R BT.601 with the TV ranges the video interface expects (Y 16-235, U / V 16-240). */
static void rgb_to_yuv(int r, int g, int b, int *y, int *u, int *v)
{
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

enum { BG_CHECK, BG_BLACK, BG_WHITE, BG_COUNT };

/* scale is in 1/256ths of a screen pixel per image pixel (256 = native size). */
static void blit(const u8 *rgba, int w, int h, int scale, int panx, int pany, int bg)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb) return;
    int top = 40, bottom = fh - 40;                 /* leave room for the text lines */
    int vw = fw, vh = bottom - top;
    int cx = w / 2 + panx, cy = h / 2 + pany;
    for (int y = 0; y < fh; y++) {
        /* The framebuffer is uncached: a byte store there writes the whole 32-bit word (the other three
         * bytes become zero, which shows as green), so each pair of pixels goes out as one word. */
        u32 *row = (u32 *)(fb + y * fw * 2);
        int sy = y - top - vh / 2;
        sy = sy >= 0 ? cy + (sy * 256) / scale : cy - ((-sy * 256) + scale - 1) / scale;
        for (int x = 0; x < fw; x += 2) {
            int yy[2], uu = 0, vv = 0;
            for (int k = 0; k < 2; k++) {
                int r = 0, g = 0, b = 0;
                if (y >= top && y < bottom) {
                    int sx = x + k - vw / 2;
                    sx = sx >= 0 ? cx + (sx * 256) / scale : cx - ((-sx * 256) + scale - 1) / scale;
                    int br, bgc, bb;
                    if (bg == BG_CHECK) { int c = (((x + k) >> 4) ^ (y >> 4)) & 1 ? 0x99 : 0x66; br = bgc = bb = c; }
                    else if (bg == BG_WHITE) br = bgc = bb = 255;
                    else br = bgc = bb = 0;
                    if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                        const u8 *p = rgba + (sy * w + sx) * 4;
                        int a = p[3];
                        r = (p[0] * a + br * (255 - a)) / 255;
                        g = (p[1] * a + bgc * (255 - a)) / 255;
                        b = (p[2] * a + bb * (255 - a)) / 255;
                    } else { r = br / 3; g = bgc / 3; b = bb / 3; }
                }
                int u, v;
                rgb_to_yuv(r, g, b, &yy[k], &u, &v);
                uu += u; vv += v;
            }
            row[x / 2] = (u32)yy[0] << 24 | (u32)(uu / 2) << 16 | (u32)yy[1] << 8 | (u32)(vv / 2);   /* Y0 U Y1 V */
        }
    }
}

/* Zoom steps in 1/256ths; index ZOOM_NATIVE is 1:1. */
static const int ZOOMS[] = { 32, 64, 128, 256, 512, 1024, 2048 };
#define ZOOM_COUNT  ((int)(sizeof(ZOOMS) / sizeof(ZOOMS[0])))
#define ZOOM_NATIVE 3

static bool view(const char *path)
{
    set_t *s = calloc(1, sizeof(set_t));
    if (!s) return false;
    ui_splash("Loading image...");
    if (!load_set(s, path)) { free_set(s); free(s); return false; }

    int cur = 0, zoom = ZOOM_NATIVE, panx = 0, pany = 0, bg = BG_CHECK;
    bool info = true, redraw = true, fit = false;
    u8 *rgba = NULL;
    bool owned = false;                         /* rgba was decoded here (PNG / JPEG / WebP pixels belong to the set) */
    int shown = -1;
    const char *err = NULL;
    int fw_all, fh_all;
    ui_framebuffer(&fw_all, &fh_all);
    for (;;) {
        img_t *im = &s->img[cur];
        if (shown != cur) {
            if (owned) free(rgba);
            rgba = NULL;
            owned = false;
            err = NULL;
            if (im->fmt == IMG_RGBA) {
                rgba = im->rgba;
            } else {
                rgba = malloc(im->w * im->h * 4);
                owned = rgba != NULL;
                if (!rgba) err = "Out of memory";
                else if (!gx_decode(im, rgba)) { free(rgba); rgba = NULL; owned = false; err = "Truncated texture data"; }
            }
            shown = cur;
            zoom = ZOOM_NATIVE; panx = pany = 0;
            redraw = true;
        }
        if (redraw) {
            int fw, fh;
            ui_framebuffer(&fw, &fh);
            int scale = ZOOMS[zoom];
            if (fit && rgba) {                      /* the largest size that shows all of it */
                int fx = (fw * 256) / im->w, fy = ((fh - 80) * 256) / im->h;
                scale = fx < fy ? fx : fy;
                if (scale < 1) scale = 1;
            }
            int pct = (scale * 100 + 128) / 256;
            ui_clear();
            if (rgba) blit(rgba, im->w, im->h, scale, panx, pany, bg);
            ui_color(C_WHITE, true);
            ui_field(0, 0, ui_cols() - 1, " %s  %s  [%d/%d] %s", s->kind ? s->kind : "IMAGE", vfs_basename(path), cur + 1, s->n, im->label);
            if (info)
                ui_field(1, 0, ui_cols() - 1, " %dx%d %s%s  %s %d%%", im->w, im->h, fmt_name(im->fmt),
                         im->pal ? " +palette" : "", fit ? "fit" : "zoom", pct);
            if (err) { ui_color(C_RED, true); ui_field(3, 2, ui_cols() - 4, "%s", err); }
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 1, 0, ui_cols() - 1, " <> Prev/Next  +/- Zoom  1 Fit/1:1  D-Pad Pan  A Bg  2 Info  B Close");
            ui_reset();
            redraw = false;
            UI_TEST_SHOT();
        }
        u32 p = input_pressed();
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        int step = im->w / 8 > 8 ? im->w / 8 : 8;
        /* All of the image on screen: Left / Right go to the previous / next one. Bigger: the D-Pad pans. */
        bool whole = fit || !rgba || ((im->w * ZOOMS[zoom]) / 256 <= fw_all && (im->h * ZOOMS[zoom]) / 256 <= fh_all - 80);
        if (!whole && (p & WPAD_BUTTON_LEFT))  { panx -= step; redraw = true; }
        if (!whole && (p & WPAD_BUTTON_RIGHT)) { panx += step; redraw = true; }
        if (!whole && (p & WPAD_BUTTON_UP))    { pany -= step; redraw = true; }
        if (!whole && (p & WPAD_BUTTON_DOWN))  { pany += step; redraw = true; }
        if (whole && s->n > 1 && (p & WPAD_BUTTON_LEFT))  cur = (cur + s->n - 1) % s->n;
        if (whole && s->n > 1 && (p & WPAD_BUTTON_RIGHT)) cur = (cur + 1) % s->n;
        if ((p & WPAD_BUTTON_PLUS) && (fit || zoom < ZOOM_COUNT - 1))  { if (!fit) zoom++; fit = false; redraw = true; }
        if ((p & WPAD_BUTTON_MINUS) && (fit || zoom > 0)) { if (!fit) zoom--; fit = false; redraw = true; }
        if ((p & (WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS)) && rgba &&
            (im->w * ZOOMS[zoom]) / 256 <= fw_all && (im->h * ZOOMS[zoom]) / 256 <= fh_all - 80)
            panx = pany = 0;                        /* back to all of it on screen: centre it again */
        if (p & WPAD_BUTTON_A) { bg = (bg + 1) % BG_COUNT; redraw = true; }
        if (p & WPAD_BUTTON_2) { info = !info; redraw = true; }
        if (p & WPAD_BUTTON_1) { fit = !fit; if (!fit) { zoom = ZOOM_NATIVE; panx = pany = 0; } redraw = true; }
    }
    if (owned) free(rgba);
    free_set(s);
    free(s);
    ui_clear();
    return true;
}

bool image_is_name(const char *name)
{
    static const char *const ext[] = { ".tpl", ".bti", ".tex0", ".brtex", ".brfnt", ".breft", ".bmd", ".bdl",
                                       ".png", ".jpg", ".jpeg", ".webp", ".bnr", "banner.bin", ".txtr", NULL };
    for (int i = 0; ext[i]; i++)
        if (arc_name_has_ext(name, ext[i]))
            return true;
    return false;
}

bool image_view(const char *path)
{
    return view(path);
}

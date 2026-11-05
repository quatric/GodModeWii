/*
 * Mii faces from Arian Kordi's Mii renderer. See miiface.h.
 *
 * As the PictoCard server asks for them: GET /miis/image.png?data=<the 74-byte record in hex>
 * &width=<pixels>&type=face, one Mii per request; the reply is a PNG with a transparent background
 * (400 when the record can't be drawn). Plain HTTP on port 80: the renderer's "unsecure" host
 * serves it, so no TLS (or root certificates) is needed on the Wii.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <png.h>
#include <network.h>
#include <poll.h>
#include <ogc/lwp_watchdog.h>

#include "miiface.h"
#include "app.h"
#include "ftp.h"
#include "ui.h"

#define MII_SIZE   0x4A
#define CACHE_MAX  128
#define TIMEOUT_MS 10000
#define MAX_REPLY  (1u << 20)

typedef struct {
    u8 rec[MII_SIZE];
    miiface_t face;
} entry_t;

static entry_t *cache;
static int ncache;

const miiface_t *miiface_cached(const u8 *rec)
{
    for (int i = 0; i < ncache; i++)
        if (!memcmp(cache[i].rec, rec, MII_SIZE)) return &cache[i].face;
    return NULL;
}

static void remember(const u8 *rec, u8 *rgba)
{
    if (!cache && !(cache = calloc(CACHE_MAX, sizeof(*cache)))) {
        free(rgba);
        return;
    }
    if (ncache == CACHE_MAX) {                      /* full: the oldest goes */
        free(cache[0].face.rgba);
        memmove(cache, cache + 1, (CACHE_MAX - 1) * sizeof(*cache));
        ncache--;
    }
    memcpy(cache[ncache].rec, rec, MII_SIZE);
    cache[ncache].face.rgba = rgba;
    ncache++;
}

/* ---------------------------------------------------------------- PNG */

typedef struct {
    const u8 *p;
    size_t left;
} memsrc;

static void png_mem(png_structp png, png_bytep out, png_size_t n)
{
    memsrc *r = png_get_io_ptr(png);
    if (n > r->left) png_error(png, "short");
    memcpy(out, r->p, n);
    r->p += n;
    r->left -= n;
}

/* A MIIFACE_SIZE square RGBA picture, or NULL. */
static u8 *png_face(const u8 *d, size_t n)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    u8 *volatile rgba = NULL;
    png_bytep *volatile rows = NULL;
    if (!info) goto out;
    if (setjmp(png_jmpbuf(png))) {
        free(rgba);
        rgba = NULL;
        goto out;
    }
    memsrc r = { d, n };
    png_set_read_fn(png, &r, png_mem);
    png_read_info(png, info);
    if (png_get_image_width(png, info) != MIIFACE_SIZE || png_get_image_height(png, info) != MIIFACE_SIZE)
        goto out;
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    rgba = malloc(MIIFACE_SIZE * MIIFACE_SIZE * 4);
    rows = malloc(MIIFACE_SIZE * sizeof(png_bytep));
    if (!rgba || !rows) {
        free(rgba);
        rgba = NULL;
        goto out;
    }
    for (int y = 0; y < MIIFACE_SIZE; y++) rows[y] = rgba + y * MIIFACE_SIZE * 4;
    png_read_image(png, rows);
out:
    free(rows);
    png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    return rgba;
}

/* ---------------------------------------------------------------- request */

static u64 ms_now(void) { return ticks_to_millisecs(gettime()); }

/* Waits until the socket can be read (or written); false after TIMEOUT_MS or on an error. */
static bool sock_wait(s32 fd, int events)
{
    for (u64 start = ms_now(); ms_now() - start < TIMEOUT_MS;) {
        struct pollsd p = { fd, events, 0 };
        s32 r = net_poll(&p, 1, 100);
        if (r < 0) return false;
        if (r > 0) return !(p.revents & (POLLERR | POLLNVAL));
    }
    return false;
}

typedef struct {
    s32 fd;
    u8 buf[2048];
    int pos, len;
    bool eof;
} conn_t;

static int conn_byte(conn_t *c)
{
    if (c->pos == c->len) {
        if (c->eof || !sock_wait(c->fd, POLLIN)) return -1;
        s32 r = net_recv(c->fd, c->buf, sizeof(c->buf), 0);
        if (r <= 0) {
            c->eof = true;
            return -1;
        }
        c->pos = 0;
        c->len = r;
    }
    return c->buf[c->pos++];
}

static bool conn_line(conn_t *c, char *out, int max)
{
    int n = 0, ch;
    while ((ch = conn_byte(c)) >= 0 && ch != '\n')
        if (n + 1 < max) out[n++] = (char)ch;
    if (ch < 0 && !n) return false;
    if (n && out[n - 1] == '\r') n--;
    out[n] = '\0';
    return true;
}

/* Appends up to want bytes (want < 0: to the end) to *body; false when they don't all come. */
static bool conn_take(conn_t *c, u8 **body, u32 *len, long want)
{
    while (want != 0) {
        int ch = conn_byte(c);
        if (ch < 0) return want < 0;
        if (*len >= MAX_REPLY) return false;
        if (!(*len & 4095)) {
            u8 *n = realloc(*body, *len + 4096);
            if (!n) return false;
            *body = n;
        }
        (*body)[(*len)++] = (u8)ch;
        if (want > 0) want--;
    }
    return true;
}

/* GET http://HOST/path; the body of a 200 reply, or NULL (*status says what came back, 0 for no
 * reply at all). */
static u8 *http_get(const char *path, u32 *len, int *status)
{
    *len = 0;
    *status = 0;
    struct hostent *he = net_gethostbyname(MIIFACE_HOST);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return NULL;
    conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    u8 *body = NULL;
    bool ok = false;
    if (c->fd < 0) goto out;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    if (net_connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) goto out;

    char line[600];
    int n = snprintf(line, sizeof(line),
                     "GET %s HTTP/1.1\r\nHost: " MIIFACE_HOST "\r\nUser-Agent: GodModeWii/" GMW_VERSION
                     "\r\nAccept: image/png\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", path);
    for (int sent = 0; sent < n;) {
        if (!sock_wait(c->fd, POLLOUT)) goto out;
        s32 r = net_send(c->fd, line + sent, n - sent, 0);
        if (r <= 0) goto out;
        sent += r;
    }
    if (!conn_line(c, line, sizeof(line)) || strncmp(line, "HTTP/1.", 7)) goto out;
    *status = atoi(line + 9);
    long length = -1;
    bool chunked = false;
    for (;;) {
        if (!conn_line(c, line, sizeof(line))) goto out;
        if (!line[0]) break;
        if (!strncasecmp(line, "content-length:", 15)) length = strtol(line + 15, NULL, 10);
        else if (!strncasecmp(line, "transfer-encoding:", 18) && strstr(line + 18, "chunked")) chunked = true;
    }
    if (*status != 200) goto out;
    if (chunked) {
        for (;;) {
            if (!conn_line(c, line, sizeof(line))) goto out;
            long size = strtol(line, NULL, 16);
            if (size <= 0) break;
            if (!conn_take(c, &body, len, size) || !conn_line(c, line, sizeof(line))) goto out;
        }
        ok = true;
    } else {
        ok = conn_take(c, &body, len, length);
    }
out:
    if (c->fd >= 0) net_close(c->fd);
    free(c);
    if (!ok) {
        free(body);
        *len = 0;
        return NULL;
    }
    return body;
}

/* One Mii's face into the cache; false when the server couldn't be reached. */
static bool fetch_one(const u8 *rec, char *err, size_t errsz)
{
    char path[64 + MII_SIZE * 2];
    int o = snprintf(path, sizeof(path), "/miis/image.png?data=");
    for (int i = 0; i < MII_SIZE; i++) o += snprintf(path + o, sizeof(path) - o, "%02x", rec[i]);
    snprintf(path + o, sizeof(path) - o, "&width=%d&type=face", MIIFACE_SIZE);
    u32 len;
    int st;
    u8 *png = http_get(path, &len, &st);
    if (!png && (st == 0 || st >= 500 || st == 429)) {
        if (!st) snprintf(err, errsz, "Couldn't reach %s.", MIIFACE_HOST);
        else snprintf(err, errsz, "%s answered with HTTP %d.", MIIFACE_HOST, st);
        return false;
    }
    remember(rec, png ? png_face(png, len) : NULL);  /* a 400: the server can't draw this one */
    free(png);
    return true;
}

bool miiface_fetch(const u8 *const *recs, int n, const miiface_t **faces,
                   bool (*progress)(void *ctx, int done, int total), void *ctx, char *err, size_t errsz)
{
    err[0] = '\0';
    bool failed = false, net = false;
    for (int i = 0; i < n && !failed; i++) {
        if (miiface_cached(recs[i])) continue;
        if (!net) {
            if (!ftp_net_start()) {
                snprintf(err, errsz, "The network isn't up, so the faces couldn't be fetched.");
                failed = true;
                break;
            }
            net = true;
        }
        if (progress && !progress(ctx, i, n)) break;
        failed = !fetch_one(recs[i], err, errsz);
    }
    for (int i = 0; i < n && faces; i++) faces[i] = miiface_cached(recs[i]);
    if (progress) progress(ctx, n, n);
    return !failed;
}

/* ---------------------------------------------------------------- drawing */

static void yuv(int r, int g, int b, int *y, int *u, int *v)
{
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

void miiface_draw(const miiface_t *f, int x, int y, int scale, u32 bg_rgb)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb || !f || !f->rgba || scale < 1) return;
    int side = MIIFACE_SIZE * scale, br = bg_rgb >> 16 & 0xFF, bgc = bg_rgb >> 8 & 0xFF, bb = bg_rgb & 0xFF;
    x &= ~1;                                        /* YUY2: pixels go out in pairs */
    for (int dy = 0; dy < side; dy++) {
        int sy = y + dy;
        if (sy < 0 || sy >= fh) continue;
        /* the framebuffer is uncached: whole 32-bit words only (a byte store zeroes the rest) */
        u32 *row = (u32 *)(fb + sy * fw * 2);
        const u8 *src = f->rgba + (dy / scale) * MIIFACE_SIZE * 4;
        for (int dx = 0; dx < side; dx += 2) {
            int sx = x + dx;
            if (sx < 0 || sx + 1 >= fw) continue;
            int yy[2], uu = 0, vv = 0;
            for (int k = 0; k < 2; k++) {
                const u8 *p = src + ((dx + k) / scale) * 4;
                int a = p[3], u, v;
                yuv((p[0] * a + br * (255 - a)) / 255, (p[1] * a + bgc * (255 - a)) / 255,
                    (p[2] * a + bb * (255 - a)) / 255, &yy[k], &u, &v);
                uu += u;
                vv += v;
            }
            row[sx / 2] = (u32)yy[0] << 24 | (u32)(uu / 2) << 16 | (u32)yy[1] << 8 | (u32)(vv / 2);
        }
    }
}

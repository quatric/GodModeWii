#include <gccore.h>
#include <ogcsys.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include "video.h"

static GXRModeObj *rmode;
static void *xfb[2];
static int cur = 0;

static vu32 g_vsync = 0;
static VIRetraceCallback prev_cb;
static void retrace_cb(u32 count) { g_vsync++; if (prev_cb) prev_cb(count); }
unsigned vid_retraces(void) { return g_vsync; }

/* GodModeWii: the app already owns VIDEO_Init and its console framebuffer.
 * Playback borrows two extra framebuffers (allocated once, kept for reuse)
 * and hands the screen back to the console when it is done. */
int vid_attach(void)
{
    rmode = VIDEO_GetPreferredMode(NULL);
    for (int i = 0; i < 2; i++)
        if (!xfb[i]) {
            void *p = SYS_AllocateFramebuffer(rmode);
            if (!p) return 0;
            xfb[i] = MEM_K0_TO_K1(p);
        }
    prev_cb = VIDEO_SetPreRetraceCallback(retrace_cb);
    cur = 0;
    vid_clear_both();
    VIDEO_SetNextFramebuffer(xfb[1]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    return 1;
}

void vid_detach(void *console_xfb)
{
    VIDEO_SetPreRetraceCallback(prev_cb);
    prev_cb = NULL;
    VIDEO_SetNextFramebuffer(console_xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

int vid_screen_w(void) { return rmode->fbWidth; }
int vid_screen_h(void) { return rmode->xfbHeight; }
void *vid_xfb(void) { return xfb[cur]; }

void vid_flip(void)
{
    VIDEO_SetNextFramebuffer(xfb[cur]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    cur ^= 1;
}

void vid_clear(void)
{
    uint32_t *p = (uint32_t *)xfb[cur];
    int n = rmode->fbWidth * rmode->xfbHeight / 2;
    uint32_t black = 0x10801080;     /* Y=16 Cb=128 Y=16 Cr=128 */
    for (int i = 0; i < n; i++) p[i] = black;
}

void vid_clear_both(void)
{
    int n = rmode->fbWidth * rmode->xfbHeight / 2;
    uint32_t black = 0x10801080;
    for (int b = 0; b < 2; b++) {
        uint32_t *p = (uint32_t *)xfb[b];
        for (int i = 0; i < n; i++) p[i] = black;
    }
}

int vid_refresh_hz(void)
{
    int fmt = rmode->viTVMode >> 2;
    /* VI_PAL (1) and VI_DEBUG_PAL (4) are 50 Hz; the rest run at ~60 Hz. */
    return (fmt == VI_PAL || fmt == VI_DEBUG_PAL) ? 50 : 60;
}

void vid_vsync(void) { VIDEO_WaitVSync(); }

/* --- chroma 4:2:0 -> RGB ---------------------------------------------- */
static inline int clamp255(int v){ return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Decode one pixel to RGB given luma Y and centered chroma U,V (-128).
 * moflex==0: YCgCo  R=Y+U-V, G=Y+V, B=Y-U-V
 * moflex==1: BT.601 limited-range YCbCr (matches reference Moflex3DS path) */
static inline void to_rgb(int moflex, int Y, int U, int V, int *r, int *g, int *b)
{
    if (!moflex) {
        *r = clamp255(Y + U - V);
        *g = clamp255(Y + V);
        *b = clamp255(Y - U - V);
    } else {
        int R = Y + ((363 * V) >> 8);
        int G = Y - ((88 * U) >> 8) - ((183 * V) >> 8);
        int B = Y + ((453 * U) >> 8);
        /* limited (16..235) -> full range: (x-16)*255/239 ~= (x-16)*273>>8 */
        R = ((R - 16) * 273) >> 8;
        G = ((G - 16) * 273) >> 8;
        B = ((B - 16) * 273) >> 8;
        *r = clamp255(R); *g = clamp255(G); *b = clamp255(B);
    }
}

/* Convert two adjacent source pixels (Y0,Y1 share chroma U,V) to one YUY2 word. */
static inline uint32_t pair_to_yuy2(int moflex, int y0, int y1, int u, int v)
{
    int r0,g0,b0,r1,g1,b1;
    to_rgb(moflex, y0, u, v, &r0,&g0,&b0);
    to_rgb(moflex, y1, u, v, &r1,&g1,&b1);
    int Y0 = (77*r0 + 150*g0 + 29*b0) >> 8;
    int Y1 = (77*r1 + 150*g1 + 29*b1) >> 8;
    int Cb = ((-43*(r0+r1) - 85*(g0+g1) + 128*(b0+b1)) >> 9) + 128;
    int Cr = ((128*(r0+r1) - 107*(g0+g1) - 21*(b0+b1)) >> 9) + 128;
    return (clamp255(Y0) << 24) | (clamp255(Cb) << 16) |
           (clamp255(Y1) << 8)  |  clamp255(Cr);
}

static void draw_frame(MoFrame *f, uint32_t *dst);

/* Moflex: a channel is clamp255(((Y + d - 16) * 273) >> 8), d coming from the chroma alone, so one table
 * over Y + d (-256 .. 511) gives what to_rgb gives, bit for bit. */
static uint8_t moflex_lut[768];
static int moflex_lut_ready;

static inline uint32_t moflex_pair(int y0, int y1, int dr, int dg, int db)
{
    const uint8_t *L = moflex_lut + 256;
    int r0 = L[y0 + dr], g0 = L[y0 + dg], b0 = L[y0 + db];
    int r1 = L[y1 + dr], g1 = L[y1 + dg], b1 = L[y1 + db];
    uint32_t Y0 = (uint32_t)(77 * r0 + 150 * g0 + 29 * b0) >> 8;
    uint32_t Y1 = (uint32_t)(77 * r1 + 150 * g1 + 29 * b1) >> 8;
    int r = r0 + r1, g = g0 + g1, b = b0 + b1;
    uint32_t Cb = (uint32_t)(((-43 * r - 85 * g + 128 * b) >> 9) + 128);
    uint32_t Cr = (uint32_t)(((128 * r - 107 * g - 21 * b) >> 9) + 128);
    return (Y0 << 24) | (Cb << 16) | (Y1 << 8) | Cr;
}

/* The framebuffers are mapped uncached, where every 4-byte store goes out to memory on its own:
 * draw through the cached mirror instead, then write the cache back once. */
void vid_draw_frame(MoFrame *f)
{
    uint32_t *dst = (uint32_t *)MEM_K1_TO_K0(xfb[cur]);
    draw_frame(f, dst);
    DCStoreRange(dst, rmode->fbWidth * rmode->xfbHeight * VI_DISPLAY_PIX_SZ);
}

static void draw_frame(MoFrame *f, uint32_t *dst)
{
    int SW = rmode->fbWidth;
    int SH = rmode->xfbHeight;
    uint8_t *Y = f->data[0], *U = f->data[1], *V = f->data[2];
    int ly = f->linesize[0], lc = f->linesize[1];

    /* YCgCo frames that fit the screen: two rows at a time, sharing their chroma. While no R, G or B
     * clips, pair_to_yuy2 reduces exactly (same bits) to
     *   Y' = (256Y + 48U + 44V) >> 8,  Cb = ((-342U - 340V) >> 9) + 128,  Cr = ((298U - 428V) >> 9) + 128;
     * a pixel whose RGB would clip takes the full conversion. */
    if (!f->moflex && f->width <= SW && f->height <= SH) {
        int w = f->width & ~1, h = f->height;
        int ox = (SW - w) / 2, oy = (SH - h) / 2;
        for (int y = 0; y < h; y += 2) {
            const uint8_t *yr0 = Y + y * ly;
            const uint8_t *yr1 = y + 1 < h ? yr0 + ly : NULL;
            const uint8_t *ur = U + (y >> 1) * lc, *vr = V + (y >> 1) * lc;
            uint32_t *d0 = dst + ((oy + y) * SW + ox) / 2;
            uint32_t *d1 = d0 + SW / 2;
            for (int c = 0; c < w / 2; c++) {
                int u = ur[c] - 128, v = vr[c] - 128;
                /* R = Y+u-v, G = Y+v, B = Y-u-v stay in 0..255 for Y in lo..hi */
                int a = u - v, b = -u - v;
                int mn = a < v ? a : v, mx = a > v ? a : v;
                if (b < mn) mn = b;
                if (b > mx) mx = b;
                unsigned lo = (unsigned)-mn, span = (unsigned)(255 - mx + mn);
                int dy = 48 * u + 44 * v;
                uint32_t cbcr = ((uint32_t)(((-342 * u - 340 * v) >> 9) + 128) << 16) |
                                (uint32_t)(((298 * u - 428 * v) >> 9) + 128);
                int p0 = yr0[2 * c], p1 = yr0[2 * c + 1];
                if (mx - mn <= 255 && (unsigned)(p0 - lo) <= span && (unsigned)(p1 - lo) <= span)
                    d0[c] = ((uint32_t)(((p0 << 8) + dy) >> 8) << 24) | ((uint32_t)(((p1 << 8) + dy) >> 8) << 8) | cbcr;
                else
                    d0[c] = pair_to_yuy2(0, p0, p1, u, v);
                if (!yr1) continue;
                p0 = yr1[2 * c], p1 = yr1[2 * c + 1];
                if (mx - mn <= 255 && (unsigned)(p0 - lo) <= span && (unsigned)(p1 - lo) <= span)
                    d1[c] = ((uint32_t)(((p0 << 8) + dy) >> 8) << 24) | ((uint32_t)(((p1 << 8) + dy) >> 8) << 8) | cbcr;
                else
                    d1[c] = pair_to_yuy2(0, p0, p1, u, v);
            }
        }
        return;
    }

    if (f->moflex && f->width <= SW && f->height <= SH) {
        if (!moflex_lut_ready) {
            for (int s = -256; s < 512; s++) moflex_lut[s + 256] = (uint8_t)clamp255(((s - 16) * 273) >> 8);
            moflex_lut_ready = 1;
        }
        int w = f->width & ~1, h = f->height;
        int ox = (SW - w) / 2, oy = (SH - h) / 2;
        for (int y = 0; y < h; y += 2) {
            const uint8_t *yr0 = Y + y * ly;
            const uint8_t *yr1 = y + 1 < h ? yr0 + ly : NULL;
            const uint8_t *ur = U + (y >> 1) * lc, *vr = V + (y >> 1) * lc;
            uint32_t *d0 = dst + ((oy + y) * SW + ox) / 2;
            uint32_t *d1 = d0 + SW / 2;
            for (int c = 0; c < w / 2; c++) {
                int u = ur[c] - 128, v = vr[c] - 128;
                int dr = (363 * v) >> 8, dg = -((88 * u) >> 8) - ((183 * v) >> 8), db = (453 * u) >> 8;
                d0[c] = moflex_pair(yr0[2 * c], yr0[2 * c + 1], dr, dg, db);
                if (yr1) d1[c] = moflex_pair(yr1[2 * c], yr1[2 * c + 1], dr, dg, db);
            }
        }
        return;
    }

    /* Fast path: frame fits the screen -> 1:1 centered, no scaling/divides. */
    if (f->width <= SW && f->height <= SH) {
        int w = f->width & ~1;
        int ox = (SW - w) / 2, oy = (SH - f->height) / 2;
        for (int y = 0; y < f->height; y++) {
            uint8_t *yr = Y + y * ly;
            uint8_t *ur = U + (y >> 1) * lc;
            uint8_t *vr = V + (y >> 1) * lc;
            uint32_t *drow = dst + ((oy + y) * SW + ox) / 2;
            for (int x = 0; x < w; x += 2) {
                int c = x >> 1;
                drow[c] = pair_to_yuy2(f->moflex, yr[x], yr[x+1],
                                       ur[c] - 128, vr[c] - 128);
            }
        }
        return;
    }

    /* Slow path: aspect-fit scaling (frame larger than the screen). */
    int dw = SW, dh = (int)((long)SW * f->height / f->width);
    if (dh > SH) { dh = SH; dw = (int)((long)SH * f->width / f->height); }
    dw &= ~1;
    int ox = (SW - dw) / 2, oy = (SH - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * f->height / dh);
        uint8_t *yr = Y + sy * ly;
        uint8_t *ur = U + (sy >> 1) * lc;
        uint8_t *vr = V + (sy >> 1) * lc;
        uint32_t *drow = dst + ((oy + y) * SW + ox) / 2;
        for (int x = 0; x < dw; x += 2) {
            int sx0 = (int)((long)x * f->width / dw);
            int sx1 = (int)((long)(x + 1) * f->width / dw);
            int y0 = yr[sx0], y1 = yr[sx1];
            int u = ur[sx0 >> 1] - 128, v = vr[sx0 >> 1] - 128;
            drow[x >> 1] = pair_to_yuy2(f->moflex, y0, y1, u, v);
        }
    }
}

void vid_draw_rgb24(const uint8_t *rgb, int w, int h)
{
    uint32_t *dst = (uint32_t *)MEM_K1_TO_K0(xfb[cur]);      /* cached, as in vid_draw_frame */
    int SW = rmode->fbWidth;
    int SH = rmode->xfbHeight;

    int dw = SW, dh = (int)((long)SW * h / w);
    if (dh > SH) { dh = SH; dw = (int)((long)SH * w / h); }
    dw &= ~1;
    int ox = (SW - dw) / 2;
    int oy = (SH - dh) / 2;

    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * h / dh);
        const uint8_t *srow = rgb + sy * w * 3;
        uint32_t *drow = dst + ((oy + y) * SW + ox) / 2;

        for (int x = 0; x < dw; x += 2) {
            int sx0 = (int)((long)x * w / dw);
            int sx1 = (int)((long)(x + 1) * w / dw);
            const uint8_t *p0 = srow + sx0 * 3;
            const uint8_t *p1 = srow + sx1 * 3;
            int r0 = p0[0], g0 = p0[1], b0 = p0[2];
            int r1 = p1[0], g1 = p1[1], b1 = p1[2];

            int Y0 = (77*r0 + 150*g0 + 29*b0) >> 8;
            int Y1 = (77*r1 + 150*g1 + 29*b1) >> 8;
            int Cb = ((-43*(r0+r1) - 85*(g0+g1) + 128*(b0+b1)) >> 9) + 128;
            int Cr = ((128*(r0+r1) - 107*(g0+g1) - 21*(b0+b1)) >> 9) + 128;

            drow[x >> 1] = (clamp255(Y0) << 24) | (clamp255(Cb) << 16) |
                           (clamp255(Y1) << 8)  |  clamp255(Cr);
        }
    }
    DCStoreRange(dst, SW * SH * VI_DISPLAY_PIX_SZ);
}

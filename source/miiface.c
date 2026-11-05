/*
 * Miis drawn by MiiKit. See miiface.h.
 *
 * Each Mii is drawn as a portrait (mii_capture_bust), copied out of the GPU as RGBA, and kept as
 * YUY2, so putting it on the console's framebuffer is a plain copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "miiface.h"
#include "miikit/miidraw.h"
#include "media.h"
#include "ui.h"
#include "vfs.h"

#define CACHE_MAX 64
#define MASK_SIZE 256                   /* the face texture: sharp at this size */

typedef struct {
    u8 rec[MII_SIZE];
    int expression;
    u32 *yuy2;                          /* a word per pixel pair; NULL: this Mii can't be drawn */
} entry_t;

static mii_res res;
static mii_body male, female;
static bool have_res, have_bodies;
static entry_t *cache;
static int ncache;
static bool dolphin_blank;

static const char *const EXPRESSIONS[MIIFACE_EXPRESSIONS] = {
    "Normal", "Smile", "Angry", "Sad", "Surprised", "Blink", "Open mouth"
};

const char *miiface_expression_name(int expression)
{
    return expression >= 0 && expression < MIIFACE_EXPRESSIONS ? EXPRESSIONS[expression] : "?";
}

bool miiface_dolphin_blank(void) { return dolphin_blank; }

bool miiface_open(char *err, size_t errsz)
{
    err[0] = '\0';
    if (have_res) return true;
    int nand = mii_res_load_nand(&res);
    int sd = nand < 0 ? mii_res_load(&res, NULL) : MII_OK;
    have_res = sd == MII_OK;
    /* without the Wii's bodies, MiiKit builds a plainer one of its own */
    have_bodies = mii_bodies_load_nand(&male, &female) == MII_OK;
    if (!have_bodies && mii_body_load(&male, MII_BODY_MALE_PATH) == MII_OK) {
        have_bodies = mii_body_load(&female, MII_BODY_FEMALE_PATH) == MII_OK;
        if (!have_bodies) mii_body_free(&male);
    }
    if (!have_res)
        snprintf(err, errsz, "Couldn't read the Mii face models (RFL_Res.dat) from the Wii Menu (error %d) "
                 "or from " MII_RES_PATH " (error %d), so the Miis are shown without pictures.", nand, sd);
    return have_res;
}

void miiface_close(void)
{
    for (int i = 0; i < ncache; i++) free(cache[i].yuy2);
    free(cache);
    cache = NULL;
    ncache = 0;
    if (have_res) mii_res_free(&res);
    if (have_bodies) {
        mii_body_free(&male);
        mii_body_free(&female);
    }
    have_res = have_bodies = false;
}

static void yuv(int r, int g, int b, int *y, int *u, int *v)
{
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

/* GX_TF_RGBA8 (4x4 tiles: 16 AR pairs, then 16 GB pairs) to YUY2. Where there's no Mii the
 * copy is clear black, and the edges' colors are already mixed with it, so alpha is dropped. */
static u32 *tiles_to_yuy2(const u8 *tex)
{
    const int w = MIIFACE_WIDTH, h = MIIFACE_HEIGHT;
    u32 *out = malloc(w * h * 2);
    if (!out) return NULL;
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            const u8 *t = tex + ((by / 4) * (w / 4) + bx / 4) * 64;
            for (int i = 0; i < 16; i += 2) {
                int yy[2], uu = 0, vv = 0;
                for (int k = 0; k < 2; k++) {
                    int u, v;
                    yuv(t[(i + k) * 2 + 1], t[32 + (i + k) * 2], t[32 + (i + k) * 2 + 1], &yy[k], &u, &v);
                    uu += u;
                    vv += v;
                }
                int x = bx + (i & 3), y = by + (i >> 2);
                out[(y * w + x) / 2] = (u32)yy[0] << 24 | (u32)(uu / 2) << 16 | (u32)yy[1] << 8 | (u32)(vv / 2);
            }
        }
    }
    return out;
}

/* True when some pixel isn't clear: the copy reached memory. */
static bool drawn(const u8 *tex)
{
    for (int i = 0; i < MIIFACE_WIDTH * MIIFACE_HEIGHT * 4; i += 64)
        for (int k = 0; k < 32; k += 2)
            if (tex[i + k]) return true;
    return false;
}

/* The Mii by the GPU, or NULL. */
static u32 *render(const u8 *rec, int expression)
{
    static const GXColor black = { 0, 0, 0, 255 };
    const u32 size = MIIFACE_WIDTH * MIIFACE_HEIGHT * 4;
    mii m;
    mii_model model;
    mii_pose pose = { 0 };
    mii_unpack(rec, &m);
    mii_model_opts o = { MASK_SIZE, MII_EXPR_BIT(expression), NULL, 0, 1 };
    model_gx_init();
    if (mii_model_build(&res, &m, &o, &model) != MII_OK) return NULL;
    if (have_bodies) {
        const mii_body *body = m.sex == MII_FEMALE ? &female : &male;
        int wait = mii_body_anim(body, "bodyWait");  /* standing as in the Mii Channel, arms down */
        mii_model_set_body(&model, body);
        if (wait >= 0) pose.anim = 1 + wait;
    }
    pose.expression = expression;
    u32 *yuy2 = NULL;
    u8 *tex = memalign(32, size);
    if (tex && (!model.faces_pending || mii_model_make_faces(&model, &res, black) == MII_OK) &&
        mii_capture_bust(&model, &pose, MIIFACE_WIDTH, MIIFACE_HEIGHT, black, tex) == MII_OK) {
        GX_DrawDone();
        DCInvalidateRange(tex, size);
        if (drawn(tex)) yuy2 = tiles_to_yuy2(tex);
        else if (g_is_dolphin) dolphin_blank = true;
    }
    GX_DrawDone();
    mii_model_free(&model);
    free(tex);
    return yuy2;
}

static const entry_t *face(const u8 *rec, int expression)
{
    for (int i = 0; i < ncache; i++)
        if (cache[i].expression == expression && !memcmp(cache[i].rec, rec, MII_SIZE)) return &cache[i];
    if (!cache && !(cache = calloc(CACHE_MAX, sizeof(*cache)))) return NULL;
    if (ncache == CACHE_MAX) {                      /* full: the oldest goes */
        free(cache[0].yuy2);
        memmove(cache, cache + 1, (CACHE_MAX - 1) * sizeof(*cache));
        ncache--;
    }
    entry_t *e = &cache[ncache++];
    memcpy(e->rec, rec, MII_SIZE);
    e->expression = expression;
    e->yuy2 = render(rec, expression);
    return e;
}

bool miiface_draw(const u8 *rec, int expression, int x, int y)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    const entry_t *e = have_res ? face(rec, expression) : NULL;
    const u32 *src = e ? e->yuy2 : NULL;
    if (!fb) return false;
    x &= ~1;                                        /* YUY2: pixels go out in pairs */
    for (int dy = 0; dy < MIIFACE_HEIGHT; dy++) {
        int sy = y + dy;
        if (sy < 0 || sy >= fh) continue;
        /* the framebuffer is uncached: whole 32-bit words only */
        u32 *row = (u32 *)(fb + sy * fw * 2) + x / 2;
        for (int i = 0; i < MIIFACE_WIDTH / 2; i++) {
            if (x + i * 2 < 0 || x + i * 2 + 1 >= fw) continue;
            row[i] = src ? src[dy * MIIFACE_WIDTH / 2 + i] : 0x10801080;
        }
    }
    return src != NULL;
}

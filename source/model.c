/*
 * 3D model viewer: NW4R MDL0 (in .brres) and J3D BMD / BDL, drawn with GX.
 *
 * Layouts match quatric's nintoolbox (lib-brres-model.c, lib-j3d.c):
 *   MDL0  bones carry their world matrix; vertex arrays (VTX0 / NRM0 / CLR0)
 *         are indexed from each object's GX display list, and LOAD_INDX_A
 *         commands map matrix slots to nodes
 *   J3D   INF1 hierarchy + JNT1 joints give world matrices; SHP1 packets carry
 *         display lists whose matrix slots go through DRW1
 * Vertices bound to a single bone / joint are moved into place with its world
 * matrix; envelope-weighted ones are already in bind-pose model space.
 * Each mesh shows the first texture layer of its material (MDL0 materials name
 * a TEX0 in the BRRES's Textures(NW4R) folder, with its PLT0 for palette
 * formats; J3D materials index TEX1), mapped with UV set 0 and lit with GX
 * hardware lighting. View only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "pointer.h"
#include "input.h"
#include <input_wii.h>
#include "archive.h"
#include "excite.h"

#define FILE_MAX (32u * 1024 * 1024)
#define MAX_VERTS (2u * 1024 * 1024)

#define MAX_TEX 256

typedef struct {
    float p[3], n[3], t[2];
    u8 c[4];
} vert_t;

/* pe: the material's blend mode (GX PE_CMODE0: bit 0 blend, bits 5-7 destination factor, 8-10
 * source factor, 11 subtract), with bit 31 set for translucent materials, drawn after the rest. */
typedef struct {
    u32 start, count;
    int tex;
    u32 pe;
} mesh_t;
#define PE_XLU   0x80000000u
#define PE_BLEND 0x00000001u

typedef struct {
    GXTexObj obj;
    /* converted / realigned copy, or NULL when it points into the file */
    void *own;
    u32 key;                    /* where it came from in the file, to share it between meshes */
    bool cutout;                /* has real alpha (I4 / I8 repeat their intensity as alpha on GX) */
} tex_t;

typedef struct {
    vert_t *v;
    u32 n, cap;
    float min[3], max[3];
    int meshes, bones;
    char name[64];
    const char *kind;
    mesh_t *mesh;
    int nmesh, capmesh;
    tex_t tex[MAX_TEX];
    int ntex, textured;
} model_t;

static float bef(const u8 *p)
{
    u32 b = be32(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static bool push(model_t *m, const vert_t *v)
{
    if (m->n == m->cap) {
        u32 nc = m->cap ? m->cap * 2 : 4096;
        if (nc > MAX_VERTS) return false;
        vert_t *nv = realloc(m->v, nc * sizeof(vert_t));
        if (!nv) return false;
        m->v = nv;
        m->cap = nc;
    }
    m->v[m->n++] = *v;
    for (int k = 0; k < 3; k++) {
        if (v->p[k] < m->min[k]) m->min[k] = v->p[k];
        if (v->p[k] > m->max[k]) m->max[k] = v->p[k];
    }
    return true;
}

/* 3x4 row-major affine matrices */
static void xform(const float *M, const float *in, float *out)
{
    for (int r = 0; r < 3; r++)
        out[r] = M[r * 4] * in[0] + M[r * 4 + 1] * in[1] + M[r * 4 + 2] * in[2] + M[r * 4 + 3];
}
static void xform_n(const float *M, const float *in, float *out)
{
    for (int r = 0; r < 3; r++)
        out[r] = M[r * 4] * in[0] + M[r * 4 + 1] * in[1] + M[r * 4 + 2] * in[2];
    float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 1e-6f) for (int k = 0; k < 3; k++) out[k] /= l;
}
static void mul34(const float *a, const float *b, float *o)
{
    float t[12];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] +
                           (c == 3 ? a[r * 4 + 3] : 0);
    memcpy(o, t, sizeof(t));
}

/* ---------------------------------------------------------------- meshes and textures */

static void add_mesh(model_t *m, u32 start, int tex, u32 pe)
{
    if (m->n <= start) return;
    if (m->nmesh == m->capmesh) {
        int nc = m->capmesh ? m->capmesh * 2 : 64;
        mesh_t *nm = realloc(m->mesh, nc * sizeof(mesh_t));
        if (!nm) return;
        m->mesh = nm;
        m->capmesh = nc;
    }
    m->mesh[m->nmesh++] = (mesh_t){ start, m->n - start, tex, pe };
    if (tex >= 0) m->textured++;
}

static bool is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

/* A GX texture from data in the file. GX formats other than the palette ones are used as they are;
 * palette textures are decoded and handed to GX as RGBA8. Returns the index into m->tex, or -1. */
static int add_texture(model_t *m, const u8 *file, u32 flen, u32 at, int fmt, int w, int h,
                       const u8 *pal, int palfmt, int palcount, int wrap_s, int wrap_t, u32 keybase)
{
    u32 key = at | keybase;
    for (int i = 0; i < m->ntex; i++)
        if (m->tex[i].key == key) return i;
    if (m->ntex >= MAX_TEX || w <= 0 || h <= 0 || w > 1024 || h > 1024 || at >= flen) return -1;
    if (!((fmt >= 0 && fmt <= 6) || (fmt >= 8 && fmt <= 10) || fmt == 14)) return -1;

    u32 need = image_gx_size(fmt, w, h);
    if (need > flen - at) return -1;

    tex_t *t = &m->tex[m->ntex];
    memset(t, 0, sizeof(*t));

    void *pixels;
    if (fmt >= 8 && fmt <= 10) {
        u8 *rgba = image_decode_gx(file + at, flen - at, fmt, w, h, pal, palfmt, palcount);
        if (!rgba) return -1;
        int tw = (w + 3) & ~3, th = (h + 3) & ~3;
        u8 *gx = memalign(32, tw * th * 4);
        if (!gx) {
            free(rgba);
            return -1;
        }

        /* RGBA8 tiles: 4x4 pixels, 32 bytes of AR then 32 bytes of GB */
        u8 *o = gx;
        for (int by = 0; by < th; by += 4)
            for (int bx = 0; bx < tw; bx += 4, o += 64)
                for (int i = 0; i < 16; i++) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    const u8 *c = x < w && y < h ? rgba + (y * w + x) * 4 : (const u8 *)"\0\0\0\0";
                    o[i * 2] = c[3];
                    o[i * 2 + 1] = c[0];
                    o[32 + i * 2] = c[1];
                    o[32 + i * 2 + 1] = c[2];
                }
        free(rgba);

        t->own = gx;
        pixels = gx;
        need = tw * th * 4;
        fmt = GX_TF_RGBA8;
    } else if (((uintptr_t)(file + at)) & 31) {
        t->own = memalign(32, need);
        if (!t->own) return -1;
        memcpy(t->own, file + at, need);
        pixels = t->own;
    } else
        pixels = (void *)(file + at);

    /* repeat and mirror need power-of-two sizes on GX */
    if (!is_pow2(w)) wrap_s = GX_CLAMP;
    if (!is_pow2(h)) wrap_t = GX_CLAMP;
    if (wrap_s < 0 || wrap_s > 2) wrap_s = GX_REPEAT;
    if (wrap_t < 0 || wrap_t > 2) wrap_t = GX_REPEAT;

    DCFlushRange(pixels, need);
    GX_InitTexObj(&t->obj, pixels, w, h, fmt, wrap_s, wrap_t, GX_FALSE);
    GX_InitTexObjLOD(&t->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    t->key = key;
    t->cutout = fmt != GX_TF_I4 && fmt != GX_TF_I8 && fmt != GX_TF_RGB565;
    return m->ntex++;
}

/* A decoded RGBA picture as a GX RGBA8 texture (repeating where the size allows); key tells textures
 * apart for sharing. Returns the index into m->tex, or -1. */
static int add_rgba_texture(model_t *m, const u8 *rgba, int w, int h, u32 key)
{
    for (int i = 0; i < m->ntex; i++)
        if (m->tex[i].key == key) return i;
    if (m->ntex >= MAX_TEX || w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    int tw = (w + 3) & ~3, th = (h + 3) & ~3;
    u8 *gx = memalign(32, tw * th * 4);
    if (!gx) return -1;
    u8 *o = gx;
    for (int by = 0; by < th; by += 4)                 /* 4x4 tiles: 32 bytes of AR, then 32 of GB */
        for (int bx = 0; bx < tw; bx += 4, o += 64)
            for (int i = 0; i < 16; i++) {
                int x = bx + (i & 3), y = by + (i >> 2);
                const u8 *c = x < w && y < h ? rgba + (y * w + x) * 4 : (const u8 *)"\0\0\0\0";
                o[i * 2] = c[3];
                o[i * 2 + 1] = c[0];
                o[32 + i * 2] = c[1];
                o[32 + i * 2 + 1] = c[2];
            }
    tex_t *t = &m->tex[m->ntex];
    memset(t, 0, sizeof(*t));
    t->own = gx;
    DCFlushRange(gx, tw * th * 4);
    GX_InitTexObj(&t->obj, gx, w, h, GX_TF_RGBA8, is_pow2(w) ? GX_REPEAT : GX_CLAMP,
                  is_pow2(h) ? GX_REPEAT : GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&t->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    t->key = key;
    t->cutout = true;
    return m->ntex++;
}

static void free_model(model_t *m)
{
    for (int i = 0; i < m->ntex; i++) free(m->tex[i].own);
    free(m->v);
    free(m->mesh);
    memset(m, 0, sizeof(*m));
}

static const u8 PALETTE[8][3] = {
    { 220, 200, 170 }, { 170, 200, 230 }, { 200, 230, 170 }, { 230, 180, 200 },
    { 230, 220, 150 }, { 180, 170, 230 }, { 160, 220, 210 }, { 230, 190, 150 },
};

/* ---------------------------------------------------------------- vertex arrays */

typedef struct {
    const u8 *base;             /* first element */
    u32 count, stride;
    int type, comps, frac;      /* type: 0 u8, 1 s8, 2 u16, 3 s16, 4 f32 */
} arr_t;

static float comp(const arr_t *a, u32 i, int k)
{
    const u8 *p = a->base + i * a->stride;
    int sz = a->type < 2 ? 1 : a->type < 4 ? 2 : 4;
    p += k * sz;
    float d = (float)(1 << (a->frac & 31));
    switch (a->type) {
    case 0: return p[0] / d;
    case 1: return (s8)p[0] / d;
    case 2: return be16(p) / d;
    case 3: return (s16)be16(p) / d;
    default: return bef(p);
    }
}

static void get_vec(const arr_t *a, u32 i, float *o)
{
    o[0] = o[1] = o[2] = 0;
    if (!a->base || i >= a->count) return;
    for (int k = 0; k < a->comps && k < 3; k++) o[k] = comp(a, i, k);
}

/* CLR0 / J3D color formats: 0 RGB565, 1 RGB8, 2 RGBX8, 3 RGBA4, 4 RGBA6, 5 RGBA8 */
static void get_color(const u8 *base, u32 count, u32 stride, int fmt, u32 i, u8 *o)
{
    if (!base || i >= count) return;
    const u8 *p = base + i * stride;
    switch (fmt) {
    case 0: {
        u16 v = be16(p);
        o[0] = ((v >> 11) & 31) * 255 / 31;
        o[1] = ((v >> 5) & 63) * 255 / 63;
        o[2] = (v & 31) * 255 / 31;
        o[3] = 255;
        break;
    }
    case 1:
    case 2:
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = 255;
        break;
    case 3: {
        u16 v = be16(p);
        o[0] = (v >> 12) * 17;
        o[1] = ((v >> 8) & 15) * 17;
        o[2] = ((v >> 4) & 15) * 17;
        o[3] = (v & 15) * 17;
        break;
    }
    case 4: {
        u32 v = p[0] << 16 | p[1] << 8 | p[2];
        o[0] = ((v >> 18) & 63) * 255 / 63;
        o[1] = ((v >> 12) & 63) * 255 / 63;
        o[2] = ((v >> 6) & 63) * 255 / 63;
        o[3] = (v & 63) * 255 / 63;
        break;
    }
    default:
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = p[3];
        break;
    }
}

/* ---------------------------------------------------------------- primitives */

typedef struct {
    u32 pos, nrm, clr, uv;
    int mtx;                    /* matrix slot node, -1 = none */
} pv_t;

typedef struct {
    model_t *m;
    const arr_t *pos, *nrm, *uv;
    const u8 *clr;
    u32 clr_n, clr_stride;
    int clr_fmt;
    const float *(*matrix)(void *ctx, int node);   /* node -> world matrix or NULL */
    void *ctx;
    int default_node;
    u8 tint[3];
} emit_t;

static void make_vert(emit_t *e, const pv_t *p, vert_t *v)
{
    float pos[3], nrm[3] = { 0, 1, 0 };
    get_vec(e->pos, p->pos, pos);
    if (e->nrm && e->nrm->base) get_vec(e->nrm, p->nrm, nrm);
    int node = p->mtx >= 0 ? p->mtx : e->default_node;
    const float *M = node >= 0 && e->matrix ? e->matrix(e->ctx, node) : NULL;
    if (M) {
        xform(M, pos, v->p);
        xform_n(M, nrm, v->n);
    } else {
        memcpy(v->p, pos, 12);
        memcpy(v->n, nrm, 12);
    }
    v->t[0] = v->t[1] = 0;
    if (e->uv && e->uv->base && p->uv < e->uv->count) {
        v->t[0] = comp(e->uv, p->uv, 0);
        if (e->uv->comps > 1) v->t[1] = comp(e->uv, p->uv, 1);
    }
    v->c[0] = e->tint[0];
    v->c[1] = e->tint[1];
    v->c[2] = e->tint[2];
    v->c[3] = 255;
    if (e->clr) get_color(e->clr, e->clr_n, e->clr_stride, e->clr_fmt, p->clr, v->c);
}

static void tri(emit_t *e, const pv_t *a, const pv_t *b, const pv_t *c)
{
    vert_t v[3];
    make_vert(e, a, &v[0]);
    make_vert(e, b, &v[1]);
    make_vert(e, c, &v[2]);
    if (push(e->m, &v[0]) && push(e->m, &v[1]))
        push(e->m, &v[2]);
}

/* GX primitives -> triangles (winding reversed, as nintoolbox does). */
static void emit_prim(emit_t *e, int prim, const pv_t *p, u32 n)
{
    if (prim == 0x90) for (u32 i = 0; i + 2 < n; i += 3) tri(e, &p[i + 2], &p[i + 1], &p[i]);
    else if (prim == 0x80)
        for (u32 i = 0; i + 3 < n; i += 4) {
            tri(e, &p[i], &p[i + 2], &p[i + 1]);
            tri(e, &p[i], &p[i + 3], &p[i + 2]);
        }
    else if (prim == 0x98)
        for (u32 i = 2; i < n; i++) {
            if (i & 1)
                tri(e, &p[i], &p[i - 2], &p[i - 1]);
            else
                tri(e, &p[i], &p[i - 1], &p[i - 2]);
        }
    else if (prim == 0xA0) for (u32 i = 2; i < n; i++) tri(e, &p[0], &p[i], &p[i - 1]);
}

/* ---------------------------------------------------------------- MDL0 */

typedef struct {
    const u8 *b;
    u32 len, ver;
    /* the BRRES around it: names and textures live outside the MDL0 */
    const u8 *file;
    u32 flen;
    float (*mtx)[12];           /* by node id */
    u8 *has;
    int nmtx;
} mdl_t;

static const float *mdl_matrix(void *ctx, int node)
{
    mdl_t *d = ctx;
    return node >= 0 && node < d->nmtx && d->has[node] ? d->mtx[node] : NULL;
}

static u32 mdl_group(const mdl_t *d, int idx)
{
    u32 f = 0x10 + idx * 4;
    if (f + 4 > d->len) return 0;
    s32 o = (s32)be32(d->b + f);
    return o > 0 && (u32)o + 8 <= d->len ? (u32)o : 0;
}

/* entry `id` (0-based) of a resource group -> absolute offset */
static u32 grp_item(const mdl_t *d, u32 grp, int id, u32 need)
{
    if (!grp || id < 0) return 0;
    u32 cnt = be32(d->b + grp + 4);
    if ((u32)id >= cnt || grp + 8 + (cnt + 1) * 16 > d->len) return 0;
    s32 o = (s32)be32(d->b + grp + 8 + (id + 1) * 16 + 12);
    u32 at = grp + o;
    return o > 0 && at + need <= d->len ? at : 0;
}

enum { ARR_POS, ARR_NRM, ARR_UV };

static void mdl_array(const mdl_t *d, u32 at, int kind, arr_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!at) return;
    s32 doff = (s32)be32(d->b + at + 8);
    u32 flag = be32(d->b + at + 20);
    a->comps = kind == ARR_NRM ? 3 : kind == ARR_UV ? (flag ? 2 : 1) : (flag ? 3 : 2);
    a->type = be32(d->b + at + 24);
    a->frac = d->b[at + 28];
    a->stride = d->b[at + 29];
    a->count = be16(d->b + at + 30);
    if (doff <= 0 || a->type > 4 || at + doff + (u64)a->stride * a->count > d->len) {
        a->base = NULL;
        return;
    }
    a->base = d->b + at + doff;
}

static void mdl_object(mdl_t *d, emit_t *e, u32 o)
{
    u32 lo = be32(d->b + o + 0x0C), hi = be32(d->b + o + 0x10);
    s32 psz = (s32)be32(d->b + o + 0x28), poff = (s32)be32(d->b + o + 0x2C);
    u32 dl = o + 0x24 + poff;
    if (psz <= 0 || poff <= 0 || dl + psz > d->len) return;
    int pos_t = lo >> 9 & 3, nrm_t = lo >> 11 & 3, c0_t = lo >> 13 & 3, c1_t = lo >> 15 & 3;
    if (pos_t < 2) return;                          /* direct data: not handled */
    int isz[4] = { 0, 0, 1, 2 };
    if (nrm_t == 1 || c0_t == 1 || c1_t == 1) return;
    for (int t = 0; t < 8; t++) if ((hi >> (t * 2) & 3) == 1) return;
    int stride = (lo & 1);
    for (int i = 0; i < 8; i++) stride += lo >> (i + 1) & 1;
    stride += isz[pos_t] + isz[nrm_t] + isz[c0_t] + isz[c1_t];
    for (int t = 0; t < 8; t++) stride += isz[hi >> (t * 2) & 3];

    int slots[16];
    for (int i = 0; i < 16; i++) slots[i] = -1;

    const u8 *p = d->b + dl, *end = p + psz;
    pv_t *pts = NULL;
    u32 cap = 0;

    while (p < end) {
        u8 cmd = *p++;
        if (!cmd) continue;                         /* NOP padding */
        /* LOAD_CP */
        if (cmd == 0x08) {
            p += 5;
            continue;
        }
        /* LOAD_XF */
        if (cmd == 0x10) {
            if (p + 4 > end) break;
            u32 n = be16(p) + 1;
            p += 4 + n * 4;
            continue;
        }
        if (cmd >= 0x20 && cmd <= 0x38 && !(cmd & 7)) {
            if (p + 4 > end) break;
            if (cmd == 0x20) {
                u32 slot = (be16(p + 2) & 0xFFF) / 12;
                if (slot < 16) slots[slot] = be16(p);
            }
            p += 4;
            continue;
        }
        if (cmd == 0x48) continue;

        u8 prim = cmd & 0xF8;
        if (prim < 0x80 || prim > 0xB8 || p + 2 > end) break;

        u32 n = be16(p);
        p += 2;
        if ((u64)n * stride > (u64)(end - p)) break;

        if (n > cap) {
            pv_t *np = realloc(pts, n * sizeof(pv_t));
            if (!np) break;
            pts = np;
            cap = n;
        }

        for (u32 i = 0; i < n; i++) {
            const u8 *q = p + i * stride;
            pv_t *v = &pts[i];
            v->mtx = -1;
            if (lo & 1) {
                u32 s = *q++ / 3;
                v->mtx = s < 16 ? slots[s] : -1;
            }
            for (int k = 0; k < 8; k++) q += lo >> (k + 1) & 1;
            v->pos = pos_t == 2 ? *q : be16(q);
            q += isz[pos_t];
            v->nrm = nrm_t == 2 ? *q : nrm_t == 3 ? be16(q) : 0;
            q += isz[nrm_t];
            v->clr = c0_t == 2 ? *q : c0_t == 3 ? be16(q) : 0;
            q += isz[c0_t];
            q += isz[c1_t];
            int t0 = hi & 3;
            v->uv = t0 == 2 ? *q : t0 == 3 ? be16(q) : 0;
        }

        p += n * stride;
        emit_prim(e, prim, pts, n);
    }

    free(pts);
}

/* A BRRES resource by folder and name ("Textures(NW4R)", "Palettes(NW4R)"): its offset in the
 * file, or 0. */
static u32 brres_find(const u8 *f, u32 flen, const char *folder, const char *name, u32 nlen)
{
    if (flen < 0x10 || memcmp(f, "bres", 4)) return 0;
    u32 root = be16(f + 0x0C);
    if (root + 16 > flen || memcmp(f + root, "root", 4)) return 0;
    u32 g = root + 8, n = be32(f + g + 4);
    for (u32 i = 1; i <= n && g + 8 + (i + 1) * 16 <= flen; i++) {
        const u8 *e = f + g + 8 + i * 16;
        u32 fn = g + be32(e + 8), sub = g + be32(e + 12);
        if (fn >= flen || sub + 8 > flen || strncmp((const char *)f + fn, folder, flen - fn))
            continue;
        u32 sn = be32(f + sub + 4);
        for (u32 k = 1; k <= sn && sub + 8 + (k + 1) * 16 <= flen; k++) {
            const u8 *se = f + sub + 8 + k * 16;
            u32 no = sub + be32(se + 8), at = sub + be32(se + 12);
            if (no + nlen < flen && !memcmp(f + no, name, nlen) && !f[no + nlen] &&
                at + 0x40 <= flen)
                return at;
        }
    }
    return 0;
}

/* Textures that are not in the MDL0's own BRRES: a bare .mdl0 keeps them in files next to it, either
 * "<texture>.tex0" (+ ".plt0") or inside some .brres of the same folder. The loaded files stay in memory
 * while the model is open. */
#define EXT_MAX 16
#define EXT_FILE_MAX (12u * 1024 * 1024)
typedef struct { u8 *buf; u32 len; char path[VFS_PATH_MAX]; } extbuf_t;
static extbuf_t g_ext[EXT_MAX];
static int g_next;
static char g_ext_dir[VFS_PATH_MAX];
static u8 *load_file(const char *path, u32 *len);

static void ext_reset(const char *dir)
{
    for (int i = 0; i < g_next; i++) free(g_ext[i].buf);
    g_next = 0;
    snprintf(g_ext_dir, sizeof(g_ext_dir), "%s", dir ? dir : "");
}

/* index into g_ext, or -1 (the file is loaded once; a failed load is remembered with buf == NULL) */
static int ext_get(const char *path)
{
    for (int i = 0; i < g_next; i++)
        if (!strcmp(g_ext[i].path, path)) return g_ext[i].buf ? i : -1;
    if (g_next >= EXT_MAX) return -1;
    bool st_dir = false; u64 sz = 0;
    u8 *b = NULL; u32 n = 0;
    if (vfs_stat(path, &st_dir, &sz) && !st_dir && sz <= EXT_FILE_MAX) b = load_file(path, &n);
    extbuf_t *e = &g_ext[g_next];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->buf = b;
    e->len = n;
    g_next++;
    return b ? g_next - 1 : -1;
}

static bool name_is(const char *a, const char *want, u32 wlen, const char *ext)
{
    size_t al = strlen(a), el = strlen(ext);
    return al == wlen + el && !strncasecmp(a, want, wlen) && !strcasecmp(a + wlen, ext);
}

/* Looks for the TEX0 called `name` outside the model's own file. On success: the buffer holding it, its
 * offset in there, a palette (pointer into some buffer, or NULL), and a number that tells buffers apart. */
static bool ext_texture(const char *name, u32 nlen, const u8 **buf, u32 *blen, u32 *tex, const u8 **pal, u32 *pallen, int *id)
{
    *pal = NULL; *pallen = 0;
    if (!g_ext_dir[0]) return false;
    vfs_entry *list; int n;
    if (!vfs_list(g_ext_dir, &list, &n)) return false;
    bool found = false;
    char p[VFS_PATH_MAX];
    /* 1: <name>.tex0 */
    for (int i = 0; i < n && !found; i++) {
        if (list[i].is_dir || !name_is(list[i].name, name, nlen, ".tex0")) continue;
        if (!vfs_join(p, sizeof(p), g_ext_dir, list[i].name)) continue;
        int k = ext_get(p);
        if (k < 0 || g_ext[k].len < 0x40 || memcmp(g_ext[k].buf, "TEX0", 4)) continue;
        *buf = g_ext[k].buf; *blen = g_ext[k].len; *tex = 0; *id = k + 1; found = true;
        for (int j = 0; j < n; j++) {
            if (list[j].is_dir || !name_is(list[j].name, name, nlen, ".plt0") || !vfs_join(p, sizeof(p), g_ext_dir, list[j].name)) continue;
            int pk = ext_get(p);
            if (pk >= 0 && g_ext[pk].len >= 0x20 && !memcmp(g_ext[pk].buf, "PLT0", 4)) {
                *pal = g_ext[pk].buf; *pallen = g_ext[pk].len;
            }
            break;
        }
    }
    /* 2: a BRRES in the folder that has it in "Textures(NW4R)" */
    int tried = 0;
    for (int i = 0; i < n && !found && tried < 12; i++) {
        if (list[i].is_dir || !arc_name_has_ext(list[i].name, ".brres") || list[i].size > EXT_FILE_MAX || !vfs_join(p, sizeof(p), g_ext_dir, list[i].name)) continue;
        tried++;
        int k = ext_get(p);
        if (k < 0 || g_ext[k].len < 0x20 || memcmp(g_ext[k].buf, "bres", 4)) continue;
        u32 t = brres_find(g_ext[k].buf, g_ext[k].len, "Textures(NW4R)", name, nlen);
        if (!t || memcmp(g_ext[k].buf + t, "TEX0", 4)) continue;
        *buf = g_ext[k].buf; *blen = g_ext[k].len; *tex = t; *id = k + 1; found = true;
        u32 pl = brres_find(g_ext[k].buf, g_ext[k].len, "Palettes(NW4R)", name, nlen);
        if (pl && !memcmp(g_ext[k].buf + pl, "PLT0", 4)) { *pal = g_ext[k].buf + pl; *pallen = g_ext[k].len - pl; }
    }
    free(list);
    return found;
}

/* Texture layer 0 of an MDL0 material -> m->tex index, or -1. */
static int mdl_material_texture(mdl_t *d, model_t *m, u32 mat)
{
    const u8 *f = d->file;
    if (!mat || mat + 0x34 > d->len) return -1;
    u32 count = be32(d->b + mat + 0x2C);
    s32 refs = (s32)be32(d->b + mat + 0x30);
    if (!count || count > 8 || refs <= 0 || mat + refs + 0x34 > d->len) return -1;
    const u8 *ref = d->b + mat + refs;
    s32 rel = (s32)be32(ref);                   /* self-relative, to the name's characters */
    const u8 *nm = ref + rel;
    if (!rel || nm - 4 < d->b || nm >= d->b + d->len) return -1;
    u32 nlen = be32(nm - 4);
    if (!nlen || nlen > 255 || nm + nlen > d->b + d->len) return -1;

    const u8 *tf = f;                           /* the buffer the texture is in */
    u32 tflen = d->flen, tex = brres_find(f, d->flen, "Textures(NW4R)", (const char *)nm, nlen);
    int id = 0;
    const u8 *pal = NULL; int palfmt = 0, palcount = 0;
    const u8 *palbuf = NULL; u32 pallen = 0;
    if (tex && !memcmp(f + tex, "TEX0", 4)) {
        u32 pl = brres_find(f, d->flen, "Palettes(NW4R)", (const char *)nm, nlen);
        if (pl && !memcmp(f + pl, "PLT0", 4)) { palbuf = f + pl; pallen = d->flen - pl; }
    } else if (!ext_texture((const char *)nm, nlen, &tf, &tflen, &tex, &palbuf, &pallen, &id))
        return -1;
    const u8 *t = tf + tex;
    int fmt = be32(t + 0x20), w = be16(t + 0x1C), h = be16(t + 0x1E);
    if (fmt >= 8 && fmt <= 10 && palbuf) {
        u32 plen = be32(palbuf + 4), doff = be32(palbuf + 0x10);
        palcount = be16(palbuf + 0x1C);
        if (doff + palcount * 2u <= pallen && doff + palcount * 2u <= plen) { pal = palbuf + doff; palfmt = be32(palbuf + 0x18); }
        else palcount = 0;
    }
    /* the textures of a model are told apart by where they are; another file gets its own range */
    int idx = add_texture(m, tf, tflen, tex + be32(t + 0x10), fmt, w, h, pal, palfmt, palcount,
                          (int)be32(ref + 0x18), (int)be32(ref + 0x1C), (u32)id << 26);
    return idx;
}

/* An MDL0 material's blend mode, from the pixel-engine part of its display list
 * (BP 0xFE mask, then BP 0x41 PE_CMODE0), and its translucent flag. 0: opaque, no blending. */
static u32 mdl_material_pe(mdl_t *d, u32 mat)
{
    if (!mat || mat + 0x14 > d->len) return 0;
    u32 size = be32(d->b + mat);
    if (size < 0x20 || mat + size > d->len) size = d->len - mat;
    u32 pe = be32(d->b + mat + 0x10) & PE_XLU;
    const u8 *p = d->b + mat;
    for (u32 i = 0x40; i + 10 <= size; i++)
        if (p[i] == 0x61 && p[i + 1] == 0xFE && p[i + 5] == 0x61 && p[i + 6] == 0x41) {
            pe |= (u32)p[i + 8] << 8 | p[i + 9];
            break;
        }
    return pe;
}

static bool load_mdl0(model_t *m, const u8 *file, u32 flen, u32 off, u32 len, const char *name)
{
    const u8 *b = file + off;
    mdl_t d = { b, len, be32(b + 8), file, flen, NULL, NULL, 0 };
    if (d.ver < 8 || d.ver > 11) return false;
    bool v10 = d.ver >= 10;
    u32 g_def = mdl_group(&d, 0), g_bone = mdl_group(&d, 1), g_pos = mdl_group(&d, 2),
        g_nrm = mdl_group(&d, 3), g_clr = mdl_group(&d, 4), g_uv = mdl_group(&d, 5),
        g_mat = mdl_group(&d, v10 ? 8 : 6), g_obj = mdl_group(&d, v10 ? 10 : 8);
    if (!g_obj || !g_pos) return false;

    /* bones: node id -> world matrix */
    int nb = g_bone ? (int)be32(b + g_bone + 4) : 0;
    d.nmtx = 0;
    for (int i = 0; i < nb; i++) {
        u32 at = grp_item(&d, g_bone, i, 0xD0);
        if (at) {
            s32 id = (s32)be32(b + at + 0x10);
            if (id >= d.nmtx && id < 4096) d.nmtx = id + 1;
        }
    }
    if (d.nmtx) {
        d.mtx = calloc(d.nmtx, sizeof(*d.mtx));
        d.has = calloc(d.nmtx, 1);
    }
    for (int i = 0; i < nb && d.mtx && d.has; i++) {
        u32 at = grp_item(&d, g_bone, i, 0xD0);
        if (!at) continue;
        s32 id = (s32)be32(b + at + 0x10);
        if (id < 0 || id >= d.nmtx) continue;
        for (int k = 0; k < 12; k++) d.mtx[id][k] = bef(b + at + 0x70 + k * 4);
        d.has[id] = 1;
    }
    m->bones = nb;

    /* draw lists: object -> bone, for objects without their own node */
    int nobj = be32(b + g_obj + 4);
    int *obj_bone = malloc((nobj > 0 ? nobj : 1) * sizeof(int) * 2);
    int *obj_mat = obj_bone ? obj_bone + (nobj > 0 ? nobj : 1) : NULL;
    for (int i = 0; obj_bone && i < nobj; i++) obj_bone[i] = obj_mat[i] = -1;
    int ndef = g_def ? (int)be32(b + g_def + 4) : 0;
    for (int i = 0; i < ndef && obj_bone; i++) {
        u32 at = grp_item(&d, g_def, i, 1);
        while (at && at + 8 <= len && b[at] == 4) {
            u32 mat = be16(b + at + 1), obj = be16(b + at + 3), bone = be16(b + at + 5);
            if ((int)obj < nobj && obj_mat[obj] < 0) obj_mat[obj] = mat;
            if ((int)obj < nobj && obj_bone[obj] < 0) {
                u32 ba = grp_item(&d, g_bone, bone, 0x14);
                obj_bone[obj] = ba ? (s32)be32(b + ba + 0x10) : -1;
            }
            at += 8;
        }
    }

    for (int i = 0; i < nobj; i++) {
        u32 o = grp_item(&d, g_obj, i, 0x60);
        if (!o) continue;
        arr_t pos, nrm, uv;
        mdl_array(&d, grp_item(&d, g_pos, (s16)be16(b + o + 0x48), 0x20), ARR_POS, &pos);
        mdl_array(&d, grp_item(&d, g_nrm, (s16)be16(b + o + 0x4A), 0x20), ARR_NRM, &nrm);
        mdl_array(&d, grp_item(&d, g_uv, (s16)be16(b + o + 0x50), 0x20), ARR_UV, &uv);
        if (!pos.base) continue;

        int tex = -1;
        u32 pe = 0;

        if (obj_mat && obj_mat[i] >= 0) {
            u32 mat = grp_item(&d, g_mat, obj_mat[i], 0x34);
            if (uv.base) tex = mdl_material_texture(&d, m, mat);
            pe = mdl_material_pe(&d, mat);
        }

        emit_t e = { m, &pos, &nrm, &uv, NULL, 0, 0, 0, mdl_matrix, &d, -1, { 0 } };
        if (tex >= 0) memset(e.tint, 255, 3);
        else memcpy(e.tint, PALETTE[i & 7], 3);

        u32 ca = grp_item(&d, g_clr, (s16)be16(b + o + 0x4C), 0x20);
        if (ca) {
            s32 doff = (s32)be32(b + ca + 8);
            e.clr_fmt = be32(b + ca + 24);
            e.clr_stride = b[ca + 28];
            e.clr_n = be16(b + ca + 30);
            if (doff > 0 && e.clr_fmt <= 5 && ca + doff + (u64)e.clr_stride * e.clr_n <= len)
                e.clr = b + ca + doff;
        }

        s32 node = (s32)be32(b + o + 0x08);
        e.default_node = node >= 0 ? node : (obj_bone ? obj_bone[i] : -1);

        u32 start = m->n;
        mdl_object(&d, &e, o);
        add_mesh(m, start, tex, pe);
        m->meshes++;
    }

    free(obj_bone);
    free(d.mtx);
    free(d.has);

    snprintf(m->name, sizeof(m->name), "%s", name);
    m->kind = "MDL0";
    return m->n > 0;
}

/* MDL0s inside a BRRES ("3DModels(NW4R)"), or a bare MDL0. */
static int brres_models(const u8 *b, u32 len, u32 *offs, char names[][64], int max)
{
    if (!memcmp(b, "MDL0", 4)) {
        offs[0] = 0;
        snprintf(names[0], 64, "model");
        return 1;
    }
    if (len < 0x10 || memcmp(b, "bres", 4)) return 0;

    u32 root = be16(b + 0x0C);
    if (root + 16 > len || memcmp(b + root, "root", 4)) return 0;

    u32 g = root + 8, n = be32(b + g + 4), found = 0;

    for (u32 i = 1; i <= n && g + 8 + (i + 1) * 16 <= len; i++) {
        const u8 *e = b + g + 8 + i * 16;
        u32 sub = g + be32(e + 12);
        if (sub + 8 > len) continue;
        u32 sn = be32(b + sub + 4);
        for (u32 k = 1; k <= sn && sub + 8 + (k + 1) * 16 <= len && (int)found < max; k++) {
            const u8 *se = b + sub + 8 + k * 16;
            u32 at = sub + be32(se + 12), no = sub + be32(se + 8);
            if (at + 0x40 > len || memcmp(b + at, "MDL0", 4)) continue;
            offs[found] = at;
            if (no < len)
                snprintf(names[found], 64, "%.*s", (int)(len - no < 63 ? len - no : 63),
                         (const char *)b + no);
            else snprintf(names[found], 64, "model %u", (unsigned)found + 1);
            found++;
        }
    }

    return found;
}

/* ---------------------------------------------------------------- HSF */

/* HAL Laboratory "HSFV037" (Mario Party 4-8 models, after nintoolbox's lib-hsf.c): a table of 20
 * { offset, count } entries; positions, normals, UVs and primitives (index 4 - 7) are arrays of
 * 12-byte { name, count, data offset } headers, one per mesh part. Each primitive names its
 * material (2), whose attribute symbols (19 -> 3) pick a bitmap (9) and its palette (10); untextured
 * materials are drawn in their diffuse color. */
typedef struct { u32 name, count, off; } hsfh_t;

static bool hsf_hdrs(const u8 *b, u32 len, u32 off, u32 cnt, hsfh_t *o)
{
    if (!cnt || cnt > 512 || (u64)off + (u64)cnt * 12 > len) return false;
    for (u32 i = 0; i < cnt; i++) {
        o[i].name = be32(b + off + i * 12);
        o[i].count = be32(b + off + i * 12 + 4);
        o[i].off = be32(b + off + i * 12 + 8);
    }
    return true;
}

static const char *hsf_str(const u8 *b, u32 len, u32 strs, u32 rel)
{
    u64 at = (u64)strs + rel;
    return at < len && memchr(b + at, 0, len - at) ? (const char *)b + at : "";
}

#define HSF_MAT_MAX 512
typedef struct {
    int tex;                            /* index into m->tex, or -1 */
    u8 rgb[3];                          /* diffuse */
    float ss, st, ts, tt;               /* UV scale and translation of the texture */
} hsf_mat_t;

/* Bitmap ti as a GX texture: formats 0 - 6 are GX's own, 7 is CMPR, 9 - 11 are color-indexed (4 or
 * 8 bits per pixel) with an RGB565, RGB5A3 or IA8 palette, as Mario Party's loader sets its TLUT. */
static int hsf_texture(model_t *m, const u8 *b, u32 len, const u32 *eo, const u32 *ec, u32 ti, int ws, int wt)
{
    u64 o = (u64)eo[9] + (u64)ti * 32;
    if (ti >= ec[9] || o + 32 > len) return -1;
    const u8 *r = b + o;
    int fmt = r[8], bpp = r[9], w = be16(r + 10), h = be16(r + 12), gx = fmt;
    const u8 *pal = NULL;
    int palfmt = 0, palcount = 0;
    if (fmt == 7) gx = GX_TF_CMPR;
    else if (fmt >= 9 && fmt <= 11) {
        gx = bpp == 4 ? GX_TF_CI4 : GX_TF_CI8;
        palfmt = fmt == 9 ? 1 : fmt == 10 ? 2 : 0;          /* GX_TL_RGB565 / RGB5A3 / IA8 */
        s32 pi = (s32)be32(r + 20);
        u64 po = (u64)eo[10] + (u64)pi * 16;
        if (pi < 0 || (u32)pi >= ec[10] || po + 16 > len) return -1;
        palcount = (int)be32(b + po + 8);
        u64 pd = (u64)eo[10] + (u64)ec[10] * 16 + be32(b + po + 12);
        if (palcount <= 0 || palcount > 256 || pd + (u64)palcount * 2 > len) return -1;
        pal = b + pd;
    } else if (fmt > 6) return -1;
    u64 at = (u64)eo[9] + (u64)ec[9] * 32 + be32(r + 28);
    if (at >= len) return -1;
    return add_texture(m, b, len, (u32)at, gx, w, h, pal, palfmt, palcount, ws, wt, 0);
}

/* Each material's first texture (through its attribute symbols) and diffuse color. */
static void hsf_materials(model_t *m, const u8 *b, u32 len, const u32 *eo, const u32 *ec, u32 strs,
                          hsf_mat_t *mat, u32 nmat)
{
    u32 nsym = eo[19] && strs > eo[19] ? (strs - eo[19]) / 4 : 0;
    for (u32 i = 0; i < nmat; i++) {
        hsf_mat_t *t = &mat[i];
        u64 o = (u64)eo[2] + (u64)i * 60;
        t->tex = -1;
        t->ss = t->st = 1;
        t->ts = t->tt = 0;
        memcpy(t->rgb, PALETTE[i & 7], 3);
        if (o + 60 > len) continue;
        memcpy(t->rgb, b + o + 0x0E, 3);
        u32 nt = be32(b + o + 52), first = be32(b + o + 56);
        for (u32 j = 0; j < nt && j < 8 && t->tex < 0; j++) {
            u64 so = (u64)eo[19] + (u64)(first + j) * 4;
            if (first + j >= nsym || so + 4 > len) break;
            u32 ai = be32(b + so);
            u64 ao = (u64)eo[3] + (u64)ai * 132;
            if (ai >= ec[3] || ao + 132 > len) continue;
            s32 ti = (s32)be32(b + ao + 128);
            if (ti < 0) continue;
            t->tex = hsf_texture(m, b, len, eo, ec, (u32)ti, (int)be32(b + ao + 100), (int)be32(b + ao + 104));
            if (t->tex < 0) continue;
            t->ss = bef(b + ao + 0x28);
            t->st = bef(b + ao + 0x2C);
            t->ts = bef(b + ao + 0x30);
            t->tt = bef(b + ao + 0x34);
            if (!(t->ss > -1e4f && t->ss < 1e4f) || !t->ss) t->ss = 1;     /* NaN or nonsense */
            if (!(t->st > -1e4f && t->st < 1e4f) || !t->st) t->st = 1;
            if (!(t->ts > -1e4f && t->ts < 1e4f)) t->ts = 0;
            if (!(t->tt > -1e4f && t->tt < 1e4f)) t->tt = 0;
        }
    }
}

static bool load_hsf(model_t *m, const u8 *b, u32 len, const char *name)
{
    if (len < 0xB0 || memcmp(b, "HSFV037", 7)) return false;
    u32 eo[20], ec[20];
    for (int i = 0; i < 20; i++) { eo[i] = be32(b + 8 + i * 8); ec[i] = be32(b + 12 + i * 8); }
    u32 strs = be32(b + 0xA8);
    u32 np = ec[4], nn = ec[5], nu = ec[6], nf = ec[7];
    if (!np || !nf || strs >= len || np > 512 || nn > 512 || nu > 512 || nf > 512) return false;
    hsfh_t ph[512], nh[512], uh[512], fh[512];
    if (!hsf_hdrs(b, len, eo[4], np, ph) || !hsf_hdrs(b, len, eo[7], nf, fh)) return false;
    bool have_n = nn && hsf_hdrs(b, len, eo[5], nn, nh);
    bool have_u = nu && hsf_hdrs(b, len, eo[6], nu, uh);
    u32 pbase = eo[4] + np * 12, nbase = eo[5] + nn * 12, ubase = eo[6] + nu * 12, fbase = eo[7] + nf * 12;
    u64 ext = fbase;
    for (u32 i = 0; i < nf; i++) ext += (u64)fh[i].count * 48;

    /* normals are 3 x f32, or 3 signed bytes (0x20 aligned) in some files */
    bool packed = false;
    if (have_n && nn >= 2) {
        u64 end = (u64)nbase + nh[0].off + (u64)nh[0].count * 3;
        if (end % 0x20) end += 0x20 - end % 0x20;
        packed = nh[1].off == end - nbase;
    }
    u32 nstride = packed ? 3 : 12;
    m->bones = 0;

    u32 nmat = ec[2] <= HSF_MAT_MAX ? ec[2] : 0;
    hsf_mat_t *mat = nmat ? malloc(nmat * sizeof(*mat)) : NULL;
    if (!mat) nmat = 0;
    hsf_materials(m, b, len, eo, ec, strs, mat, nmat);

    for (u32 mi = 0; mi < nf; mi++) {
        const char *mname = hsf_str(b, len, strs, fh[mi].name);
        bool dup = false;
        for (u32 j = 0; j < mi && !dup; j++) dup = !strcmp(hsf_str(b, len, strs, fh[j].name), mname);
        if (dup) continue;
        int pi = -1, ni = -1, ui = -1;
        for (u32 j = 0; j < np; j++) if (!strcmp(hsf_str(b, len, strs, ph[j].name), mname)) { pi = j; break; }
        if (pi < 0 && mi < np) pi = mi;
        if (pi < 0) continue;
        if (have_n) for (u32 j = 0; j < nn; j++) if (!strcmp(hsf_str(b, len, strs, nh[j].name), mname)) { ni = j; break; }
        if (have_u) for (u32 j = 0; j < nu; j++) if (!strcmp(hsf_str(b, len, strs, uh[j].name), mname)) { ui = j; break; }
        u32 pc = ph[pi].count;
        u64 pdata = (u64)pbase + ph[pi].off;
        if (!pc || pdata + (u64)pc * 12 > len) continue;
        u32 ncnt = 0, ucnt = 0;
        u64 ndata = 0, udata = 0;
        if (ni >= 0) { ncnt = nh[ni].count; ndata = (u64)nbase + nh[ni].off; if (ndata + (u64)ncnt * nstride > len) ncnt = 0; }
        if (ui >= 0) { ucnt = uh[ui].count; udata = (u64)ubase + uh[ui].off; if (udata + (u64)ucnt * 8 > len) ucnt = 0; }
        u32 nprim = fh[mi].count;
        u64 fdata = (u64)fbase + fh[mi].off;
        if (!nprim || fdata + (u64)nprim * 48 > len) continue;

        u32 start = m->n;
        int run = -2;                   /* the material of the triangles since start */
        const u8 *rp = b + fdata;
        for (u32 i = 0; i < nprim; i++, rp += 48) {
            s16 type = (s16)be16(rp);
            if (type < 2 || type > 4) continue;
            u32 mn = be16(rp + 2) & 0xFFF;
            const hsf_mat_t *mt = mn < nmat ? &mat[mn] : NULL;
            int mkey = mt ? (int)mn : -1;
            if (mkey != run) {          /* one mesh per material */
                if (run != -2) {
                    add_mesh(m, start, run >= 0 ? mat[run].tex : -1, 0);
                    start = m->n;
                }
                run = mkey;
            }
            s16 pidx[64], nidx[64], uidx[64];
            int ng = type == 4 ? 3 : 4, tot = ng;
            const u8 *gp = rp + 4;
            for (int g = 0; g < ng; g++, gp += 8) { pidx[g] = (s16)be16(gp); nidx[g] = (s16)be16(gp + 2); uidx[g] = (s16)be16(gp + 6); }
            if (type == 4) {
                s32 ecount = (s32)be32(gp);
                u32 eoff = be32(gp + 4);
                pidx[3] = pidx[1]; nidx[3] = nidx[1]; uidx[3] = uidx[1];
                tot = 4;
                if (ecount > 0 && ecount <= 60 && ext + (u64)eoff * 8 + (u64)ecount * 8 <= len) {
                    const u8 *ep = b + ext + (u64)eoff * 8;
                    for (int g = 0; g < ecount; g++, ep += 8) { pidx[4 + g] = (s16)be16(ep); nidx[4 + g] = (s16)be16(ep + 2); uidx[4 + g] = (s16)be16(ep + 6); }
                    tot = 4 + ecount;
                }
            }
            int seq[3 * 64], ns = 0;
            if (type == 2) { seq[0] = 0; seq[1] = 1; seq[2] = 2; ns = 3; }
            else if (type == 3) { int q[6] = { 0, 1, 2, 1, 3, 2 }; for (int k = 0; k < 6; k++) seq[k] = q[k]; ns = 6; }
            else for (int c = 2; c < tot; c++) {
                int i0 = (c & 1) ? c - 1 : c - 2, i1 = (c & 1) ? c - 2 : c - 1;
                if (pidx[i0] == pidx[i1] || pidx[i1] == pidx[c] || pidx[i0] == pidx[c]) continue;
                seq[ns++] = i0; seq[ns++] = i1; seq[ns++] = c;
            }
            for (int k = 0; k + 2 < ns + 0 && ns; k += 3) {
                vert_t v[3];
                for (int q = 0; q < 3; q++) {
                    int g = seq[k + q];
                    u32 pidv = pidx[g] >= 0 && (u32)pidx[g] < pc ? (u32)pidx[g] : 0;
                    const u8 *pp = b + pdata + (u64)pidv * 12;
                    v[q].p[0] = bef(pp); v[q].p[1] = bef(pp + 4); v[q].p[2] = bef(pp + 8);
                    v[q].n[0] = 0; v[q].n[1] = 1; v[q].n[2] = 0;
                    if (ncnt) {
                        u32 nv = nidx[g] >= 0 && (u32)nidx[g] < ncnt ? (u32)nidx[g] : 0;
                        const u8 *np_ = b + ndata + (u64)nv * nstride;
                        if (packed) { v[q].n[0] = (s8)np_[0] / 127.f; v[q].n[1] = (s8)np_[1] / 127.f; v[q].n[2] = (s8)np_[2] / 127.f; }
                        else { v[q].n[0] = bef(np_); v[q].n[1] = bef(np_ + 4); v[q].n[2] = bef(np_ + 8); }
                    }
                    v[q].t[0] = v[q].t[1] = 0;
                    if (ucnt) {
                        u32 uv = uidx[g] >= 0 && (u32)uidx[g] < ucnt ? (u32)uidx[g] : 0;
                        v[q].t[0] = bef(b + udata + (u64)uv * 8); v[q].t[1] = bef(b + udata + (u64)uv * 8 + 4);
                    }
                    if (mt && mt->tex >= 0) {   /* the texture's own colors */
                        v[q].t[0] = v[q].t[0] * mt->ss + mt->ts;
                        v[q].t[1] = v[q].t[1] * mt->st + mt->tt;
                        memset(v[q].c, 255, 3);
                    } else if (mt)
                        memcpy(v[q].c, mt->rgb, 3);
                    else
                        memcpy(v[q].c, PALETTE[mi & 7], 3);
                    v[q].c[3] = 255;
                }
                if (!push(m, &v[0]) || !push(m, &v[1]) || !push(m, &v[2])) break;
            }
        }
        add_mesh(m, start, run >= 0 ? mat[run].tex : -1, 0);
        m->meshes++;
    }
    free(mat);
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->kind = "HSF";
    return m->n > 0;
}

/* ---------------------------------------------------------------- Monster Games .mod */

/* Excite Truck / Excitebots models (after nintoolbox's lib-excite.c, GPL-2.0+): one or more "3LDN" /
 * "2LDN" chunks, each a 0x40-byte little-endian header, the position and texcoord arrays, then a
 * genuine GX display list. The list's CP writes give the arrays' formats (VAT group A at 0x70); its
 * draw calls carry index tuples whose width isn't stored, so it is found by trying each one and
 * keeping the one that reads the whole list as well-formed commands with sane positions. A file may
 * start with an entry table (count, offset; 16-byte entries whose +12 is a chunk); without one every
 * chunk in the file is taken. Geometry only: each chunk gets a flat color. */
#define MOD_PRIMS 4096

typedef struct { u8 kind; u16 cnt; u32 off; } modprim_t;

/* The display list read with bpv-byte vertices; false on a malformed command. */
static bool mod_dl(const u8 *dl, u32 n, u32 bpv, modprim_t *pr, u32 *np, u32 *total, u32 *vat70, bool *have70)
{
    u32 p = 0;
    *np = *total = 0;
    *have70 = false;
    while (p < n) {
        u8 op = dl[p];
        if (op == 0x00) { p++; continue; }
        if (op == 0xE3) break;
        if (op == 0x08) {
            if (p + 6 > n) return false;
            if (dl[p + 1] == 0x70) { *vat70 = be32(dl + p + 2); *have70 = true; }
            p += 6;
            continue;
        }
        if (op == 0x10) {
            if (p + 5 > n) return false;
            u64 adv = 5 + ((u64)be16(dl + p + 1) + 1) * 4;
            if (p + adv > n) return false;
            p += (u32)adv;
            continue;
        }
        if (op == 0x61) {
            if (p + 5 > n) return false;
            p += 5;
            continue;
        }
        if (op < 0x80 || op >= 0xC0 || p + 3 > n) return false;
        u16 cnt = be16(dl + p + 1);
        p += 3;
        if (p + (u64)cnt * bpv > n || *np >= MOD_PRIMS) return false;
        pr[*np].kind = (op - 0x80) >> 3;        /* 0 quads, 1 quads, 2 triangles, 3 strip, 4 fan, 5+ lines */
        pr[*np].cnt = cnt;
        pr[*np].off = p;
        (*np)++;
        *total += cnt;
        p += cnt * bpv;
    }
    return *np > 0;
}

static float mod_attr(const u8 *p, u32 fmt, float scale)
{
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return be16(p) * scale;
    case 3: return (s16)be16(p) * scale;
    default: return bef(p);
    }
}

/* One chunk at m into the model. */
static bool mod_chunk(model_t *m, const u8 *b, u32 len, u32 at, int color, int tex)
{
    static const u32 FSZ[5] = { 1, 1, 2, 2, 4 };
    if ((u64)at + 0x40 > len) return false;
    u32 h[14];
    for (int i = 0; i < 14; i++) h[i] = le32(b + at + i * 4);
    u32 dl_end = h[1];
    if (dl_end >> 16 == 0xE3E3) dl_end &= 0xFFFF;                 /* NDL2 keeps it in 16 bits */
    u32 npos = h[7];
    if (!npos || npos > 100000) return false;
    bool ndl2 = !memcmp(b + at, "2LDN", 4);

    /* the display list starts with its CP preamble: 08 70 .. 08 80 .. 08 90 */
    u32 hi = dl_end && (u64)at + dl_end <= len ? at + dl_end : len, dls = 0, dln = 0;
    for (u32 q = at; q + 14 <= hi; q++)
        if (b[q] == 0x08 && b[q + 1] == 0x70 && b[q + 6] == 0x08 && b[q + 7] == 0x80 && b[q + 12] == 0x08 &&
            b[q + 13] == 0x90) {
            dls = q;
            dln = dl_end > q - at && (u64)at + dl_end <= len ? dl_end - (q - at) : len - q;
            break;
        }
    if (!dls && h[13] >= at && h[13] < len && h[12] && (u64)h[13] + h[12] <= len) {
        dls = h[13];
        dln = h[12];
    }
    if (!dls) return false;
    const u8 *dl = b + dls;

    modprim_t *pr = malloc(MOD_PRIMS * sizeof(*pr)), *best = malloc(MOD_PRIMS * sizeof(*best));
    if (!pr || !best) {
        free(pr);
        free(best);
        return false;
    }
    static const u32 CAND[8] = { 3, 2, 4, 5, 6, 7, 8, 1 };
    bool excluded[9] = { false }, ok = false, padded = false;
    int tbyte = -1;
    u32 bpv = 0, nb = 0, pos_n = 3, pos_fmt = 0, tex_fmt = 0, tex_n = 2, pos_off = 0, tex_off = 0, ntex = 0;
    float pscale = 1, tscale = 1;
    for (int retry = 0; retry < 8 && !ok; retry++) {
        u32 best_total = 0, va = 0;
        bpv = 0;
        for (int c = 0; c < 8; c++) {
            u32 v = CAND[c], np, total, va_c = 0;
            bool have;
            if (excluded[v] || !mod_dl(dl, dln, v, pr, &np, &total, &va_c, &have) || total <= best_total) continue;
            if (!have) {
                if (!bpv) excluded[v] = true;
                continue;
            }
            best_total = total;
            bpv = v;
            nb = np;
            va = va_c;
            memcpy(best, pr, np * sizeof(*pr));
        }
        if (!bpv) break;
        pos_fmt = va >> 1 & 7;
        u32 pos_shift = va >> 4 & 31;
        tex_fmt = va >> 22 & 7;
        u32 tex_shift = va >> 25 & 31;
        pos_n = (va & 1 ? 3 : 2) + (padded ? 1 : 0);    /* some NDL2 files pad XYZ to four floats */
        /* NDL2 names its arrays: positions at h[8], the next array at h[9] (so the gap says whether
         * positions are padded), texcoords at h[10] */
        bool n2arr = ndl2 && h[8] >= at + 0x40 && h[9] > h[8] && h[9] <= dls && !((h[9] - h[8]) % npos) &&
                     (h[9] - h[8]) / npos / FSZ[pos_fmt] >= pos_n && (h[9] - h[8]) / npos / FSZ[pos_fmt] <= 4 &&
                     !((h[9] - h[8]) / npos % FSZ[pos_fmt]);
        if (n2arr) pos_n = (h[9] - h[8]) / npos / FSZ[pos_fmt];
        tex_n = va >> 21 & 1 ? 2 : 1;
        if (pos_fmt > 4) {
            excluded[bpv] = true;
            continue;
        }
        pscale = pos_fmt == 4 ? 1 : 1.0f / (1u << pos_shift);
        tscale = tex_fmt == 4 ? 1 : 1.0f / (1u << tex_shift);
        /* NDL3 names its arrays at h[9..11]; NDL2's positions follow the header */
        u32 pbytes = npos * pos_n * FSZ[pos_fmt];
        pos_off = !ndl2 && h[9] >= at + 0x40 && h[9] < dls && h[9] + pbytes <= dls ? h[9] : at + 0x40;
        u32 second = !ndl2 && h[10] >= at + 0x40 && h[10] < dls ? h[10] : 0;
        u32 third = !ndl2 && h[11] >= at + 0x40 && h[11] < dls ? h[11] : 0;
        tex_off = third ? third : second ? second : pos_off + pbytes;
        if (n2arr) {
            pos_off = h[8];
            if (h[10] >= h[9] && h[10] < dls) tex_off = h[10];
        }
        /* which byte of a vertex is its texcoord index (the last one; -1: none) */
        tbyte = bpv == 3 && (n2arr || third || (second && second != tex_off)) ? 2
              : bpv == 5 ? 4 : n2arr && bpv > 1 ? (int)bpv - 1 : -1;
        u32 maxt = 0;
        for (u32 i = 0; tbyte >= 0 && i < nb; i++)
            for (u32 v = 0; v < best[i].cnt; v++) {
                u32 ti = dl[best[i].off + v * bpv + tbyte];
                if (ti > maxt) maxt = ti;
            }
        ntex = tbyte >= 0 ? maxt + 1 : 0;
        if ((u64)pos_off + pbytes > len) {
            excluded[bpv] = true;
            continue;
        }
        if (tex_fmt > 4 || (u64)tex_off + (u64)ntex * tex_n * FSZ[tex_fmt] > len) ntex = 0;
        /* a misread shows as huge or broken positions */
        float max_abs = 0;
        ok = true;
        for (u32 i = 0; i < npos * pos_n && ok; i++) {
            float v = mod_attr(b + pos_off + i * FSZ[pos_fmt], pos_fmt, pscale);
            if (!isfinite(v) || fabsf(v) > 1e9f) ok = false;
            else if (fabsf(v) > max_abs) max_abs = fabsf(v);
        }
        /* the header's radius is no use as a bound (tracks have tiny ones), but a misread runs away */
        if (ok && max_abs > 1e6f) ok = false;
        if (!ok && ndl2 && !padded && !n2arr) {
            padded = true;                          /* the same width again, with padded positions */
            retry--;
            continue;
        }
        padded = false;
        if (!ok) excluded[bpv] = true;
    }
    free(pr);
    if (!ok) {
        free(best);
        return false;
    }

    u32 start = m->n;
    for (u32 i = 0; i < nb; i++) {
        const modprim_t *q = &best[i];
        u32 ntri = q->kind == 3 || q->kind == 4 ? (q->cnt > 2 ? q->cnt - 2 : 0)
                 : q->kind == 2 ? q->cnt / 3 : q->kind <= 1 ? q->cnt / 4 * 2 : 0;
        for (u32 t = 0; t < ntri; t++) {
            u32 c[3];
            if (q->kind == 3) {                     /* strip, alternating winding */
                if (t & 1) { c[0] = t + 1; c[1] = t; c[2] = t + 2; }
                else       { c[0] = t; c[1] = t + 1; c[2] = t + 2; }
            } else if (q->kind == 4) { c[0] = 0; c[1] = t + 1; c[2] = t + 2; }
            else if (q->kind == 2)   { c[0] = t * 3; c[1] = t * 3 + 1; c[2] = t * 3 + 2; }
            else {                                  /* quads: two triangles each */
                u32 k = t / 2 * 4;
                if (t & 1) { c[0] = k; c[1] = k + 2; c[2] = k + 3; }
                else       { c[0] = k; c[1] = k + 1; c[2] = k + 2; }
            }
            vert_t v[3];
            for (int k = 0; k < 3; k++) {
                const u8 *vp = dl + q->off + c[k] * bpv;
                u32 pi = bpv == 4 || bpv == 5 ? be16(vp) : vp[0];
                u32 ti = tbyte >= 0 ? vp[tbyte] : 0;
                if (pi >= npos) pi = 0;
                const u8 *pp = b + pos_off + pi * pos_n * FSZ[pos_fmt];
                v[k].p[0] = mod_attr(pp, pos_fmt, pscale);
                v[k].p[1] = mod_attr(pp + FSZ[pos_fmt], pos_fmt, pscale);
                v[k].p[2] = pos_n > 2 ? mod_attr(pp + 2 * FSZ[pos_fmt], pos_fmt, pscale) : 0;
                v[k].t[0] = v[k].t[1] = 0;
                if (ntex && ti < ntex) {
                    const u8 *tp = b + tex_off + ti * tex_n * FSZ[tex_fmt];
                    v[k].t[0] = mod_attr(tp, tex_fmt, tscale);
                    v[k].t[1] = tex_n > 1 ? mod_attr(tp + FSZ[tex_fmt], tex_fmt, tscale) : 0;
                }
                if (tex >= 0) memset(v[k].c, 255, 3);
                else memcpy(v[k].c, PALETTE[color & 7], 3);
                v[k].c[3] = 255;
            }
            /* no normals in the file: the face's own */
            float e1[3], e2[3], n[3];
            for (int k = 0; k < 3; k++) { e1[k] = v[1].p[k] - v[0].p[k]; e2[k] = v[2].p[k] - v[0].p[k]; }
            n[0] = e1[1] * e2[2] - e1[2] * e2[1];
            n[1] = e1[2] * e2[0] - e1[0] * e2[2];
            n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l < 1e-12f) continue;
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) v[k].n[j] = n[j] / l;
            if (!push(m, &v[0]) || !push(m, &v[1]) || !push(m, &v[2])) break;
        }
    }
    free(best);
    if (m->n == start) return false;
    add_mesh(m, start, tex, 0);
    m->meshes++;
    return true;
}

/* Where a .mod's textures are: its own folder (usually the .car / .trk archive it came from), then,
 * beside that archive, the region's base track ("Canada_Opt1.trk" -> "canada.trk") and the shared
 * HobbySet.res, race.res and common.res. Set by model_view. */
#define MOD_DIRS 5
static char g_mod_dir[MOD_DIRS][VFS_PATH_MAX];
static vfs_entry *g_mod_list[MOD_DIRS];
static int g_mod_n[MOD_DIRS], g_mod_ndirs;

static void mod_dirs_free(void)
{
    for (int i = 0; i < g_mod_ndirs; i++) free(g_mod_list[i]);
    for (int i = 0; i < MOD_DIRS; i++) g_mod_list[i] = NULL, g_mod_n[i] = -1;
    g_mod_ndirs = 0;
}

/* name in the folder dir, any case; false when it isn't there */
static bool find_ci(const char *dir, const char *name, char *out)
{
    vfs_entry *l;
    int n;
    if (!vfs_list(dir, &l, &n)) return false;
    bool found = false;
    for (int i = 0; i < n && !found; i++)
        if (!strcasecmp(l[i].name, name)) found = vfs_join(out, VFS_PATH_MAX, dir, l[i].name);
    free(l);
    return found;
}

static void mod_dirs_set(const char *path)
{
    mod_dirs_free();
    char dir[VFS_PATH_MAX], outer[VFS_PATH_MAX], arc[VFS_PATH_MAX], p[VFS_PATH_MAX];
    if (!vfs_parent(path, dir, sizeof(dir))) return;
    snprintf(g_mod_dir[g_mod_ndirs++], VFS_PATH_MAX, "%s", dir);
    /* the archive the model is in, and the folder that holds it */
    snprintf(arc, sizeof(arc), "%s", path);
    char *bar = strstr(arc, "|/");
    if (!bar) return;
    *bar = 0;
    if (!vfs_parent(arc, outer, sizeof(outer))) return;
    const char *an = vfs_basename(arc);
    char want[4][64];
    int nw = 0;
    const char *cut = an + strcspn(an, "_.");
    if (arc_name_has_ext(an, ".trk") && *cut == '_')
        snprintf(want[nw++], 64, "%.*s.trk", (int)(cut - an), an);
    snprintf(want[nw++], 64, "HobbySet.res");
    snprintf(want[nw++], 64, "race.res");
    snprintf(want[nw++], 64, "common.res");
    for (int i = 0; i < nw && g_mod_ndirs < MOD_DIRS; i++)
        if (find_ci(outer, want[i], p)) {
            size_t l = strlen(p);
            if (l + 3 < VFS_PATH_MAX) snprintf(g_mod_dir[g_mod_ndirs++], VFS_PATH_MAX, "%s|/", p);
        }
}

/* The texture a chunk's descriptor names, loaded from the folders above; -1 when it isn't found. */
static int mod_texture(model_t *m, const u8 *b, u32 len, u32 desc)
{
    if (!desc || (u64)desc + 64 > len) return -1;
    u32 layers = le32(b + desc), at = layers == 1 ? 16 : layers == 2 ? 20 : layers <= 8 ? 24 : 0;
    if (!at) return -1;
    char name[48];
    u32 k = 0;
    for (; k + 1 < sizeof(name) && b[desc + at + k] >= 0x20 && b[desc + at + k] < 0x7F; k++)
        name[k] = (char)b[desc + at + k];
    name[k] = 0;
    if (k < 5 || strcasecmp(name + k - 4, ".tex")) return -1;
    u32 key = 0x80000000u;                              /* by name: the same texture is loaded once */
    for (u32 i = 0; i < k; i++) key = key * 31 + (u8)(name[i] | 0x20);
    key |= 0x80000000u;
    for (int i = 0; i < m->ntex; i++)
        if (m->tex[i].key == key) return i;
    for (int d = 0; d < g_mod_ndirs; d++) {
        if (g_mod_n[d] < 0 && !vfs_list(g_mod_dir[d], &g_mod_list[d], &g_mod_n[d])) {
            g_mod_n[d] = 0;
            g_mod_list[d] = NULL;
        }
        for (int i = 0; i < g_mod_n[d]; i++) {
            if (strcasecmp(g_mod_list[d][i].name, name)) continue;
            char p[VFS_PATH_MAX];
            u32 n;
            u8 *f = vfs_join(p, sizeof(p), g_mod_dir[d], g_mod_list[d][i].name) ? load_file(p, &n) : NULL;
            int w, h, t = -1;
            u8 *rgba = f ? excite_tex(f, n, &w, &h) : NULL;
            if (rgba) t = add_rgba_texture(m, rgba, w, h, key);
            free(rgba);
            free(f);
            return t;
        }
    }
    return -1;
}

static bool mod_is(const u8 *b, u32 len)
{
    for (u32 q = 0; q + 0x40 <= len; q += 4)
        if (!memcmp(b + q, "3LDN", 4) || !memcmp(b + q, "2LDN", 4)) return true;
    return false;
}

static bool load_mod(model_t *m, const u8 *b, u32 len, const char *name)
{
    m->bones = 0;
    u32 count = len >= 8 ? le32(b) : 0, table = len >= 8 ? le32(b + 4) : 0;
    if (count && count < 2000 && table >= 0x40 && (u64)table + (u64)count * 16 <= len) {
        for (u32 i = 0; i < count; i++)
            mod_chunk(m, b, len, le32(b + table + i * 16 + 12), (int)i, mod_texture(m, b, len, le32(b + table + i * 16 + 8)));
    }
    if (!m->n)
        for (u32 q = 0, i = 0; q + 0x40 <= len; q++) {
            if (memcmp(b + q, "3LDN", 4) && memcmp(b + q, "2LDN", 4)) continue;
            mod_chunk(m, b, len, q, (int)i++, -1);
            /* past this chunk, so its data isn't taken for another one */
            u32 skip = le32(b + q + 4);
            if (skip >> 16 == 0xE3E3) skip &= 0xFFFF;
            if (skip > 0x38 && (u64)q + skip <= len) q += skip - 1;
            else q += 0x37;
        }
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->kind = "Monster Games MOD";
    return m->n > 0;
}

/* ---------------------------------------------------------------- J3D */

typedef struct {
    float (*w)[12];
    int nj;
    const u8 *drw_weighted;
    const u8 *drw_idx;
    int ndrw;
} j3d_t;

static const float *j3d_matrix(void *ctx, int drw)
{
    j3d_t *d = ctx;
    if (drw < 0 || drw >= d->ndrw || d->drw_weighted[drw]) return NULL;
    int j = be16(d->drw_idx + drw * 2);
    return j < d->nj ? d->w[j] : NULL;
}

static void euler_mtx(const float s[3], const float r[3], const float t[3], float *M)
{
    float cx = cosf(r[0]), sx = sinf(r[0]), cy = cosf(r[1]), sy = sinf(r[1]), cz = cosf(r[2]),
          sz = sinf(r[2]);
    /* R = Rz * Ry * Rx, then scale columns */
    float R[9] = {
        cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
        sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
        -sy,     cy * sx,                cy * cx,
    };
    for (int r2 = 0; r2 < 3; r2++) {
        for (int c = 0; c < 3; c++) M[r2 * 4 + c] = R[r2 * 3 + c] * s[c];
        M[r2 * 4 + 3] = t[r2];
    }
}

static bool load_j3d(model_t *m, const u8 *b, u32 len, const char *name)
{
    u32 nsec = be32(b + 0x0C), pos = 0x20, inf = 0, vtx = 0, drw = 0, jnt = 0, shp = 0, mat3 = 0,
        tex1 = 0;
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (!memcmp(b + pos, "INF1", 4)) inf = pos;
        else if (!memcmp(b + pos, "VTX1", 4)) vtx = pos;
        else if (!memcmp(b + pos, "DRW1", 4)) drw = pos;
        else if (!memcmp(b + pos, "JNT1", 4)) jnt = pos;
        else if (!memcmp(b + pos, "SHP1", 4)) shp = pos;
        else if (!memcmp(b + pos, "MAT3", 4)) mat3 = pos;
        else if (!memcmp(b + pos, "TEX1", 4)) tex1 = pos;
        if (sz < 8 || pos + sz > len) break;
        pos += sz;
    }
    if (!vtx || !shp) return false;

    /* VTX1 arrays */
    arr_t pa = { 0 }, na = { 0 }, ua = { 0 };
    const u8 *cbase = NULL;
    u32 cn = 0, cstride = 4;
    int cfmt = 5;

    u32 vsz = be32(b + vtx + 4), offs[13];
    for (int i = 0; i < 13; i++) offs[i] = be32(b + vtx + 12 + i * 4);
    for (u32 hp = vtx + be32(b + vtx + 8); hp + 16 <= len; hp += 16) {
        u32 attr = be32(b + hp);
        if (attr == 0xFF) break;
        u32 cnt = be32(b + hp + 4), type = be32(b + hp + 8);
        int frac = b[hp + 12];
        int slot = attr == 9 ? 0 : attr == 10 ? 1 : attr == 11 ? 3 : attr == 13 ? 5 : -1;
        if (slot < 0 || !offs[slot]) continue;

        u32 start = offs[slot], stop = vsz;
        for (int k = 0; k < 13; k++) if (offs[k] > start && offs[k] < stop) stop = offs[k];
        if (vtx + stop > len) continue;

        if (attr == 11) {
            static const u8 csz[6] = { 2, 3, 4, 2, 3, 4 };
            if (type > 5) continue;
            cbase = b + vtx + start;
            cfmt = type;
            cstride = csz[type];
            cn = (stop - start) / cstride;
            continue;
        }

        arr_t *a = attr == 9 ? &pa : attr == 10 ? &na : &ua;
        int esz = type < 2 ? 1 : type < 4 ? 2 : 4;
        if (type > 4) continue;

        a->type = type;
        a->frac = frac;
        a->comps = attr == 9 ? (cnt ? 3 : 2) : attr == 13 ? (cnt ? 2 : 1) : 3;
        a->stride = esz * a->comps;
        a->base = b + vtx + start;
        a->count = (stop - start) / a->stride;
    }

    if (!pa.base) return false;

    /* joints: world matrices from the INF1 hierarchy */
    j3d_t d = { 0 };

    if (jnt) {
        int n = (s16)be16(b + jnt + 8);
        u32 oj = be32(b + jnt + 12), orm = be32(b + jnt + 16);
        if (n > 0 && n < 4096 && jnt + oj + n * 64u <= len && jnt + orm + n * 2u <= len) {
            float (*local)[12] = malloc(n * sizeof(*local));
            d.w = calloc(n, sizeof(*d.w));
            int *parent = malloc(n * sizeof(int));
            if (local && d.w && parent) {
                d.nj = n;
                for (int i = 0; i < n; i++) {
                    int src = (s16)be16(b + jnt + orm + i * 2);
                    if (src < 0 || src >= n) src = i;
                    const u8 *jo = b + jnt + oj + src * 64;
                    float s[3], r[3], t[3];
                    for (int k = 0; k < 3; k++) {
                        s[k] = bef(jo + 4 + k * 4);
                        r[k] = (s16)be16(jo + 16 + k * 2) * (float)M_PI / 32768.0f;
                        t[k] = bef(jo + 24 + k * 4);
                    }
                    euler_mtx(s, r, t, local[i]);
                    parent[i] = -2;
                }

                if (inf) {
                    int stack[256], sp = 0, cur = -1, last = -1;
                    for (u32 p = inf + be32(b + inf + 0x14); p + 4 <= len; p += 4) {
                        int t = be16(b + p), idx = be16(b + p + 2);
                        if (t == 0) break;
                        if (t == 1) {
                            if (sp < 256) stack[sp++] = cur;
                            cur = last;
                        } else if (t == 2) {
                            cur = sp ? stack[--sp] : -1;
                        } else if (t == 0x10 && idx < n) {
                            if (parent[idx] == -2) parent[idx] = cur;
                            last = idx;
                        }
                    }
                }

                /* parents come first in the hierarchy; resolve in passes */
                u8 *done = calloc(n, 1);
                for (int pass = 0; done && pass < n; pass++) {
                    bool progress = false;
                    for (int i = 0; i < n; i++) {
                        if (done[i]) continue;
                        int p = parent[i] < 0 ? -1 : parent[i];
                        if (p >= 0 && !done[p]) continue;
                        if (p < 0) memcpy(d.w[i], local[i], sizeof(local[i]));
                        else mul34(d.w[p], local[i], d.w[i]);
                        done[i] = 1;
                        progress = true;
                    }
                    if (!progress) break;
                }
                free(done);
                m->bones = n;
            }

            free(local);
            free(parent);
        }
    }

    if (drw) {
        d.ndrw = (s16)be16(b + drw + 8);
        u32 ob = be32(b + drw + 12), oi = be32(b + drw + 16);
        if (d.ndrw < 0 || drw + ob + d.ndrw > len || drw + oi + d.ndrw * 2u > len) d.ndrw = 0;
        else {
            d.drw_weighted = b + drw + ob;
            d.drw_idx = b + drw + oi;
        }
    }

    /* shape -> material (INF1) -> texture (MAT3 texture slot 0 -> TEX1) */
    int nshape = be16(b + shp + 8);
    int *shape_tex = malloc((nshape > 0 ? nshape : 1) * sizeof(int));
    for (int i = 0; shape_tex && i < nshape; i++) shape_tex[i] = -1;

    if (shape_tex && inf && mat3 && tex1 && ua.base && mat3 + 12 + 30 * 4 <= len &&
        tex1 + 0x14 <= len) {
        int nmat = (s16)be16(b + mat3 + 8);

        u32 mo[30];
        for (int k = 0; k < 30; k++) mo[k] = be32(b + mat3 + 12 + k * 4);
        u32 msz = be32(b + mat3 + 4), remap_end = msz;
        for (int k = 16; k < 30; k++) if (mo[k] > mo[15] && mo[k] < remap_end) remap_end = mo[k];
        int ntexremap = mo[15] && remap_end > mo[15] ? (int)((remap_end - mo[15]) / 2) : 0;

        int ntex = (s16)be16(b + tex1 + 8);
        u32 thdr = tex1 + be32(b + tex1 + 12);

        int stack[256], sp = 0, cur = -1;
        for (u32 p = inf + be32(b + inf + 0x14); p + 4 <= len; p += 4) {
            int t = be16(b + p), idx = be16(b + p + 2);
            if (t == 0) break;
            if (t == 1) { if (sp < 256) stack[sp++] = cur; }
            else if (t == 2) cur = sp ? stack[--sp] : -1;
            else if (t == 0x11) cur = idx;
            else if (t == 0x12 && idx < nshape && shape_tex[idx] < 0 && cur >= 0 && cur < nmat &&
                     mo[0] && mo[1]) {

                u32 ra = mat3 + mo[1] + cur * 2;
                if (ra + 2 > len) continue;
                u32 rec = mat3 + mo[0] + be16(b + ra) * 332u;
                if (rec + 0x86 > len) continue;
                int tn = (s16)be16(b + rec + 0x84);
                if (tn < 0 || tn >= ntexremap || mat3 + mo[15] + tn * 2 + 2 > len) continue;
                int ti = (s16)be16(b + mat3 + mo[15] + tn * 2);
                u32 h = thdr + ti * 0x20;
                if (ti < 0 || ti >= ntex || h + 0x20 > len) continue;

                const u8 *pal = NULL;
                int palcount = be16(b + h + 0x0A);
                if (palcount && h + be32(b + h + 0x0C) + palcount * 2u <= len)
                    pal = b + h + be32(b + h + 0x0C);
                else palcount = 0;

                shape_tex[idx] =
                    add_texture(m, b, len, h + be32(b + h + 0x1C), b[h], be16(b + h + 2),
                                be16(b + h + 4), pal, b[h + 9], palcount, b[h + 6], b[h + 7], 0);
            }
        }
    }

    /* SHP1 */
    u32 o_shape = shp + be32(b + shp + 0x0C), o_attr = shp + be32(b + shp + 0x18),
        o_mtab = shp + be32(b + shp + 0x1C), o_dl = shp + be32(b + shp + 0x20),
        o_mdat = shp + be32(b + shp + 0x24), o_pkt = shp + be32(b + shp + 0x28);

    pv_t *pts = NULL;
    u32 cap = 0;

    for (int si = 0; si < nshape; si++) {
        const u8 *sh = b + o_shape + si * 0x28;
        if (sh + 0x28 > b + len) break;
        int npkt = be16(sh + 2), first_md = be16(sh + 6), first_pkt = be16(sh + 8);
        /* attribute list */

        int attrs[26], types[26], na2 = 0, stride = 0;
        bool has_pm = false;

        for (const u8 *ap = b + o_attr + be16(sh + 4); ap + 8 <= b + len && na2 < 26; ap += 8) {
            u32 at = be32(ap), ty = be32(ap + 4);
            if (at == 0xFF) break;
            attrs[na2] = at;
            types[na2] = ty;
            na2++;
            stride += ty == 3 ? 2 : 1;
            if (at == 0) has_pm = true;
        }

        int tex = shape_tex ? shape_tex[si] : -1;
        emit_t e = { m, &pa, &na, &ua, cbase, cn, cstride, cfmt, j3d_matrix, &d, -1, { 0 } };
        if (tex >= 0) memset(e.tint, 255, 3);
        else memcpy(e.tint, PALETTE[si & 7], 3);

        u32 start = m->n;
        int prev_mtx[16];
        for (int k = 0; k < 16; k++) prev_mtx[k] = -1;

        for (int pk = 0; pk < npkt; pk++) {
            const u8 *pl = b + o_pkt + (first_pkt + pk) * 8;
            const u8 *md = b + o_mdat + (first_md + pk) * 8;
            if (pl + 8 > b + len || md + 8 > b + len) break;

            u32 psize = be32(pl), poff = be32(pl + 4);
            int mcount = be16(md + 2);
            u32 mstart = be32(md + 4);

            int slots[16];
            for (int k = 0; k < 16; k++) {
                int v = k < mcount && o_mtab + (mstart + k) * 2 + 2 <= len
                            ? be16(b + o_mtab + (mstart + k) * 2)
                            : 0xFFFF;
                slots[k] = v == 0xFFFF ? prev_mtx[k] : v;
                prev_mtx[k] = slots[k];
            }

            e.default_node = slots[0];
            const u8 *p = b + o_dl + poff, *end = p + psize;
            if (end > b + len) break;

            while (p + 3 <= end) {
                u8 prim = *p & 0xF8;
                if (!*p || prim < 0x80 || prim > 0xB8) break;
                u32 n = be16(p + 1);
                p += 3;

                if ((u64)n * stride > (u64)(end - p)) break;
                if (n > cap) {
                    pv_t *np = realloc(pts, n * sizeof(pv_t));
                    if (!np) break;
                    pts = np;
                    cap = n;
                }

                for (u32 i = 0; i < n; i++) {
                    pv_t *v = &pts[i];
                    memset(v, 0, sizeof(*v));
                    v->mtx = -1;
                    for (int a = 0; a < na2; a++) {
                        u32 idx = types[a] == 3 ? be16(p) : *p;
                        p += types[a] == 3 ? 2 : 1;
                        if (attrs[a] == 0) {
                            int s = types[a] == 1 ? idx / 3 : idx;
                            v->mtx = s < 16 ? slots[s] : -1;
                        } else if (attrs[a] == 9)
                            v->pos = idx;
                        else if (attrs[a] == 10) v->nrm = idx;
                        else if (attrs[a] == 11) v->clr = idx;
                        else if (attrs[a] == 13) v->uv = idx;
                    }
                }
                emit_prim(&e, prim, pts, n);
            }
        }

        (void)has_pm;
        add_mesh(m, start, tex, 0);
        m->meshes++;
    }

    free(shape_tex);
    free(pts);
    free(d.w);
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->kind = "J3D";
    return m->n > 0;
}

/* ---------------------------------------------------------------- HSD */

/* HAL Laboratory sysdolphin (HSD) .dat models (Super Smash Bros. Melee, Kirby Air Ride, TV no Tomo
 * Channel, ...), after nintoolbox's lib-hsd.c and Ploaj's HSDLib: a 0x20 byte header, the data, a
 * relocation table (every pointer in the data is listed there), then root / reference tables and
 * their names. A root is a joint (JObj) tree, or "scene_data" listing several. Each joint has a
 * list of DObjs, each a material (MObj: color, TObj textures) and polygons (PObj: GX vertex
 * attributes and display lists), bound to their joint, another one, or weighted envelopes. */
#define HSD_BASE   0x20
#define HSD_JOINTS 2048

typedef struct {
    const u8 *d;
    u32 size, ro;                       /* file size, start of the relocation table */
    u32 *loc, *dst;                     /* pointer locations (sorted) and where they point */
    u32 n;
    u32 *joff;                          /* joints: offset and world matrix */
    float (*jw)[12];
    int nj;
} hsdm_t;

static bool hsd_head(const u8 *b, u32 len, u32 *ro, u32 *nrel, u32 *nroot, u32 *nref)
{
    if (len < 0x40 || be32(b) != len) return false;
    *ro = be32(b + 4) + HSD_BASE;
    *nrel = be32(b + 8);
    *nroot = be32(b + 12);
    *nref = be32(b + 16);
    return *ro >= HSD_BASE && *ro <= len && *nroot >= 1 && *nroot <= 0x10000 && *nref <= 0x10000 &&
           *nrel <= 0x400000 && (u64)*ro + (u64)*nrel * 4 + (u64)(*nroot + *nref) * 8 <= len;
}

bool model_hsd_is(const u8 *b, u32 len)
{
    u32 ro, nrel, nroot, nref;
    return hsd_head(b, len, &ro, &nrel, &nroot, &nref);
}

/* What the pointer stored at loc points to (an absolute offset), or 0. */
static u32 hp(const hsdm_t *h, u32 loc)
{
    u32 lo = 0, hi = h->n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (h->loc[mid] < loc) lo = mid + 1;
        else hi = mid;
    }
    return lo < h->n && h->loc[lo] == loc ? h->dst[lo] : 0;
}

static bool hsd_in(const hsdm_t *h, u32 off, u32 need) { return off >= HSD_BASE && (u64)off + need <= h->ro; }

static const float *hsd_world(const hsdm_t *h, u32 jobj)
{
    for (int i = 0; i < h->nj; i++)
        if (h->joff[i] == jobj) return h->jw[i];
    return NULL;
}

/* A joint's scale, rotation and translation, if they are sane numbers. */
static bool hsd_srt(const hsdm_t *h, u32 off, float *M)
{
    if (!hsd_in(h, off, 0x40)) return false;
    float r[3], s[3], t[3];
    for (int k = 0; k < 3; k++) {
        r[k] = bef(h->d + off + 0x14 + k * 4);
        s[k] = bef(h->d + off + 0x20 + k * 4);
        t[k] = bef(h->d + off + 0x2C + k * 4);
        if (!isfinite(r[k]) || !isfinite(s[k]) || !isfinite(t[k]) || fabsf(t[k]) > 1e7f || fabsf(s[k]) > 1e5f)
            return false;
    }
    euler_mtx(s, r, t, M);
    return true;
}

static void hsd_skeleton(hsdm_t *h, u32 off, const float *parent, int depth)
{
    while (off && depth < 64 && h->nj < HSD_JOINTS) {
        if (hsd_world(h, off)) return;                  /* already seen: a loop */
        float L[12];
        if (!hsd_srt(h, off, L)) {
            /* a fighter's ftData: its joint tree hangs off +0x5C */
            u32 sk = hsd_in(h, off, 0x60) ? hp(h, off + 0x5C) : 0;
            if (sk && sk != off) hsd_skeleton(h, sk, parent, depth + 1);
            return;
        }
        int j = h->nj++;
        h->joff[j] = off;
        if (parent) mul34(parent, L, h->jw[j]);
        else memcpy(h->jw[j], L, sizeof(L));
        hsd_skeleton(h, hp(h, off + 0x08), h->jw[j], depth + 1);
        off = hp(h, off + 0x0C);                        /* siblings, iteratively */
        depth++;
    }
}

/* ---- vertex attributes */

typedef struct {
    u32 name, type, ctype, buf;
    int stride, frac;
} hsda_t;

enum { HV_PNMTX = 0, HV_POS = 9, HV_NRM = 10, HV_CLR0 = 11, HV_TEX0 = 13, HV_NBT = 25 };

static int hsd_attrs(const hsdm_t *h, u32 off, hsda_t *a)
{
    int n = 0;
    while (n < 16 && hsd_in(h, off, 0x18)) {
        const u8 *s = h->d + off;
        u32 name = be32(s);
        if (name == 0xFF) break;
        a[n].name = name;
        a[n].type = be32(s + 4);
        a[n].ctype = be32(s + 0x0C);
        a[n].frac = s[0x10];
        a[n].stride = (s16)be16(s + 0x12);
        a[n].buf = hp(h, off + 0x14);
        n++;
        off += 0x18;
    }
    return n;
}

static float hsd_comp(const hsda_t *a, const u8 *p, int k)
{
    float d = (float)(1u << (a->frac & 31));
    switch (a->ctype) {
    case 0: return p[k] / d;
    case 1: return (s8)p[k] / d;
    case 2: return be16(p + k * 2) / d;
    case 3: return (s16)be16(p + k * 2) / d;
    default: return bef(p + k * 4);
    }
}

typedef struct {
    float p[3], n[3], t[2];
    u8 c[4];
    int env;                            /* PNMTXIDX / 3, or -1 */
    bool has_c;
} hsdv_t;

/* One vertex from the display list at *dl. */
static bool hsd_vertex(const hsdm_t *h, const hsda_t *a, int na, const u8 **dl, const u8 *end, hsdv_t *v)
{
    memset(v, 0, sizeof(*v));
    v->n[1] = 1;
    v->env = -1;
    for (int i = 0; i < na; i++) {
        const u8 *src;
        if (a[i].type == 1) {                           /* direct: inline */
            int w = a[i].stride > 0 ? a[i].stride : 1;
            if (*dl + w > end) return false;
            src = *dl;
            *dl += w;
        } else if (a[i].type == 2 || a[i].type == 3) {
            u32 idx;
            if (a[i].type == 2) {
                if (*dl + 1 > end) return false;
                idx = **dl;
                *dl += 1;
            } else {
                if (*dl + 2 > end) return false;
                idx = be16(*dl);
                *dl += 2;
            }
            if (!a[i].buf || a[i].stride <= 0) continue;
            u64 at = a[i].buf + (u64)idx * (u32)a[i].stride;
            if (at + (u32)a[i].stride > h->ro) return false;
            src = h->d + at;
        } else
            continue;
        int esz = a[i].ctype < 2 ? 1 : a[i].ctype < 4 ? 2 : 4;
        int nc = a[i].stride > 0 ? a[i].stride / esz : 0;
        switch (a[i].name) {
        case HV_PNMTX: v->env = src[0] / 3; break;
        case HV_POS:
            for (int k = 0; k < 3 && k < nc; k++) v->p[k] = hsd_comp(&a[i], src, k);
            break;
        case HV_NRM:
        case HV_NBT:
            for (int k = 0; k < 3 && k < nc; k++) v->n[k] = hsd_comp(&a[i], src, k);
            break;
        case HV_TEX0:
            for (int k = 0; k < 2 && k < nc; k++) v->t[k] = hsd_comp(&a[i], src, k);
            break;
        case HV_CLR0:
            get_color(src, 1, 0, (int)a[i].ctype, 0, v->c);
            v->has_c = true;
            break;
        default: break;
        }
    }
    return true;
}

/* ---- materials */

typedef struct {
    int tex;
    u8 rgba[4];
    bool xlu;
    float ss, st, ts, tt;
} hsdmat_t;

/* An HSD_Image's GX format: a plain u32 (Melee), or packed into the high byte with the mip count
 * after it (TV no Tomo). */
static int hsd_img_fmt(const u8 *p)
{
    u32 f = be32(p);
    if (f <= 14) return (int)f;
    return p[0] <= 14 && !(f & 0xFFFF) ? p[0] : -1;
}

static void hsd_material(model_t *m, const hsdm_t *h, u32 mobj, hsdmat_t *mt)
{
    memset(mt, 0, sizeof(*mt));
    mt->tex = -1;
    mt->rgba[0] = mt->rgba[1] = mt->rgba[2] = 200;
    mt->rgba[3] = 255;
    mt->ss = mt->st = 1;
    if (!hsd_in(h, mobj, 0x18)) return;
    u32 mode = be32(h->d + mobj + 4), mat = hp(h, mobj + 0x0C);
    if (hsd_in(h, mat, 0x14)) {
        memcpy(mt->rgba, h->d + mat + 4, 3);
        float al = bef(h->d + mat + 0x0C);
        if (al > 0 && al < 1) mt->rgba[3] = (u8)(al * 255);
    }
    mt->xlu = (mode & (1u << 30)) && mt->rgba[3] < 255;
    u32 t = hp(h, mobj + 0x08);
    for (int layer = 0; layer < 8 && hsd_in(h, t, 0x58) && mt->tex < 0; layer++) {
        const u8 *to = h->d + t;
        u32 img = hp(h, t + 0x4C);
        if (hsd_in(h, img, 0x0C)) {
            u32 pix = hp(h, img);
            int w = be16(h->d + img + 4), hh = be16(h->d + img + 6), fmt = hsd_img_fmt(h->d + img + 8);
            const u8 *pal = NULL;
            int pf = 0, pn = 0;
            if (fmt == 8 || fmt == 9 || fmt == 10) {
                u32 tl = hp(h, t + 0x50), pd = tl ? hp(h, tl) : 0;
                if (hsd_in(h, tl, 0x0E) && pd) {
                    pf = (int)be32(h->d + tl + 4);
                    pn = be16(h->d + tl + 0x0C);
                    if (pf <= 2 && pn > 0 && hsd_in(h, pd, pn * 2)) pal = h->d + pd;
                }
                if (!pal) fmt = -1;
            }
            if (pix && fmt >= 0)
                mt->tex = add_texture(m, h->d, h->ro, pix, fmt, w, hh, pal, pf, pn, (int)be32(to + 0x34),
                                      (int)be32(to + 0x38), 0);
            if (mt->tex >= 0) {
                mt->ss = bef(to + 0x1C);
                mt->st = bef(to + 0x20);
                mt->ts = bef(to + 0x28);
                mt->tt = bef(to + 0x2C);
                if (!(fabsf(mt->ss) > 1e-6f && fabsf(mt->ss) < 1e4f)) mt->ss = 1;
                if (!(fabsf(mt->st) > 1e-6f && fabsf(mt->st) < 1e4f)) mt->st = 1;
                if (!(fabsf(mt->ts) < 1e4f)) mt->ts = 0;
                if (!(fabsf(mt->tt) < 1e4f)) mt->tt = 0;
            }
        }
        u32 next = hp(h, t + 0x04);
        if (next == t) break;
        t = next;
    }
}

/* ---- polygons */

/* Envelope e of a PObj as one matrix: the weighted sum of each joint's world matrix times its
 * inverse bind matrix (identity at the bind pose, where such vertices are stored). */
static bool hsd_envelope(const hsdm_t *h, u32 pobj, int e, float *M)
{
    u32 arr = hp(h, pobj + 0x14);
    if (!arr || e < 0 || e > 255) return false;
    for (int i = 0; i <= e; i++)
        if (!hp(h, arr + i * 4)) return false;
    u32 ed = hp(h, arr + e * 4);
    memset(M, 0, 12 * sizeof(float));
    float total = 0;
    for (int k = 0; k < 16 && hsd_in(h, ed + k * 8, 8); k++) {
        u32 j = hp(h, ed + k * 8);
        float w = bef(h->d + ed + k * 8 + 4);
        if (!j || !(w > 0)) break;
        const float *W = hsd_world(h, j);
        if (!W) continue;
        float B[12];
        u32 ib = hp(h, j + 0x38);
        if (hsd_in(h, ib, 48)) {
            float I[12];
            for (int q = 0; q < 12; q++) I[q] = bef(h->d + ib + q * 4);
            mul34(W, I, B);
        } else
            memcpy(B, W, sizeof(B));
        for (int q = 0; q < 12; q++) M[q] += w * B[q];
        total += w;
        if (total >= 0.999f) break;
    }
    if (!(total > 0)) return false;
    if (fabsf(total - 1) > 1e-4f)
        for (int q = 0; q < 12; q++) M[q] /= total;
    return true;
}

static void hsd_put(model_t *m, const hsdv_t *s, const float *M, const hsdmat_t *mt)
{
    vert_t v;
    if (M) {
        xform(M, s->p, v.p);
        xform_n(M, s->n, v.n);
    } else {
        memcpy(v.p, s->p, 12);
        memcpy(v.n, s->n, 12);
    }
    v.t[0] = s->t[0];
    v.t[1] = s->t[1];
    if (mt->tex >= 0) {
        v.t[0] = v.t[0] * mt->ss + mt->ts;
        v.t[1] = v.t[1] * mt->st + mt->tt;
        if (s->has_c) memcpy(v.c, s->c, 4);
        else v.c[0] = v.c[1] = v.c[2] = 255, v.c[3] = mt->rgba[3];
    } else if (s->has_c)
        memcpy(v.c, s->c, 4);
    else
        memcpy(v.c, mt->rgba, 4);
    push(m, &v);
}

static void hsd_tri(model_t *m, const hsdv_t *a, const hsdv_t *b, const hsdv_t *c, const float *const *M,
                    const hsdmat_t *mt)
{
    hsd_put(m, a, M[0], mt);
    hsd_put(m, b, M[1], mt);
    hsd_put(m, c, M[2], mt);
}

/* One PObj's display list. Rigid polygons go through `rigid`; envelope ones per vertex. */
static void hsd_pobj(model_t *m, const hsdm_t *h, u32 pobj, const float *rigid, const hsdmat_t *mt)
{
    hsda_t a[16];
    int na = hsd_attrs(h, hp(h, pobj + 0x08), a);
    u16 flags = be16(h->d + pobj + 0x0C);
    s16 words = (s16)be16(h->d + pobj + 0x0E);
    u32 dlo = hp(h, pobj + 0x10);
    if (!na || words <= 0 || !hsd_in(h, dlo, (u32)words * 32)) return;
    bool env = (flags & 0x3000) == 0x2000;
    if ((flags & 0x3000) == 0) {                         /* bound to another joint */
        u32 sb = hp(h, pobj + 0x14);
        const float *W = sb ? hsd_world(h, sb) : NULL;
        if (W) rigid = W;
    }
    static float envm[256][12];
    static bool have[256];
    memset(have, 0, sizeof(have));
    int vsize = 0;
    for (int i = 0; i < na; i++)
        vsize += a[i].type == 1 ? (a[i].stride > 0 ? a[i].stride : 1) : a[i].type == 2 ? 1 : a[i].type == 3 ? 2 : 0;

    const u8 *p = h->d + dlo, *end = p + (u32)words * 32;
    static hsdv_t vs[4096];
    while (p < end) {
        u8 op = *p;
        if (!op) { p++; continue; }
        if (op == 0x61 || op == 0x08 || op == 0x10) {   /* BP / CP / XF register loads */
            p += op == 0x08 ? 6 : op == 0x61 ? 5 : 5 + (be16(p + 1) + 1) * 4;
            continue;
        }
        if (p + 3 > end) break;
        u32 cnt = be16(p + 1);
        int prim = op & 0xF8;
        p += 3;
        if (prim < 0x80 || prim > 0xB8) break;
        if (prim > 0xA0 || cnt > 4096) {                /* lines, points: not drawn */
            p += cnt * vsize;
            continue;
        }
        u32 n = 0;
        bool ok = true;
        for (; n < cnt && (ok = hsd_vertex(h, a, na, &p, end, &vs[n])); n++)
            ;
        if (!ok) break;
        const float *M[3];
        #define HSD_MTX(v) (env ? ((v).env >= 0 && (v).env < 256 && (have[(v).env] || (have[(v).env] = hsd_envelope(h, pobj, (v).env, envm[(v).env]))) ? envm[(v).env] : \
                          (have[0] || (have[0] = hsd_envelope(h, pobj, 0, envm[0]))) ? envm[0] : rigid) : rigid)
        #define HSD_T(i0, i1, i2) do { M[0] = HSD_MTX(vs[i0]); M[1] = HSD_MTX(vs[i1]); M[2] = HSD_MTX(vs[i2]); \
                                     hsd_tri(m, &vs[i0], &vs[i1], &vs[i2], M, mt); } while (0)
        /* GX winding, reversed as for the other formats */
        if (prim == 0x90)
            for (u32 i = 0; i + 2 < n; i += 3) HSD_T(i + 2, i + 1, i);
        else if (prim == 0x80 || prim == 0x88)
            for (u32 i = 0; i + 3 < n; i += 4) {
                HSD_T(i, i + 2, i + 1);
                HSD_T(i, i + 3, i + 2);
            }
        else if (prim == 0x98)
            for (u32 i = 2; i < n; i++) {
                if (i & 1) HSD_T(i, i - 2, i - 1);
                else HSD_T(i, i - 1, i - 2);
            }
        else if (prim == 0xA0)
            for (u32 i = 2; i < n; i++) HSD_T(0, i, i - 1);
        #undef HSD_T
        #undef HSD_MTX
        if (m->n >= MAX_VERTS - 3) break;
    }
}

static void hsd_meshes(model_t *m, hsdm_t *h, u32 off, int depth, u32 *seen, int *nseen)
{
    while (off && depth < 64) {
        const float *W = hsd_world(h, off);
        if (!W) {
            u32 sk = hsd_in(h, off, 0x60) ? hp(h, off + 0x5C) : 0;
            if (sk && sk != off) hsd_meshes(m, h, sk, depth + 1, seen, nseen);
            return;
        }
        for (int i = 0; i < *nseen; i++)
            if (seen[i] == off) return;
        if (*nseen < HSD_JOINTS) seen[(*nseen)++] = off;
        u32 jflags = be32(h->d + off + 4);
        /* +0x10 is a spline or particle instead of DObjs for those joints */
        u32 dobj = jflags & (0x20 | 0x4000) ? 0 : hp(h, off + 0x10);
        for (int k = 0; dobj && hsd_in(h, dobj, 0x10) && k < 4096; k++) {
            hsdmat_t mt;
            hsd_material(m, h, hp(h, dobj + 0x08), &mt);
            u32 start = m->n;
            u32 pobj = hp(h, dobj + 0x0C);
            for (int q = 0; pobj && hsd_in(h, pobj, 0x18) && q < 4096; q++) {
                hsd_pobj(m, h, pobj, W, &mt);
                u32 nx = hp(h, pobj + 0x04);
                if (nx == pobj) break;
                pobj = nx;
            }
            add_mesh(m, start, mt.tex, mt.xlu ? PE_XLU | PE_BLEND | (5u << 5) | (4u << 8) : 0);
            if (m->n > start) m->meshes++;
            u32 nx = hp(h, dobj + 0x04);
            if (nx == dobj) break;
            dobj = nx;
        }
        hsd_meshes(m, h, hp(h, off + 0x08), depth + 1, seen, nseen);
        off = hp(h, off + 0x0C);
        depth++;
    }
}

static bool load_hsd(model_t *m, const u8 *b, u32 len, const char *name)
{
    hsdm_t h = { 0 };
    u32 nrel, nroot, nref;
    if (!hsd_head(b, len, &h.ro, &nrel, &nroot, &nref)) return false;
    h.d = b;
    h.size = len;
    h.loc = malloc((nrel ? nrel : 1) * 4);
    h.dst = malloc((nrel ? nrel : 1) * 4);
    h.joff = malloc(HSD_JOINTS * 4);
    h.jw = malloc(HSD_JOINTS * sizeof(*h.jw));
    u32 *seen = malloc(HSD_JOINTS * 4);
    bool ok = false;
    if (!h.loc || !h.dst || !h.joff || !h.jw || !seen) goto out;
    /* the relocation table, sorted by location (it usually is already) */
    for (u32 i = 0; i < nrel; i++) {
        u32 l = be32(b + h.ro + 4 * i) + HSD_BASE;
        if (l < HSD_BASE || l + 4 > h.ro) continue;
        u32 t = be32(b + l) + HSD_BASE;
        if (t < HSD_BASE || t >= h.ro) continue;
        u32 k = h.n;
        while (k && h.loc[k - 1] > l) {
            h.loc[k] = h.loc[k - 1];
            h.dst[k] = h.dst[k - 1];
            k--;
        }
        h.loc[k] = l;
        h.dst[k] = t;
        h.n++;
    }

    /* the root joints: each root, or the joints a "scene_data" root lists */
    u32 roots[256];
    int nr = 0;
    u32 table = h.ro + 4 * nrel, strs = table + 8 * (nroot + nref);
    for (u32 i = 0; i < nroot && nr < 256; i++) {
        u32 off = be32(b + table + 8 * i) + HSD_BASE, no = strs + be32(b + table + 8 * i + 4);
        if (!hsd_in(&h, off, 4)) continue;
        if (no + 11 <= len && !memcmp(b + no, "scene_data", 11)) {
            for (u32 l = hp(&h, off); l && hsd_in(&h, l, 4) && nr < 256; l += 4) {
                u32 desc = hp(&h, l);
                if (!desc) break;
                u32 j = hp(&h, desc);
                if (j) roots[nr++] = j;
            }
        } else
            roots[nr++] = off;
    }
    for (int i = 0; i < nr; i++) hsd_skeleton(&h, roots[i], NULL, 0);
    int nseen = 0;
    for (int i = 0; i < nr; i++) hsd_meshes(m, &h, roots[i], 0, seen, &nseen);
    m->bones = h.nj;
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->kind = "HSD";
    ok = m->n > 0;
out:
    free(h.loc);
    free(h.dst);
    free(h.joff);
    free(h.jw);
    free(seen);
    return ok;
}

/* ---------------------------------------------------------------- GX drawing */

/* Static, so it stays in MEM1: a GPU fed its commands from MEM2 stalls at random on a real Wii
 * (MiiKit's README). */
static u8 gx_fifo_mem[256 * 1024] ATTRIBUTE_ALIGN(32);
static void *gx_fifo;

void model_gx_init(void)
{
    if (!gx_fifo) {
        gx_fifo = gx_fifo_mem;
        GX_Init(gx_fifo, sizeof(gx_fifo_mem));
    }
}

void model_gx_setup(void)
{
    int fw, fh;
    void *xfb = ui_framebuffer(&fw, &fh);
    (void)xfb;
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    model_gx_init();
    GXColor bg = { 24, 28, 40, 255 };
    GX_SetCopyClear(bg, GX_MAX_Z24);
    GX_SetViewport(0, 0, rm->fbWidth, rm->efbHeight, 0, 1);
    f32 ys = GX_GetYScaleFactor(rm->efbHeight, rm->xfbHeight);
    u32 xh = GX_SetDispCopyYScale(ys);
    GX_SetScissor(0, 0, rm->fbWidth, rm->efbHeight);
    GX_SetDispCopySrc(0, 0, rm->fbWidth, rm->efbHeight);
    GX_SetDispCopyDst(rm->fbWidth, xh);
    GX_SetCopyFilter(rm->aa, rm->sample_pattern, GX_TRUE, rm->vfilter);
    GX_SetFieldMode(rm->field_rendering,
                    (rm->viHeight == 2 * rm->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(rm->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetNumChans(1);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    /* cut-out textures (leaves, fences, text): drop clear pixels, and test alpha before depth is
     * written */
    GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
}

enum { SH_LIT, SH_FLAT, SH_WIRE, SH_COUNT };

static void draw_range(const model_t *m, u32 done, u32 stop, int shade);

static void render(const model_t *m, float yaw, float pitch, float dist, int shade)
{
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    void *xfb = ui_framebuffer(NULL, NULL);
    float c[3], r = 0;
    for (int k = 0; k < 3; k++) {
        c[k] = (m->min[k] + m->max[k]) / 2;
        float e = (m->max[k] - m->min[k]) / 2;
        r += e * e;
    }
    r = sqrtf(r);
    if (r < 1e-4f) r = 1;

    /* the camera sits dist * r from the center: keep all of the model between the clipping
     * planes */
    Mtx44 proj;
    float zfar = (dist + 1.5f) * r, znear = dist > 1.2f ? (dist - 1.1f) * r : zfar / 1000;
    if (znear < zfar / 1000) znear = zfar / 1000;
    guPerspective(proj, 45, (f32)rm->fbWidth / rm->efbHeight, znear, zfar);
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);

    Mtx view, model, rx, ry, mv, tr;
    guVector cam = { 0, 0, dist * r }, up = { 0, 1, 0 }, look = { 0, 0, 0 };
    guLookAt(view, &cam, &up, &look);
    guMtxTrans(tr, -c[0], -c[1], -c[2]);
    guVector ax = { 1, 0, 0 }, ay = { 0, 1, 0 };
    guMtxRotAxisDeg(rx, &ax, pitch);
    guMtxRotAxisDeg(ry, &ay, yaw);
    guMtxConcat(ry, tr, model);
    guMtxConcat(rx, model, model);
    guMtxConcat(view, model, mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    Mtx nm;
    guMtxInverse(mv, nm);
    guMtxTranspose(nm, nm);
    GX_LoadNrmMtxImm(nm, GX_PNMTX0);

    /* light from the camera, a little up and left */
    GXLightObj light;
    guVector lp = { -dist * r, dist * r, dist * r };
    GX_InitLightPos(&light, lp.x, lp.y, lp.z);
    GXColor white = { 255, 255, 255, 255 }, amb = { 70, 70, 70, 255 };
    GX_InitLightColor(&light, white);
    GX_LoadLightObj(&light, GX_LIGHT0);
    GX_SetChanAmbColor(GX_COLOR0A0, amb);
    if (shade == SH_LIT)
        GX_SetChanCtrl(GX_COLOR0, GX_ENABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP,
                       GX_AF_NONE);
    else
        GX_SetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE,
                       GX_AF_NONE);
    GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE,
                   GX_AF_NONE);

    GX_InvVtxCache();
    GX_InvalidateTexAll();

    /* opaque meshes first, then the translucent / blended ones without writing depth */
    for (int pass = 0, k = 0; pass < 2; k = 0, pass++)
    for (; k < m->nmesh; k++) {
        const mesh_t *ms = &m->mesh[k];
        bool blend = shade != SH_WIRE && (ms->pe & PE_BLEND);
        if ((blend || (ms->pe & PE_XLU)) != (pass == 1)) continue;
        if (blend) {
            GX_SetBlendMode(ms->pe & 0x800 ? GX_BM_SUBTRACT : GX_BM_BLEND, (ms->pe >> 8) & 7,
                            (ms->pe >> 5) & 7, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        } else {
            GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            GX_SetZMode(GX_TRUE, GX_LEQUAL, pass == 0 ? GX_TRUE : GX_FALSE);
        }

        if (ms->tex >= 0 && ms->tex < m->ntex && shade != SH_WIRE) {
            GX_LoadTexObj((GXTexObj *)&m->tex[ms->tex].obj, GX_TEXMAP0);
            if (m->tex[ms->tex].cutout)
                GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);
            else GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
            GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        } else {
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        }

        draw_range(m, ms->start, ms->start + ms->count, shade);
    }
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

    GX_DrawDone();
    GX_CopyDisp(xfb, GX_TRUE);
    GX_Flush();
    GX_DrawDone();
}

static void draw_range(const model_t *m, u32 done, u32 stop, int shade)
{
    while (done < stop) {
        u32 n = stop - done;

        if (shade == SH_WIRE) {
            if (n > 21843) n = 21843;               /* 3 lines (6 verts) per triangle, < 65536 */
            n -= n % 3;
            GX_Begin(GX_LINES, GX_VTXFMT0, (u16)(n * 2));
            for (u32 i = 0; i < n; i += 3)
                for (int e = 0; e < 3; e++)
                    for (int s = 0; s < 2; s++) {
                        const vert_t *v = &m->v[done + i + (e + s) % 3];
                        GX_Position3f32(v->p[0], v->p[1], v->p[2]);
                        GX_Normal3f32(v->n[0], v->n[1], v->n[2]);
                        GX_Color4u8(v->c[0], v->c[1], v->c[2], 255);
                        GX_TexCoord2f32(v->t[0], v->t[1]);
                    }
        } else {
            if (n > 65535) n = 65535 - 65535 % 3;
            GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)n);
            for (u32 i = 0; i < n; i++) {
                const vert_t *v = &m->v[done + i];
                GX_Position3f32(v->p[0], v->p[1], v->p[2]);
                GX_Normal3f32(v->n[0], v->n[1], v->n[2]);
                /* only the texture's alpha cuts pixels out */
                GX_Color4u8(v->c[0], v->c[1], v->c[2], 255);
                GX_TexCoord2f32(v->t[0], v->t[1]);
            }
        }

        GX_End();
        done += n;
    }
}

/* ---------------------------------------------------------------- viewer */

static u8 *load_file(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > FILE_MAX || f.size < 0x20) {
        vfs_close(&f);
        return NULL;
    }

    u32 n = (u32)f.size, got = 0;
    u8 *b = memalign(32, (n + 31) & ~31u);

    ui_busy_begin(vfs_basename(path));
    while (b && got < n) {
        ui_busy_progress(got, n);
        s32 r = vfs_read(&f, b + got, n - got > 0x40000 ? 0x40000 : n - got);
        if (r <= 0) break;
        got += r;
    }

    ui_busy_end();
    vfs_close(&f);

    if (b && got != n) {
        free(b);
        return NULL;
    }

    if (b && arc_lz_name(vfs_basename(path), NULL, 0)) {     /* "x.brres.lz": the file inside */
        u32 un;
        u8 *u = arc_lz_unwrap(vfs_basename(path), b, n, &un);
        free(b);
        if (!u) return NULL;
        b = u;
        n = un;
    }

    *len = n;
    return b;
}

static bool load_model(model_t *m, const u8 *b, u32 len, u32 off, const char *name)
{
    if (gx_fifo) GX_DrawDone();                 /* nothing may still be reading the old textures */
    free_model(m);
    for (int k = 0; k < 3; k++) {
        m->min[k] = 1e30f;
        m->max[k] = -1e30f;
    }
    if (!memcmp(b + off, "MDL0", 4)) {
        u32 sz = be32(b + off + 4);
        if (off + sz > len) sz = len - off;
        return load_mdl0(m, b, len, off, sz, name);
    }
    if (!memcmp(b, "HSFV037", 7)) return load_hsf(m, b, len, name);
    if (model_hsd_is(b, len)) return load_hsd(m, b, len, name);
    if (memcmp(b, "J3D", 3) && mod_is(b, len)) return load_mod(m, b, len, name);
    return load_j3d(m, b, len, name);
}

#define MAX_MODELS 64

bool model_view(const char *path)
{
    u32 len;
    ui_splash("Loading model...");
    u8 *b = load_file(path, &len);
    if (!b) return false;

    /* An MDL0 opened from inside a BRRES ("x.brres|/3DModels(NW4R)/name"): its textures are in the
     * same BRRES, so load all of it and start at this model. */
    const char *want = NULL;
    const char *bar = NULL;
    for (const char *q = path; (q = strstr(q, "|/")); q += 2) bar = q;
    if (!memcmp(b, "MDL0", 4) && bar) {
        char outer[VFS_PATH_MAX];
        snprintf(outer, sizeof(outer), "%.*s", (int)(bar - path), path);
        u32 olen;
        u8 *o = load_file(outer, &olen);
        if (o && !memcmp(o, "bres", 4)) {
            free(b);
            b = o;
            len = olen;
            want = vfs_basename(path);
        } else
            free(o);
    }
    /* where a bare MDL0 looks for textures that are not in its own BRRES: the folder of the file */
    {
        char whole[VFS_PATH_MAX], dir[VFS_PATH_MAX];
        snprintf(whole, sizeof(whole), "%s", path);
        char *first = strstr(whole, "|/");
        if (first) *first = 0;
        ext_reset(vfs_parent(whole, dir, sizeof(dir)) ? dir : NULL);
        if (arc_name_has_ext(path, ".mod")) mod_dirs_set(path);
    }

    u32 offs[MAX_MODELS];
    static char names[MAX_MODELS][64];
    int count;

    bool j3d = !memcmp(b, "J3D2", 4) || !memcmp(b, "J3D1", 4) || !memcmp(b, "HSFV037", 7) || model_hsd_is(b, len) ||
               (arc_name_has_ext(path, ".mod") && mod_is(b, len));
    if (j3d) {
        count = 1;
        offs[0] = 0;
        snprintf(names[0], 64, "%s", vfs_basename(path));
    } else
        count = brres_models(b, len, offs, names, MAX_MODELS);
    if (!count) {
        free(b);
        ext_reset(NULL);
        mod_dirs_free();
        return false;
    }

    model_t m = { 0 };
    int cur = 0;
    if (want)
        for (int i = 0; i < count; i++)
            if (!strncmp(names[i], want, strlen(names[i])) &&
                !strcmp(want + strlen(names[i]), ".mdl0")) {
                cur = i;
                break;
            }
    if (!load_model(&m, b, len, offs[cur], names[cur])) {
        /* a BRRES (or an HSD archive) with no drawable model: let the caller browse it */
        bool hsd = model_hsd_is(b, len);
        free_model(&m);
        free(b);
        ext_reset(NULL);
        mod_dirs_free();
        if (j3d && !hsd) {
            ui_message("MODEL", C_YELLOW, "No drawable geometry was found in this model.");
            return true;
        }
        return false;
    }

    model_gx_setup();

    float yaw = 30, pitch = 15, dist = 2.6f;
    bool first = true;
    int shade = SH_LIT;
    bool info = true, redraw = true;
    ptr_drag drag = { 0 };
    ptr_begin();

    for (;;) {
        if (redraw) {
            ptr_hide();
            /* the first copy after GX starts up doesn't reach the screen */
            if (first) render(&m, yaw, pitch, dist, shade);
            first = false;
            render(&m, yaw, pitch, dist, shade);
            ui_color(C_WHITE, true);
            ui_field(0, 0, ui_cols() - 1, " MODEL  %s  [%d/%d] %s", vfs_basename(path), cur + 1,
                     count, m.name);
            if (info)
                ui_field(1, 0, ui_cols() - 1,
                         " %s  %u triangles  %d meshes (%d textured, %d textures)  %d bones  %s",
                         m.kind, (unsigned)(m.n / 3), m.meshes, m.textured, m.ntex, m.bones,
                         shade == SH_LIT    ? "lit"
                         : shade == SH_FLAT ? "unlit"
                                            : "wireframe");
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 1, 0, ui_cols() - 1,
                     " D-Pad/Point+A drag Rotate  +/- Zoom  A Shading  1 Next  2 Info  B Close");
            ui_reset();
            redraw = false;
            UI_TEST_SHOT();
        }

        ptr_show();
        u32 p = DetectInput(IN_DOWN), h = INPUT_ButtonsHeld();
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;

        /* The pointer: drag with A held to turn the model; A without dragging changes the
         * shading. */
        int pdx, pdy, pev = ptr_drag_update(&drag, h, &pdx, &pdy);
        if (pev == PD_MOVE) {
            yaw += pdx * 0.5f;
            pitch += pdy * 0.5f;
            redraw = true;
        }
        if (pev == PD_TAP) {
            shade = (shade + 1) % SH_COUNT;
            redraw = true;
        }

        int wheel = ptr_wheel();
        if (wheel) {
            dist *= wheel < 0 ? 0.9f : 1.1f;
            if (dist < 0.3f) dist = 0.3f;
            if (dist > 40) dist = 40;
            redraw = true;
        }

        u32 dir = h & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN |
                       WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS);
        if (dir) {
            if (h & WPAD_BUTTON_LEFT) yaw -= 4;
            if (h & WPAD_BUTTON_RIGHT) yaw += 4;
            if (h & WPAD_BUTTON_UP) pitch -= 4;
            if (h & WPAD_BUTTON_DOWN) pitch += 4;
            if (h & WPAD_BUTTON_PLUS) dist *= 0.96f;
            if (h & WPAD_BUTTON_MINUS) dist *= 1.04f;
            if (dist < 0.3f) dist = 0.3f;
            if (dist > 40) dist = 40;
            redraw = true;
        }

        if ((p & WPAD_BUTTON_A) && !ptr_drag_owns_a(&drag)) {
            shade = (shade + 1) % SH_COUNT;
            redraw = true;
        }
        if (p & WPAD_BUTTON_2) {
            info = !info;
            redraw = true;
        }

        if ((p & WPAD_BUTTON_1) && count > 1) {
            for (int tries = 0; tries < count; tries++) {
                cur = (cur + 1) % count;
                ptr_hide();
                ui_splash("Loading model...");
                if (load_model(&m, b, len, offs[cur], names[cur])) break;
            }
            yaw = 30;
            pitch = 15;
            dist = 2.6f;
            redraw = true;
        }
    }

    if (gx_fifo) GX_DrawDone();
    ptr_hide();
    free_model(&m);
    free(b);
    ext_reset(NULL);
    mod_dirs_free();
    ui_wipe();
    return true;
}

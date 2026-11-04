/*
 * 3D model viewer: NW4R MDL0 (in .brres) and J3D BMD / BDL, drawn with GX.
 *
 * Layouts follow quatric's nintoolbox (lib-brres-model.c, lib-j3d.c):
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
                       const u8 *pal, int palfmt, int palcount, int wrap_s, int wrap_t)
{
    u32 key = at;
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

/* CLR0 / J3D colour formats: 0 RGB565, 1 RGB8, 2 RGBX8, 3 RGBA4, 4 RGBA6, 5 RGBA8 */
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
    if (!rel || nm - 4 < f || nm >= f + d->flen) return -1;

    u32 nlen = be32(nm - 4);
    if (!nlen || nlen > 255 || nm + nlen > f + d->flen) return -1;

    u32 tex = brres_find(f, d->flen, "Textures(NW4R)", (const char *)nm, nlen);
    if (!tex || memcmp(f + tex, "TEX0", 4)) return -1;

    const u8 *t = f + tex;
    int fmt = be32(t + 0x20), w = be16(t + 0x1C), h = be16(t + 0x1E);
    const u8 *pal = NULL;
    int palfmt = 0, palcount = 0;

    if (fmt >= 8 && fmt <= 10) {
        u32 pl = brres_find(f, d->flen, "Palettes(NW4R)", (const char *)nm, nlen);
        if (pl && !memcmp(f + pl, "PLT0", 4)) {
            u32 plen = be32(f + pl + 4), doff = be32(f + pl + 0x10);
            palcount = be16(f + pl + 0x1C);
            if (pl + doff + palcount * 2u <= d->flen && doff + palcount * 2u <= plen) {
                pal = f + pl + doff;
                palfmt = be32(f + pl + 0x18);
            } else
                palcount = 0;
        }
    }

    return add_texture(m, f, d->flen, tex + be32(t + 0x10), fmt, w, h, pal, palfmt, palcount,
                       (int)be32(ref + 0x18), (int)be32(ref + 0x1C));
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
                                be16(b + h + 4), pal, b[h + 9], palcount, b[h + 6], b[h + 7]);
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

/* ---------------------------------------------------------------- GX drawing */

static void *gx_fifo;

void model_gx_setup(void)
{
    int fw, fh;
    void *xfb = ui_framebuffer(&fw, &fh);
    (void)xfb;
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    if (!gx_fifo) {
        gx_fifo = memalign(32, 256 * 1024);
        memset(gx_fifo, 0, 256 * 1024);
        GX_Init(gx_fifo, 256 * 1024);
    }
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

    /* the camera sits dist * r from the centre: keep all of the model between the clipping
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
        u8 *u = arc_lz_unwrap(b, n, &un);
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
    return load_j3d(m, b, len, name);
}

#define MAX_MODELS 64

bool model_is_name(const char *name)
{
    char inner[256];
    if (arc_lz_name(name, inner, sizeof(inner))) name = inner;
    return arc_name_has_ext(name, ".brres") || arc_name_has_ext(name, ".mdl0") ||
           arc_name_has_ext(name, ".bmd") || arc_name_has_ext(name, ".bdl");
}

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

    u32 offs[MAX_MODELS];
    static char names[MAX_MODELS][64];
    int count;

    bool j3d = !memcmp(b, "J3D2", 4) || !memcmp(b, "J3D1", 4);
    if (j3d) {
        count = 1;
        offs[0] = 0;
        snprintf(names[0], 64, "%s", vfs_basename(path));
    } else
        count = brres_models(b, len, offs, names, MAX_MODELS);
    if (!count) {
        free(b);
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
        /* a BRRES with no drawable model: let the caller browse it */
        free_model(&m);
        free(b);
        if (j3d) {
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
    ui_wipe();
    return true;
}

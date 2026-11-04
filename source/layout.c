/*
 * NW4R layouts (BRLYT) drawn with GX, playing their animations (BRLAN).
 *
 * The layout's panes are drawn in file order (a parent before its children), each with its world
 * matrix (parent x translate x rotate x scale) and its material set up on the GPU as the file
 * describes it: texture maps (TPLs named by txl1, looked up in the archive's timg folder), texture
 * SRTs and coordinate generators, TEV colour / konst registers, swap tables, TEV stages, alpha
 * compare and blend mode. Materials without TEV stages get NW4R's default: the texture mapped
 * between the black (C0) and white (C1) colours, times the vertex colour.
 * Animations: RLPA (position, rotation, scale, size), RLVC (vertex colours, pane alpha), RLVI
 * (visibility), RLMC (material / TEV colours), RLTS (texture SRT) and RLTP (texture pattern), with
 * hermite and step curves. A layout opens with its "_Start" animation, which leads into "_Loop" as
 * on the Wii Menu; a BRLAN opens with its layout. Only the canvas shows (+/- zoom in and out of
 * it). Text boxes are not drawn. View only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include <input_wii.h>
#include "archive.h"

#define FILE_MAX  (8u << 20)
#define MAX_PANE  1024
#define MAX_MAT   512
#define MAX_TEX   256
#define MAX_ANIM  64

static float bef(const u8 *p) { u32 b = be32(p); float f; memcpy(&f, &b, 4); return f; }

/* ---------------------------------------------------------------- data */

typedef struct { float t[2], r, s[2]; } srt_t;

typedef struct {
    char name[24];
    s16 c[3][4];                /* TEV colour registers 0..2 */
    u8 k[4][4];                 /* konst colours */
    u8 mc[4];                   /* material colour */
    u8 ntm;
    struct { u16 tex; u8 ws, wt; } tm[8];
    u8 nsrt;
    srt_t srt[10];
    u8 ntcg;
    u8 tcg[8][3];
    bool chan, swap, acmp, blend;
    u8 csrc, asrc;
    u8 swaptab[4];
    u8 ntev;
    u8 tev[16][16];
    u8 ac[4], bl[4];
} mat_t;

enum { PK_NULL, PK_PIC, PK_TEXT, PK_WIND, PK_BOUND };

typedef struct {
    char name[20];
    int kind, parent, mat;
    u8 flags, origin, alpha;
    float t[3], r[3], s[2], w, h;
    u8 vc[4][4];                /* top left, top right, bottom left, bottom right */
    float tc[4][2];
    float inf[4];               /* window: content inflation left, right, top, bottom */
    int nfr, fmat[8];
    u8 fflip[8];
} pane_t;

typedef struct {
    GXTexObj obj;
    void *pix;
    int w, h;
    bool ok;
    char name[64];
} tex_t;

typedef struct {
    u32 tag;                    /* RLPA, RLVC, ... */
    int pane, mat;
    u8 idx, tgt, type;
    u16 n;
    const u8 *keys;
} track_t;

typedef struct {
    char path[VFS_PATH_MAX];
    char name[64];
    u8 *file;
    int frames;
    bool loop;
    track_t *tr;
    int ntr;
    int texslot[64];            /* RLTP: the animation's texture list -> layout texture slots */
} anim_t;

typedef struct {
    char path[VFS_PATH_MAX];
    u8 *file;
    u32 len;
    float W, H;
    bool centred;
    int npane, nmat, ntex;
    pane_t pane[MAX_PANE], base[MAX_PANE];
    mat_t mat[MAX_MAT], mbase[MAX_MAT];
    tex_t tex[MAX_TEX];
    bool vis[MAX_PANE];
    Mtx world[MAX_PANE];
    u8 galpha[MAX_PANE];        /* alpha passed down from parents that pass theirs on */
    anim_t anim[MAX_ANIM];
    int nanim;
} lyt_t;

/* ---------------------------------------------------------------- files */

static u8 *slurp(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > FILE_MAX || f.size < 0x10) { vfs_close(&f); return NULL; }
    u32 n = (u32)f.size, got = 0;
    u8 *b = memalign(32, (n + 31) & ~31u);
    while (b && got < n) {
        s32 r = vfs_read(&f, b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    vfs_close(&f);
    if (b && got != n) { free(b); return NULL; }
    if (len) *len = n;
    return b;
}

static bool exists(const char *path)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    vfs_close(&f);
    return true;
}

/* dir/../sub/name, dir/sub/name ... (sub may be empty) */
static bool join2(char *out, size_t sz, const char *dir, const char *sub, const char *name)
{
    char d[VFS_PATH_MAX];
    if (!sub || !*sub) return vfs_join(out, sz, dir, name);
    return vfs_join(d, sizeof(d), dir, sub) && vfs_join(out, sz, d, name);
}

/* ---------------------------------------------------------------- textures */

static bool is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

/* The first image of a TPL as a GX texture (palette formats are decoded to RGBA8). */
static bool tex_from_tpl(tex_t *t, const u8 *b, u32 n)
{
    if (n < 0x20 || be32(b) != 0x0020AF30u || !be32(b + 4)) return false;
    u32 tab = be32(b + 8);
    if (tab + 8 > n) return false;
    u32 ih = be32(b + tab), ph = be32(b + tab + 4);
    if (ih + 0x24 > n) return false;
    int h = be16(b + ih), w = be16(b + ih + 2), fmt = be32(b + ih + 4);
    u32 data = be32(b + ih + 8);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || data >= n) return false;
    if (fmt >= 8 && fmt <= 10) {
        if (!ph || ph + 12 > n) return false;
        int pc = be16(b + ph), pf = be32(b + ph + 4);
        u32 pd = be32(b + ph + 8);
        if (pd >= n) return false;
        u8 *rgba = image_decode_gx(b + data, n - data, fmt, w, h, b + pd, pf, pc);
        if (!rgba) return false;
        int tw = (w + 3) & ~3, th = (h + 3) & ~3;
        u8 *gx = memalign(32, tw * th * 4);
        if (!gx) { free(rgba); return false; }
        u8 *o = gx;
        for (int by = 0; by < th; by += 4)
            for (int bx = 0; bx < tw; bx += 4, o += 64)
                for (int i = 0; i < 16; i++) {
                    int x = bx + (i & 3), y = by + (i >> 2);
                    const u8 *c = x < w && y < h ? rgba + (y * w + x) * 4 : (const u8 *)"\0\0\0\0";
                    o[i * 2] = c[3]; o[i * 2 + 1] = c[0];
                    o[32 + i * 2] = c[1]; o[32 + i * 2 + 1] = c[2];
                }
        free(rgba);
        DCFlushRange(gx, tw * th * 4);
        t->pix = gx;
        fmt = GX_TF_RGBA8;
    } else {
        if (!((fmt >= 0 && fmt <= 6) || fmt == 14)) return false;
        u32 need = image_gx_size(fmt, w, h);
        if (need > n - data) return false;
        t->pix = memalign(32, need);
        if (!t->pix) return false;
        memcpy(t->pix, b + data, need);
        DCFlushRange(t->pix, need);
    }
    GX_InitTexObj(&t->obj, t->pix, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&t->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    t->w = w;
    t->h = h;
    t->ok = true;
    return true;
}

/* A texture named by the layout: in the archive's timg folder (arc/blyt/x.brlyt -> arc/timg/name),
 * or next to the layout. */
static void tex_load(lyt_t *L, tex_t *t)
{
    char dir[VFS_PATH_MAX], up[VFS_PATH_MAX], p[VFS_PATH_MAX];
    if (!vfs_parent(L->path, dir, sizeof(dir))) return;
    bool has_up = vfs_parent(dir, up, sizeof(up));
    const char *tries[][2] = { { has_up ? up : NULL, "timg" }, { dir, "" }, { dir, "timg" }, { has_up ? up : NULL, "" } };
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        if (!tries[i][0] || !join2(p, sizeof(p), tries[i][0], tries[i][1], t->name) || !exists(p)) continue;
        u32 n;
        u8 *b = slurp(p, &n);
        if (!b) continue;
        bool ok = tex_from_tpl(t, b, n);
        free(b);
        if (ok) return;
    }
}

static int tex_slot(lyt_t *L, const char *name)
{
    for (int i = 0; i < L->ntex; i++)
        if (!strcasecmp(L->tex[i].name, name)) return i;
    if (L->ntex >= MAX_TEX) return -1;
    tex_t *t = &L->tex[L->ntex];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    return L->ntex++;
}

static GXTexObj white_obj;
static u8 white_pix[32] __attribute__((aligned(32)));

/* ---------------------------------------------------------------- BRLYT */

static void name_at(char *out, size_t sz, const u8 *b, u32 len, u32 o)
{
    out[0] = 0;
    if (o >= len) return;
    size_t i = 0;
    for (; i + 1 < sz && o + i < len && b[o + i]; i++) out[i] = b[o + i];
    out[i] = 0;
}

static bool parse_mat(mat_t *m, const u8 *b, u32 end)
{
    memset(m, 0, sizeof(*m));
    if (0x40 > end) return false;
    memcpy(m->name, b, 20);
    m->name[20] = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) m->c[i][j] = (s16)be16(b + 0x14 + i * 8 + j * 2);
    memcpy(m->k, b + 0x2C, 16);
    memset(m->mc, 255, 4);
    m->csrc = m->asrc = GX_SRC_VTX;
    u32 fl = be32(b + 0x3C), p = 0x40;
    u32 ntm = fl & 15, nsrt = (fl >> 4) & 15, ntcg = (fl >> 8) & 15, swap = (fl >> 12) & 1, nisrt = (fl >> 13) & 3,
        nist = (fl >> 15) & 7, ntev = (fl >> 18) & 31, ac = (fl >> 23) & 1, bl = (fl >> 24) & 1, cc = (fl >> 25) & 1,
        mc = (fl >> 27) & 1;
    if (ntm > 8 || nsrt > 10 || ntcg > 8 || ntev > 16) return false;
    if (p + ntm * 4 + nsrt * 20 + ntcg * 4 + (cc + mc + swap) * 4 + nisrt * 20 + nist * 4 + ntev * 16 + (ac + bl) * 4 > end)
        return false;
    m->ntm = ntm;
    for (u32 i = 0; i < ntm; i++, p += 4) { m->tm[i].tex = be16(b + p); m->tm[i].ws = b[p + 2]; m->tm[i].wt = b[p + 3]; }
    m->nsrt = nsrt;
    for (u32 i = 0; i < nsrt; i++, p += 20) {
        m->srt[i].t[0] = bef(b + p); m->srt[i].t[1] = bef(b + p + 4); m->srt[i].r = bef(b + p + 8);
        m->srt[i].s[0] = bef(b + p + 12); m->srt[i].s[1] = bef(b + p + 16);
    }
    m->ntcg = ntcg;
    for (u32 i = 0; i < ntcg; i++, p += 4) memcpy(m->tcg[i], b + p, 3);
    if (cc) { m->chan = true; m->csrc = b[p]; m->asrc = b[p + 1]; p += 4; }
    if (mc) { memcpy(m->mc, b + p, 4); p += 4; }
    if (swap) { m->swap = true; memcpy(m->swaptab, b + p, 4); p += 4; }
    p += nisrt * 20 + nist * 4;
    m->ntev = ntev;
    for (u32 i = 0; i < ntev; i++, p += 16) memcpy(m->tev[i], b + p, 16);
    if (ac) { m->acmp = true; memcpy(m->ac, b + p, 4); p += 4; }
    if (bl) { m->blend = true; memcpy(m->bl, b + p, 4); p += 4; }
    return true;
}

/* 4 vertex colours, material, texcoord sets (the first is used) at p */
static u32 parse_quad(pane_t *pn, const u8 *b, u32 p, u32 end)
{
    if (p + 20 > end) return end;
    memcpy(pn->vc, b + p, 16);
    pn->mat = be16(b + p + 16);
    u32 ntc = b[p + 18];
    p += 20;
    static const float def[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    memcpy(pn->tc, def, sizeof(def));
    if (ntc && p + 32 <= end)
        for (int i = 0; i < 4; i++) { pn->tc[i][0] = bef(b + p + i * 8); pn->tc[i][1] = bef(b + p + i * 8 + 4); }
    return p + ntc * 32;
}

static bool parse_layout(lyt_t *L)
{
    const u8 *b = L->file;
    u32 len = L->len;
    if (len < 0x10 || memcmp(b, "RLYT", 4) || be16(b + 4) != 0xFEFF) return false;
    u32 nsec = be16(b + 0x0E), pos = be16(b + 0x0C);
    int stack[64], depth = 0, last = -1;
    stack[0] = -1;
    int txl_first = -1;
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (sz < 8 || pos + sz > len) break;
        const u8 *m = b + pos;
        if (!memcmp(m, "lyt1", 4) && sz >= 0x14) {
            L->centred = m[8] != 0;
            L->W = bef(m + 0x0C);
            L->H = bef(m + 0x10);
        } else if (!memcmp(m, "txl1", 4)) {
            u32 n = be16(m + 8), tab = pos + 12;
            txl_first = L->ntex;
            for (u32 k = 0; k < n && tab + k * 8 + 4 <= len; k++) {
                char nm[64];
                name_at(nm, sizeof(nm), b, len, tab + be32(b + tab + k * 8));
                /* keep one slot per txl1 entry so material indices line up */
                if (L->ntex < MAX_TEX) {
                    tex_t *t = &L->tex[L->ntex++];
                    memset(t, 0, sizeof(*t));
                    snprintf(t->name, sizeof(t->name), "%s", nm);
                }
            }
        } else if (!memcmp(m, "mat1", 4)) {
            u32 n = be16(m + 8);
            for (u32 k = 0; k < n && L->nmat < MAX_MAT && 12 + k * 4 + 4 <= sz; k++) {
                u32 o = be32(m + 12 + k * 4);
                u32 e = k + 1 < n ? be32(m + 12 + (k + 1) * 4) : sz;
                if (o >= sz || e > sz || e <= o) { memset(&L->mat[L->nmat++], 0, sizeof(mat_t)); continue; }
                if (!parse_mat(&L->mat[L->nmat], m + o, e - o)) memset(&L->mat[L->nmat], 0, sizeof(mat_t));
                L->nmat++;
            }
        } else if (!memcmp(m, "pas1", 4)) {
            if (depth < 63) stack[++depth] = last;
        } else if (!memcmp(m, "pae1", 4)) {
            if (depth) depth--;
        } else if ((!memcmp(m, "pan1", 4) || !memcmp(m, "pic1", 4) || !memcmp(m, "txt1", 4) || !memcmp(m, "wnd1", 4) ||
                    !memcmp(m, "bnd1", 4)) && sz >= 0x4C && L->npane < MAX_PANE) {
            pane_t *pn = &L->pane[L->npane];
            memset(pn, 0, sizeof(*pn));
            pn->kind = m[0] == 'p' && m[1] == 'i' ? PK_PIC : m[0] == 't' ? PK_TEXT : m[0] == 'w' ? PK_WIND :
                       m[0] == 'b' ? PK_BOUND : PK_NULL;
            pn->parent = stack[depth];
            pn->mat = -1;
            pn->flags = m[8];
            pn->origin = m[9] <= 8 ? m[9] : 4;
            pn->alpha = m[0x0A];
            memcpy(pn->name, m + 0x0C, 16);
            pn->name[16] = 0;
            for (int k = 0; k < 3; k++) { pn->t[k] = bef(m + 0x24 + k * 4); pn->r[k] = bef(m + 0x30 + k * 4); }
            pn->s[0] = bef(m + 0x3C);
            pn->s[1] = bef(m + 0x40);
            pn->w = bef(m + 0x44);
            pn->h = bef(m + 0x48);
            if (pn->kind == PK_PIC) {
                parse_quad(pn, m, 0x4C, sz);
            } else if (pn->kind == PK_WIND && sz >= 0x68) {
                for (int k = 0; k < 4; k++) pn->inf[k] = bef(m + 0x4C + k * 4);
                pn->nfr = m[0x5C] > 8 ? 8 : m[0x5C];
                u32 co = be32(m + 0x60), fo = be32(m + 0x64);
                if (co < sz) parse_quad(pn, m, co, sz);
                for (int k = 0; k < pn->nfr && fo + k * 4 + 4 <= sz; k++) {
                    u32 f = be32(m + fo + k * 4);
                    if (f + 4 > sz) { pn->nfr = k; break; }
                    pn->fmat[k] = be16(m + f);
                    pn->fflip[k] = m[f + 2];
                }
            } else if (pn->kind == PK_TEXT && sz >= 0x58) {
                pn->mat = be16(m + 0x50);
            }
            last = L->npane++;
        }
        pos += sz;
    }
    (void)txl_first;
    if (L->W <= 0 || L->H <= 0) { L->W = 640; L->H = 480; }
    memcpy(L->base, L->pane, sizeof(pane_t) * L->npane);
    memcpy(L->mbase, L->mat, sizeof(mat_t) * L->nmat);
    return L->npane > 0;
}

/* ---------------------------------------------------------------- BRLAN */

static int find_pane(const lyt_t *L, const char *n)
{
    for (int i = 0; i < L->npane; i++)
        if (!strcmp(L->pane[i].name, n)) return i;
    return -1;
}

static int find_mat(const lyt_t *L, const char *n)
{
    for (int i = 0; i < L->nmat; i++)
        if (!strcmp(L->mat[i].name, n)) return i;
    int p = find_pane(L, n);
    return p >= 0 && L->pane[p].mat >= 0 && L->pane[p].mat < L->nmat ? L->pane[p].mat : -1;
}

static void free_anim(anim_t *a)
{
    free(a->file);
    free(a->tr);
    memset(a, 0, sizeof(*a));
}

static bool load_anim(lyt_t *L, anim_t *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    u32 len;
    u8 *b = slurp(path, &len);
    if (!b) return false;
    if (memcmp(b, "RLAN", 4) || be16(b + 4) != 0xFEFF) { free(b); return false; }
    a->file = b;
    snprintf(a->path, sizeof(a->path), "%s", path);
    snprintf(a->name, sizeof(a->name), "%s", vfs_basename(path));
    char *dot = strrchr(a->name, '.');
    if (dot) *dot = 0;
    for (int i = 0; i < 64; i++) a->texslot[i] = -1;
    u32 nsec = be16(b + 0x0E), pos = be16(b + 0x0C);
    int cap = 0;
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (sz < 8 || pos + sz > len) break;
        if (!memcmp(b + pos, "pai1", 4) && sz >= 0x14) {
            const u8 *m = b + pos;
            a->frames = be16(m + 8);
            a->loop = m[0x0A] != 0;
            u32 ntex = be16(m + 0x0C), nent = be16(m + 0x0E), eo = be32(m + 0x10);
            for (u32 t = 0; t < ntex && t < 64 && 0x14 + t * 4 + 4 <= sz; t++) {
                char nm[64];
                name_at(nm, sizeof(nm), m, sz, 0x14 + be32(m + 0x14 + t * 4));
                a->texslot[t] = nm[0] ? tex_slot(L, nm) : -1;
            }
            for (u32 e = 0; e < nent && eo + e * 4 + 4 <= sz; e++) {
                u32 en = be32(m + eo + e * 4);
                if (en + 0x18 > sz) continue;
                char nm[24];
                memcpy(nm, m + en, 20);
                nm[20] = 0;
                u32 ntag = m[en + 0x14];
                int pane = find_pane(L, nm), mat = find_mat(L, nm);
                for (u32 t = 0; t < ntag && en + 0x18 + t * 4 + 4 <= sz; t++) {
                    u32 to = en + be32(m + en + 0x18 + t * 4);
                    if (to + 8 > sz) continue;
                    u32 tag = be32(m + to), nk = m[to + 4];
                    for (u32 k = 0; k < nk && to + 8 + k * 4 + 4 <= sz; k++) {
                        u32 ko = to + be32(m + to + 8 + k * 4);
                        if (ko + 12 > sz) continue;
                        u8 type = m[ko + 2];
                        u16 n = be16(m + ko + 4);
                        u32 keys = ko + be32(m + ko + 8);
                        u32 ksz = type == 2 ? 12 : 8;
                        if (!n || keys + n * ksz > sz) continue;
                        if (a->ntr == cap) {
                            int nc = cap ? cap * 2 : 64;
                            track_t *nt = realloc(a->tr, nc * sizeof(track_t));
                            if (!nt) break;
                            a->tr = nt;
                            cap = nc;
                        }
                        track_t *tr = &a->tr[a->ntr++];
                        tr->tag = tag;
                        tr->pane = pane;
                        tr->mat = mat;
                        tr->idx = m[ko];
                        tr->tgt = m[ko + 1];
                        tr->type = type;
                        tr->n = n;
                        tr->keys = m + keys;
                    }
                }
            }
        }
        pos += sz;
    }
    if (a->frames <= 0) a->frames = 1;
    return true;
}

static float curve(const track_t *t, float f)
{
    if (t->type != 2) {                         /* step: the last key at or before f */
        int i = 0;
        while (i + 1 < t->n && bef(t->keys + (i + 1) * 8) <= f) i++;
        return be16(t->keys + i * 8 + 4);
    }
    const u8 *k = t->keys;
    if (f <= bef(k)) return bef(k + 4);
    const u8 *last = k + (t->n - 1) * 12;
    if (f >= bef(last)) return bef(last + 4);
    int i = 0;
    while (i + 2 < t->n && bef(k + (i + 1) * 12) <= f) i++;
    const u8 *a = k + i * 12, *b = a + 12;
    float f0 = bef(a), f1 = bef(b), dt = f1 - f0;
    if (dt <= 0) return bef(b + 4);
    float s = (f - f0) / dt, s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * bef(a + 4) + (s3 - 2 * s2 + s) * dt * bef(a + 8) +
           (-2 * s3 + 3 * s2) * bef(b + 4) + (s3 - s2) * dt * bef(b + 8);
}

static u8 clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)(v + 0.5f); }

#define TAG(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

static void apply(lyt_t *L, const anim_t *a, float f)
{
    for (int i = 0; i < a->ntr; i++) {
        const track_t *t = &a->tr[i];
        float v = curve(t, f);
        pane_t *p = t->pane >= 0 ? &L->pane[t->pane] : NULL;
        mat_t *m = t->mat >= 0 ? &L->mat[t->mat] : NULL;
        switch (t->tag) {
        case TAG('R', 'L', 'P', 'A'):
            if (!p) break;
            if (t->tgt < 3) p->t[t->tgt] = v;
            else if (t->tgt < 6) p->r[t->tgt - 3] = v;
            else if (t->tgt < 8) p->s[t->tgt - 6] = v;
            else if (t->tgt == 8) p->w = v;
            else if (t->tgt == 9) p->h = v;
            break;
        case TAG('R', 'L', 'V', 'C'):
            if (!p) break;
            if (t->tgt < 16) p->vc[t->tgt / 4][t->tgt % 4] = clamp8(v);
            else if (t->tgt == 16) p->alpha = clamp8(v);
            break;
        case TAG('R', 'L', 'V', 'I'):
            if (p) p->flags = v != 0 ? (p->flags | 1) : (p->flags & ~1);
            break;
        case TAG('R', 'L', 'M', 'C'):
            if (!m) break;
            if (t->tgt < 4) m->mc[t->tgt] = clamp8(v);
            else if (t->tgt < 16) {
                float c = v < -1024 ? -1024 : v > 1023 ? 1023 : v;
                m->c[(t->tgt - 4) / 4][t->tgt % 4] = (s16)c;
            } else if (t->tgt < 32) m->k[(t->tgt - 16) / 4][t->tgt % 4] = clamp8(v);
            break;
        case TAG('R', 'L', 'T', 'S'):
            if (!m || t->idx >= m->nsrt) break;
            if (t->tgt < 2) m->srt[t->idx].t[t->tgt] = v;
            else if (t->tgt == 2) m->srt[t->idx].r = v;
            else if (t->tgt < 5) m->srt[t->idx].s[t->tgt - 3] = v;
            break;
        case TAG('R', 'L', 'T', 'P'): {
            int k = (int)v;
            if (m && t->idx < m->ntm && k >= 0 && k < 64 && a->texslot[k] >= 0) m->tm[t->idx].tex = a->texslot[k];
            break;
        }
        }
    }
}

/* ---------------------------------------------------------------- drawing */

static void bind_material(lyt_t *L, const mat_t *m)
{
    /* texture maps */
    for (int i = 0; i < m->ntm && i < 8; i++) {
        int s = m->tm[i].tex;
        tex_t *t = s < L->ntex ? &L->tex[s] : NULL;
        if (t && !t->ok && !t->pix && t->name[0]) { tex_load(L, t); if (!t->ok) t->name[0] = 0; }
        if (t && t->ok) {
            int ws = m->tm[i].ws <= 2 && is_pow2(t->w) ? m->tm[i].ws : GX_CLAMP;
            int wt = m->tm[i].wt <= 2 && is_pow2(t->h) ? m->tm[i].wt : GX_CLAMP;
            GX_InitTexObjWrapMode(&t->obj, ws, wt);
            GX_LoadTexObj(&t->obj, GX_TEXMAP0 + i);
        } else
            GX_LoadTexObj(&white_obj, GX_TEXMAP0 + i);
    }
    /* texture SRTs -> texture matrices (about the texture's centre) */
    for (int i = 0; i < m->nsrt && i < 10; i++) {
        const srt_t *s = &m->srt[i];
        float r = s->r * (float)M_PI / 180, c = cosf(r), n = sinf(r);
        Mtx t;
        t[0][0] = s->s[0] * c;  t[0][1] = -s->s[1] * n; t[0][2] = 0;
        t[1][0] = s->s[0] * n;  t[1][1] = s->s[1] * c;  t[1][2] = 0;
        t[0][3] = 0.5f + s->t[0] - (t[0][0] + t[0][1]) * 0.5f;
        t[1][3] = 0.5f - s->t[1] - (t[1][0] + t[1][1]) * 0.5f;
        t[2][0] = t[2][1] = t[2][3] = 0;
        t[2][2] = 1;
        GX_LoadTexMtxImm(t, GX_TEXMTX0 + i * 3, GX_MTX2x4);
    }
    int ngen = m->ntcg ? m->ntcg : m->ntm;
    for (int i = 0; i < ngen; i++) {
        if (m->ntcg) GX_SetTexCoordGen(GX_TEXCOORD0 + i, m->tcg[i][0], m->tcg[i][1], m->tcg[i][2]);
        else GX_SetTexCoordGen(GX_TEXCOORD0 + i, GX_TG_MTX2x4, GX_TG_TEX0, i < m->nsrt ? GX_TEXMTX0 + i * 3 : GX_IDENTITY);
    }
    GX_SetNumTexGens(ngen);

    for (int i = 0; i < 3; i++) {
        GXColorS10 c = { m->c[i][0], m->c[i][1], m->c[i][2], m->c[i][3] };
        GX_SetTevColorS10(GX_TEVREG0 + i, c);
    }
    for (int i = 0; i < 4; i++) {
        GXColor k = { m->k[i][0], m->k[i][1], m->k[i][2], m->k[i][3] };
        GX_SetTevKColor(GX_KCOLOR0 + i, k);
    }
    GXColor mc = { m->mc[0], m->mc[1], m->mc[2], m->mc[3] };
    GX_SetChanMatColor(GX_COLOR0A0, mc);
    GX_SetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, m->csrc ? GX_SRC_VTX : GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, m->asrc ? GX_SRC_VTX : GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

    if (m->swap)
        for (int i = 0; i < 4; i++) {
            u8 s = m->swaptab[i];
            GX_SetTevSwapModeTable(GX_TEV_SWAP0 + i, s & 3, (s >> 2) & 3, (s >> 4) & 3, (s >> 6) & 3);
        }
    else {
        GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
        GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
        GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
        GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    }

    int nst;
    if (m->ntev) {
        nst = m->ntev;
        for (int s = 0; s < nst; s++) {
            const u8 *t = m->tev[s];
            int tc = t[0] < 8 ? t[0] : GX_TEXCOORDNULL;
            int tm = t[2] | (t[3] & 1) << 8;
            tm = tm < 8 ? tm : GX_TEXMAP_NULL;
            int ch = t[1] == 0xFF ? GX_COLORNULL : t[1];
            GX_SetTevOrder(GX_TEVSTAGE0 + s, tc, tm, ch);
            GX_SetTevSwapMode(GX_TEVSTAGE0 + s, (t[3] >> 1) & 3, (t[3] >> 3) & 3);
            GX_SetTevColorIn(GX_TEVSTAGE0 + s, t[4] & 15, t[4] >> 4, t[5] & 15, t[5] >> 4);
            GX_SetTevColorOp(GX_TEVSTAGE0 + s, t[6] & 15, (t[6] >> 4) & 3, t[6] >> 6, t[7] & 1, (t[7] >> 1) & 3);
            GX_SetTevKColorSel(GX_TEVSTAGE0 + s, t[7] >> 3);
            GX_SetTevAlphaIn(GX_TEVSTAGE0 + s, t[8] & 15, t[8] >> 4, t[9] & 15, t[9] >> 4);
            GX_SetTevAlphaOp(GX_TEVSTAGE0 + s, t[10] & 15, (t[10] >> 4) & 3, t[10] >> 6, t[11] & 1, (t[11] >> 1) & 3);
            GX_SetTevKAlphaSel(GX_TEVSTAGE0 + s, t[11] >> 3);
            GX_SetTevDirect(GX_TEVSTAGE0 + s);
        }
    } else if (m->ntm) {
        /* the texture between the black (C0) and white (C1) colours, times the vertex colour */
        nst = 2;
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevDirect(GX_TEVSTAGE0);
        GX_SetTevDirect(GX_TEVSTAGE1);
    } else {
        /* no texture: the white colour (C1) times the vertex colour */
        nst = 1;
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_C1, GX_CC_RASC, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A1, GX_CA_RASA, GX_CA_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevDirect(GX_TEVSTAGE0);
    }
    GX_SetNumTevStages(nst);

    if (m->acmp) GX_SetAlphaCompare(m->ac[0] & 15, m->ac[2], m->ac[1], m->ac[0] >> 4, m->ac[3]);
    else GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    if (m->blend) GX_SetBlendMode(m->bl[0], m->bl[1], m->bl[2], m->bl[3]);
    else GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

/* A quad: x0, y0 its top left corner (y up), corner colours with the alpha applied, texcoords. */
static void quad(float x0, float y0, float w, float h, const u8 vc[4][4], u8 alpha, const float tc[4][2])
{
    const float xs[4] = { x0, x0 + w, x0 + w, x0 }, ys[4] = { y0, y0, y0 - h, y0 - h };
    static const int ci[4] = { 0, 1, 3, 2 };     /* TL TR BR BL around the quad */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) {
        const u8 *c = vc[ci[i]];
        GX_Position3f32(xs[i], ys[i], 0);
        GX_Color4u8(c[0], c[1], c[2], (c[3] * alpha + 127) / 255);
        GX_TexCoord2f32(tc[ci[i]][0], tc[ci[i]][1]);
    }
    GX_End();
}

/* the pane's rectangle, by its origin (0..8: left / centre / right, top / centre / bottom) */
static void pane_rect(const pane_t *p, float *x0, float *y0)
{
    int ox = p->origin % 3, oy = p->origin / 3;
    *x0 = ox == 0 ? 0 : ox == 1 ? -p->w / 2 : -p->w;
    *y0 = oy == 0 ? 0 : oy == 1 ? p->h / 2 : p->h;
}

static void draw_window(lyt_t *L, const pane_t *p, u8 alpha)
{
    float x0, y0;
    pane_rect(p, &x0, &y0);
    if (p->mat >= 0 && p->mat < L->nmat) {
        bind_material(L, &L->mat[p->mat]);
        quad(x0 - p->inf[0], y0 + p->inf[2], p->w + p->inf[0] + p->inf[1], p->h + p->inf[2] + p->inf[3], p->vc, alpha, p->tc);
    }
    /* frames: one corner texture (mirrored to the other corners) or one per corner, at its own size */
    if (!p->nfr) return;
    static const u8 white[4][4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
    for (int k = 0; k < 4; k++) {
        int fi = p->nfr >= 4 ? k : 0;
        int mi = p->fmat[fi];
        if (mi < 0 || mi >= L->nmat) continue;
        const mat_t *m = &L->mat[mi];
        if (!m->ntm || m->tm[0].tex >= L->ntex) continue;
        tex_t *t = &L->tex[m->tm[0].tex];
        if (!t->ok && t->name[0]) tex_load(L, t);
        if (!t->ok) continue;
        float fw = t->w, fh = t->h;
        if (fw > p->w / 2) fw = p->w / 2;
        if (fh > p->h / 2) fh = p->h / 2;
        bool fx = p->nfr < 4 ? (k & 1) : false, fy = p->nfr < 4 ? (k >> 1) : false;
        float tc[4][2] = { { fx, fy }, { !fx, fy }, { fx, !fy }, { !fx, !fy } };
        bind_material(L, m);
        quad(x0 + ((k & 1) ? p->w - fw : 0), y0 - ((k >> 1) ? p->h - fh : 0), fw, fh, white, alpha, tc);
    }
}

static void *gx_xfb;

static void render(lyt_t *L, float vx, float vy, float vw, float vh)
{
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    GX_SetViewport(vx, vy, vw, vh, 0, 1);
    /* only the canvas shows, as on the Wii Menu: panes reaching past it are cut off */
    float x0 = vx < 0 ? 0 : vx, y0 = vy < 0 ? 0 : vy;
    float x1 = vx + vw > rm->fbWidth ? rm->fbWidth : vx + vw, y1 = vy + vh > rm->efbHeight ? rm->efbHeight : vy + vh;
    GX_SetScissor((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0));
    Mtx44 proj;
    guOrtho(proj, L->H / 2, -L->H / 2, -L->W / 2, L->W / 2, 0, 4000);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetClipMode(GX_CLIP_DISABLE);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetNumIndStages(0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_InvVtxCache();
    GX_InvalidateTexAll();

    Mtx root, tmp, rx, ry, rz;
    guMtxTrans(root, 0, 0, -2000);
    for (int i = 0; i < L->npane; i++) {
        pane_t *p = &L->pane[i];
        int par = p->parent;
        L->vis[i] = (p->flags & 1) && (par < 0 || L->vis[par]);
        /* a parent with "influenced alpha" passes its alpha on to its children */
        u8 inherited = par >= 0 && (L->pane[par].flags & 2) ? L->galpha[par] : 255;
        u8 alpha = (p->alpha * inherited + 127) / 255;
        L->galpha[i] = alpha;
        /* local: translate x rotate (z, y, x) x scale */
        Mtx local;
        guMtxScale(local, p->s[0], p->s[1], 1);
        guVector ax = { 1, 0, 0 }, ay = { 0, 1, 0 }, az = { 0, 0, 1 };
        guMtxRotAxisDeg(rx, &ax, p->r[0]);
        guMtxRotAxisDeg(ry, &ay, p->r[1]);
        guMtxRotAxisDeg(rz, &az, p->r[2]);
        guMtxConcat(rx, local, local);
        guMtxConcat(ry, local, local);
        guMtxConcat(rz, local, local);
        guMtxTrans(tmp, p->t[0], p->t[1], p->t[2]);
        guMtxConcat(tmp, local, local);
        guMtxConcat(par < 0 ? root : L->world[par], local, L->world[i]);
        if (!L->vis[i] || !alpha) continue;
        if (p->kind != PK_PIC && p->kind != PK_WIND) continue;
        GX_LoadPosMtxImm(L->world[i], GX_PNMTX0);
        GX_SetCurrentMtx(GX_PNMTX0);
        if (p->kind == PK_WIND) { draw_window(L, p, alpha); continue; }
        if (p->mat < 0 || p->mat >= L->nmat) continue;
        bind_material(L, &L->mat[p->mat]);
        float x0, y0;
        pane_rect(p, &x0, &y0);
        quad(x0, y0, p->w, p->h, p->vc, alpha, p->tc);
    }
    GX_DrawDone();
    GX_CopyDisp(gx_xfb, GX_TRUE);
    GX_Flush();
    GX_DrawDone();
}

/* GX state the other viewers expect */
static void restore_gx(void)
{
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    GX_SetViewport(0, 0, rm->fbWidth, rm->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rm->fbWidth, rm->efbHeight);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    for (int s = 0; s < 4; s++) { GX_SetTevSwapMode(GX_TEVSTAGE0 + s, GX_TEV_SWAP0, GX_TEV_SWAP0); GX_SetTevDirect(GX_TEVSTAGE0 + s); }
    GX_SetNumTevStages(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

/* ---------------------------------------------------------------- finding the files */

static bool has_ext(const char *n, const char *e) { return arc_name_has_ext(n, e); }

/* the part of a file name before its extension */
static void stem(char *out, size_t sz, const char *name)
{
    snprintf(out, sz, "%s", name);
    char *d = strrchr(out, '.');
    if (d) *d = 0;
}

/* BRLANs for the layout at L->path: in ../anim and next to it, named after the layout (x_Start
 * for x.brlyt) when any are, else all of them. */
static void find_anims(lyt_t *L)
{
    char dir[VFS_PATH_MAX], up[VFS_PATH_MAX], ad[VFS_PATH_MAX], me[VFS_NAME_MAX];
    stem(me, sizeof(me), vfs_basename(L->path));
    if (!vfs_parent(L->path, dir, sizeof(dir))) return;
    const char *dirs[2] = { NULL, dir };
    if (vfs_parent(dir, up, sizeof(up)) && vfs_join(ad, sizeof(ad), up, "anim")) dirs[0] = ad;
    for (int pass = 0; pass < 2 && !L->nanim; pass++)
        for (int d = 0; d < 2; d++) {
            if (!dirs[d]) continue;
            vfs_entry *e = NULL;
            int n = 0;
            if (!vfs_list_visible(dirs[d], &e, &n)) continue;
            for (int i = 0; i < n && L->nanim < MAX_ANIM; i++) {
                if (e[i].is_dir || !has_ext(e[i].name, ".brlan")) continue;
                size_t ml = strlen(me);
                if (pass == 0 && (strncasecmp(e[i].name, me, ml) || e[i].name[ml] != '_')) continue;
                char p[VFS_PATH_MAX];
                if (!vfs_join(p, sizeof(p), dirs[d], e[i].name)) continue;
                bool dup = false;
                for (int k = 0; k < L->nanim; k++)
                    if (!strcmp(L->anim[k].path, p)) dup = true;
                if (!dup && load_anim(L, &L->anim[L->nanim], p)) L->nanim++;
            }
            free(e);
        }
}

/* The BRLYT a BRLAN belongs to: ../blyt/x.brlyt or next to it, x being the longest name the
 * animation's name starts with (x_Start.brlan -> x.brlyt); else the only one there. */
static bool find_layout(const char *anim, char *out, size_t sz)
{
    char dir[VFS_PATH_MAX], up[VFS_PATH_MAX], bd[VFS_PATH_MAX];
    const char *an = vfs_basename(anim);
    if (!vfs_parent(anim, dir, sizeof(dir))) return false;
    const char *dirs[2] = { NULL, dir };
    if (vfs_parent(dir, up, sizeof(up)) && vfs_join(bd, sizeof(bd), up, "blyt")) dirs[0] = bd;
    size_t best = 0;
    int count = 0;
    char only[VFS_PATH_MAX] = "";
    for (int d = 0; d < 2; d++) {
        if (!dirs[d]) continue;
        vfs_entry *e = NULL;
        int n = 0;
        if (!vfs_list_visible(dirs[d], &e, &n)) continue;
        for (int i = 0; i < n; i++) {
            if (e[i].is_dir || !has_ext(e[i].name, ".brlyt")) continue;
            char s[VFS_NAME_MAX];
            stem(s, sizeof(s), e[i].name);
            size_t l = strlen(s);
            count++;
            if (!only[0]) vfs_join(only, sizeof(only), dirs[d], e[i].name);
            if (l > best && !strncasecmp(an, s, l) && (an[l] == '_' || an[l] == '.')) {
                best = l;
                vfs_join(out, sz, dirs[d], e[i].name);
            }
        }
        free(e);
        if (best) return true;
    }
    /* an archive's blyt folder with one layout in it */
    if (count == 1 && dirs[0] && !strncmp(only, dirs[0], strlen(dirs[0]))) { snprintf(out, sz, "%s", only); return true; }
    return false;
}

static int find_anim(const lyt_t *L, const char *suffix)
{
    for (int i = 0; i < L->nanim; i++) {
        size_t n = strlen(L->anim[i].name), s = strlen(suffix);
        if (n >= s && !strcasecmp(L->anim[i].name + n - s, suffix)) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- viewer */

static void free_layout(lyt_t *L)
{
    for (int i = 0; i < L->nanim; i++) free_anim(&L->anim[i]);
    for (int i = 0; i < L->ntex; i++) free(L->tex[i].pix);
    free(L->file);
}

bool lyt_view(const char *path)
{
    u8 head[4] = { 0 };
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    vfs_read(&f, head, 4);
    vfs_close(&f);
    char lpath[VFS_PATH_MAX];
    const char *start_anim = NULL;
    if (!memcmp(head, "RLAN", 4)) {
        if (!find_layout(path, lpath, sizeof(lpath))) return false;
        start_anim = path;
    } else if (!memcmp(head, "RLYT", 4))
        snprintf(lpath, sizeof(lpath), "%s", path);
    else
        return false;

    ui_splash("Loading layout...");
    lyt_t *L = calloc(1, sizeof(lyt_t));
    if (!L) return false;
    snprintf(L->path, sizeof(L->path), "%s", lpath);
    L->file = slurp(lpath, &L->len);
    if (!L->file || !parse_layout(L)) { free_layout(L); free(L); return false; }
    find_anims(L);
    if (start_anim) {
        bool have = false;
        for (int i = 0; i < L->nanim; i++)
            if (!strcmp(L->anim[i].path, start_anim)) have = true;
        if (!have && L->nanim < MAX_ANIM && load_anim(L, &L->anim[L->nanim], start_anim)) L->nanim++;
    }
    int cur = -1;
    for (int i = 0; start_anim && i < L->nanim; i++)
        if (!strcmp(L->anim[i].path, start_anim)) cur = i;
    if (cur < 0) cur = find_anim(L, "_Start");
    if (cur < 0) cur = find_anim(L, "_Loop");
    if (cur < 0 && L->nanim) cur = 0;

    /* a white 4x4 I8 texture for maps whose TPL wasn't found */
    memset(white_pix, 255, sizeof(white_pix));
    DCFlushRange(white_pix, sizeof(white_pix));
    GX_InitTexObj(&white_obj, white_pix, 4, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);

    model_gx_setup();
    int fw, fh;
    gx_xfb = ui_framebuffer(&fw, &fh);
    GXRModeObj *rm = VIDEO_GetPreferredMode(NULL);
    GXColor bg = { 0, 0, 0, 255 };
    GX_SetCopyClear(bg, GX_MAX_Z24);

    float frame = 0, zoom = 1;
    bool paused = false, info = true, first = true;
    int ntex_ok = -1;
    for (;;) {
        /* the layout at this frame */
        memcpy(L->pane, L->base, sizeof(pane_t) * L->npane);
        memcpy(L->mat, L->mbase, sizeof(mat_t) * L->nmat);
        anim_t *a = cur >= 0 ? &L->anim[cur] : NULL;
        if (a) apply(L, a, frame);

        /* fit the layout's canvas in the screen, under the title lines when they show */
        /* the console's text starts 16 pixels in, 16 pixels a row: keep clear of rows 0, 1 and the last */
        float top = info ? 16 + 2 * 16 : 0, bottom = info ? rm->efbHeight - (16 + 16 * (ui_rows() - 1)) : 0;
        float aw = rm->fbWidth, ah = rm->efbHeight - top - bottom;
        float sc = (aw / L->W < ah / L->H ? aw / L->W : ah / L->H) * zoom;
        float vw = L->W * sc, vh = L->H * sc;
        render(L, (rm->fbWidth - vw) / 2, top + (ah - vh) / 2, vw, vh);
        if (first) { render(L, (rm->fbWidth - vw) / 2, top + (ah - vh) / 2, vw, vh); first = false; }
        if (ntex_ok < 0) {
            ntex_ok = 0;
            for (int i = 0; i < L->ntex; i++) ntex_ok += L->tex[i].ok;
        }
        if (info) {
            ui_color(C_WHITE, true);
            ui_field(0, 0, ui_cols() - 1, " LAYOUT  %s  %.0fx%.0f  %d panes  %d/%d textures", vfs_basename(lpath), L->W, L->H,
                     L->npane, ntex_ok, L->ntex);
            if (a)
                ui_field(1, 0, ui_cols() - 1, " [%d/%d] %s  frame %d/%d%s%s", cur + 1, L->nanim, a->name, (int)frame, a->frames,
                         a->loop ? "  loop" : "", paused ? "  PAUSED" : "");
            else
                ui_field(1, 0, ui_cols() - 1, " no animation");
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 1, 0, ui_cols() - 1, " A Pause  <> Step  1 Next animation  +/- Zoom  2 Info  B Close");
            ui_reset();
        }
        UI_TEST_SHOT();

        u32 p = DetectInput(IN_DOWN);
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (p & WPAD_BUTTON_A) paused = !paused;
        if (p & WPAD_BUTTON_2) info = !info;
        if ((p & WPAD_BUTTON_PLUS) && zoom < 8) zoom *= 1.25f;
        if ((p & WPAD_BUTTON_MINUS) && zoom > 0.3f) zoom /= 1.25f;
        if ((p & WPAD_BUTTON_1) && L->nanim) { cur = (cur + 1) % L->nanim; frame = 0; }
        if (a && paused) {
            if (p & WPAD_BUTTON_RIGHT) frame = frame + 1 < a->frames ? frame + 1 : 0;
            if (p & WPAD_BUTTON_LEFT) frame = frame >= 1 ? frame - 1 : a->frames - 1;
        }
        if (a && !paused && !(p & WPAD_BUTTON_1)) {
            frame += 1;
            if (frame >= a->frames) {
                /* x_Start leads into x_Loop, as on the Wii Menu */
                size_t n = strlen(a->name);
                int next = -1;
                if (n > 6 && !strcasecmp(a->name + n - 6, "_Start")) {
                    char want[72];
                    snprintf(want, sizeof(want), "%.*s_Loop", (int)(n - 6), a->name);
                    for (int i = 0; i < L->nanim; i++)
                        if (!strcasecmp(L->anim[i].name, want)) next = i;
                }
                if (next >= 0) cur = next;
                frame = 0;                      /* the rest play again from the start */
            }
        }
    }
    GX_DrawDone();
    restore_gx();
    free_layout(L);
    free(L);
    ui_wipe();
    return true;
}

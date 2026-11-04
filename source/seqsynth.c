/*
 * NW4R sequence player: RSEQ sequences from a BRSAR, played with their RBNK instrument bank and
 * its waves.
 *
 * The BRSAR tables (sounds, banks, files, groups), the RSEQ command set, the RBNK region tables
 * and the envelope tables follow VGMTrans' RSAR reader (src/main/formats/RSARScanner.cpp,
 * RSARSeq.cpp, RSARInstrSet.cpp; Copyright (c) 2002-2015 Mike, VGMTrans Team; zlib/libpng
 * licence), rewritten in C here and extended into a player: this is an altered version, not
 * the original. The sequencer's variable and compare commands follow the same command set as
 * NITRO / NW4R sequence data (BrawlLib's RSEQ.cs lists the codes).
 *
 *   BRSAR   SYMB names; INFO tables of sounds (a sequence sound names its RSEQ file, the label it
 *           starts at and its bank), banks (their RBNK file), files (where each lives: group and
 *           index) and groups (data and wave data offsets of every file in them).
 *   RSEQ    DATA: offset of the command stream. Commands below 0x80 are notes (key, velocity,
 *           length); the rest are waits, program changes, jumps / calls / loops, track opening,
 *           volume / pan / pitch settings, tempo and variable operations.
 *   RBNK    DATA: instruments (8-byte references), each a key table and a velocity table
 *           (direct, a range table or an index table) leading to a region: wave index, ADSR,
 *           original key, volume, pan, tuning. WAVE: wave infos, when the waves aren't in an RWAR.
 *   RWAR    TABL of RWAV files: DSP-ADPCM, PCM16 or PCM8, optionally looping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "seqsynth.h"

#define MAX_TRACKS  16
#define MAX_VOICES  40
#define MAX_WAVES   4096
#define MAX_FILE    (24u << 20)
#define CALL_DEPTH  4
#define ENV_MIN     (-904.0f)           /* dB x 10: -90.4 dB is silence */
#define MS_FRAMES   (SEQ_RATE / 1000)   /* the envelope and the sequencer step every millisecond */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

typedef struct { const uint8_t *b; uint32_t n; } blk_t;
static uint32_t bu32(const blk_t *k, uint64_t off) { return off + 4 <= k->n ? be32(k->b + off) : 0xFFFFFFFFu; }
static uint8_t bu8(const blk_t *k, uint64_t off) { return off < k->n ? k->b[off] : 0; }

/* ---------------------------------------------------------------- envelopes (VGMTrans) */

static const float attack_table[128] = {
    0.9992175f, 0.9984326f, 0.9976452f, 0.9968553f, 0.9960629f, 0.9952679f, 0.9944704f, 0.9936704f,
    0.9928677f, 0.9920625f, 0.9912546f, 0.9904441f, 0.9896309f, 0.9888151f, 0.9879965f, 0.9871752f,
    0.9863512f, 0.9855244f, 0.9846949f, 0.9838625f, 0.9830273f, 0.9821893f, 0.9813483f, 0.9805045f,
    0.9796578f, 0.9788081f, 0.9779555f, 0.9770999f, 0.9762413f, 0.9753797f, 0.9745150f, 0.9736472f,
    0.9727763f, 0.9719023f, 0.9710251f, 0.9701448f, 0.9692612f, 0.9683744f, 0.9674844f, 0.9665910f,
    0.9656944f, 0.9647944f, 0.9638910f, 0.9629842f, 0.9620740f, 0.9611604f, 0.9602433f, 0.9593226f,
    0.9583984f, 0.9574706f, 0.9565392f, 0.9556042f, 0.9546655f, 0.9537231f, 0.9527769f, 0.9518270f,
    0.9508732f, 0.9499157f, 0.9489542f, 0.9479888f, 0.9470195f, 0.9460462f, 0.9450689f, 0.9440875f,
    0.9431020f, 0.9421124f, 0.9411186f, 0.9401206f, 0.9391184f, 0.9381118f, 0.9371009f, 0.9360856f,
    0.9350659f, 0.9340417f, 0.9330131f, 0.9319798f, 0.9309420f, 0.9298995f, 0.9288523f, 0.9278004f,
    0.9267436f, 0.9256821f, 0.9246156f, 0.9235442f, 0.9224678f, 0.9213864f, 0.9202998f, 0.9192081f,
    0.9181112f, 0.9170091f, 0.9159016f, 0.9147887f, 0.9136703f, 0.9125465f, 0.9114171f, 0.9102821f,
    0.9091414f, 0.9079949f, 0.9068427f, 0.9056845f, 0.9045204f, 0.9033502f, 0.9021740f, 0.9009916f,
    0.8998029f, 0.8986080f, 0.8974066f, 0.8961988f, 0.8949844f, 0.8900599f, 0.8824622f, 0.8759247f,
    0.8691861f, 0.8636406f, 0.8535788f, 0.8430189f, 0.8286135f, 0.8149099f, 0.8002172f, 0.7780663f,
    0.7554750f, 0.7242125f, 0.6828239f, 0.6329169f, 0.5592135f, 0.4551411f, 0.3298770f, 0.0000000f
};

static const int16_t sustain_table[128] = {
    -723, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360, -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239, -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169, -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119, -116, -114, -111, -109, -106, -103, -101,  -99,  -96,  -94,  -91,  -89,  -87,  -85,  -82,
     -80,  -78,  -76,  -74,  -72,  -70,  -68,  -66,  -64,  -62,  -60,  -58,  -56,  -54,  -52,  -50,
     -49,  -47,  -45,  -43,  -42,  -40,  -38,  -36,  -35,  -33,  -31,  -30,  -28,  -27,  -25,  -23,
     -22,  -20,  -19,  -17,  -16,  -14,  -13,  -11,  -10,   -8,   -7,   -6,   -4,   -3,   -1,    0
};

/* dB x 10 lost per millisecond while decaying / releasing */
static float falling_rate(uint8_t t)
{
    if (t >= 0x7F) return 65535.0f;
    if (t == 0x7E) return 120 / 5.0f;
    if (t < 0x32) return ((t << 1) + 1) / 128.0f / 5.0f;
    return (60.0f / (126 - t)) / 5.0f;
}

/* ---------------------------------------------------------------- state */

typedef struct {
    int16_t *pcm;                       /* decoded, channel 0; NULL until first used */
    uint32_t n, loop_start, rate;
    bool loop, bad;
} wave_t;

enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_OFF };

typedef struct {
    bool on;
    int track;
    const wave_t *w;
    double pos, step;                   /* in source samples */
    float base_ratio;                   /* wave rate / output rate * tuning */
    int key, orig_key;
    float env, att, dec, sus, rel;      /* envelope in dB x 10 and its parameters */
    int phase;
    int32_t len;                        /* ticks left; < 0: until released */
    float gain;                         /* velocity and region volume */
    int rpan;                           /* region pan offset */
    uint32_t age;
} voice_t;

typedef struct {
    bool on;
    uint32_t pc;
    int32_t wait;
    struct { uint32_t ret; int count; bool loop; } stack[CALL_DEPTH];
    int sp;
    int prg, vol, vol2, pan, transpose, bend, range;
    bool note_wait, tie, mono, cmp, mute;
    int att, dec, sus, rel;             /* -1: the region's */
    int16_t var[16];
    int tie_voice;
} track_t;

struct seq {
    /* files */
    uint8_t *rseq, *rbnk, *wdat;        /* RSEQ, RBNK, wave data (RWAR or raw) */
    uint32_t rseq_n, rbnk_n, wdat_n;
    uint32_t code, code_n;              /* RSEQ command stream: offset, size */
    uint32_t inst, inst_n;              /* RBNK DATA contents */
    uint32_t wtab, wtab_n;              /* RBNK WAVE contents, 0 when the waves are an RWAR */
    wave_t *waves;
    uint32_t nwaves;
    char bank[64];
    /* playback */
    track_t t[MAX_TRACKS];
    voice_t v[MAX_VOICES];
    uint32_t muted, seen;               /* tracks the listener turned off; tracks that have run */
    float level[MAX_TRACKS];            /* loudest note per track, for the player's meters */
    int16_t lvar[16];
    int tempo, timebase, main_vol;
    float sound_vol;
    double ticks;                       /* fractional ticks owed */
    uint32_t sub;                       /* frames into the current millisecond */
    uint64_t frames;
    uint32_t age, rng;
    bool repeat, ended;
    int loops;                          /* backward jumps of the loop track */
    int loop_track;
    float fade, fade_step;
};

static int16_t g_var[16];

/* ---------------------------------------------------------------- loading */

static uint8_t *grab(seq_read_fn rd, void *ctx, uint64_t size, uint64_t off, uint32_t n)
{
    if (!n || n > MAX_FILE || off + n > size) return NULL;
    uint8_t *b = malloc(n);
    if (b && !rd(ctx, off, b, n)) { free(b); b = NULL; }
    return b;
}

typedef struct {
    uint8_t *sb, *ib;
    blk_t sy, in;
    uint32_t stab, scnt, btab, bcnt, ftab, fcnt, gtab, gcnt;
} rsar_t;

static void rsar_free(rsar_t *r) { free(r->sb); free(r->ib); }

static bool rsar_load(rsar_t *r, seq_read_fn rd, void *ctx, uint64_t size)
{
    memset(r, 0, sizeof(*r));
    uint8_t h[0x28];
    if (size < 0x28 || !rd(ctx, 0, h, 0x28) || memcmp(h, "RSAR", 4)) return false;
    uint32_t so = be32(h + 0x10), ss = be32(h + 0x14), io = be32(h + 0x18), is = be32(h + 0x1C);
    if (ss < 8 || is < 8 || ss > (16u << 20) || is > (16u << 20)) return false;
    r->sb = grab(rd, ctx, size, so, ss);
    r->ib = grab(rd, ctx, size, io, is);
    if (!r->sb || !r->ib || memcmp(r->sb, "SYMB", 4) || memcmp(r->ib, "INFO", 4)) return false;
    r->sy = (blk_t){ r->sb + 8, ss - 8 };
    r->in = (blk_t){ r->ib + 8, is - 8 };
    r->stab = bu32(&r->in, 0x04); r->scnt = bu32(&r->in, r->stab);
    r->btab = bu32(&r->in, 0x0C); r->bcnt = bu32(&r->in, r->btab);
    r->ftab = bu32(&r->in, 0x1C); r->fcnt = bu32(&r->in, r->ftab);
    r->gtab = bu32(&r->in, 0x24); r->gcnt = bu32(&r->in, r->gtab);
    uint32_t lim = r->in.n / 8;
    if (r->scnt > lim) r->scnt = 0;
    if (r->bcnt > lim) r->bcnt = 0;
    if (r->fcnt > lim) r->fcnt = 0;
    if (r->gcnt > lim) r->gcnt = 0;
    return true;
}

static const char *rsar_name(const rsar_t *r, uint32_t id)
{
    uint32_t tab = bu32(&r->sy, 0), cnt = bu32(&r->sy, tab);
    if (id == 0xFFFFFFFFu || tab == 0xFFFFFFFFu || id >= cnt) return NULL;
    uint32_t o = bu32(&r->sy, tab + 4 + (uint64_t)id * 4);
    if (o >= r->sy.n || !memchr(r->sy.b + o, 0, r->sy.n - o)) return NULL;
    return (const char *)r->sy.b + o;
}

/* Where file `fid` is: its data and its wave data, as absolute offsets and sizes. */
static bool rsar_file(const rsar_t *r, uint32_t fid, uint64_t *d, uint32_t *dn, uint64_t *w, uint32_t *wn)
{
    if (fid >= r->fcnt) return false;
    const blk_t *in = &r->in;
    uint32_t fe = bu32(in, r->ftab + 8 + fid * 8ull);
    uint32_t pt = bu32(in, fe + 0x18ull), pc = bu32(in, pt);
    if (!pc || pc == 0xFFFFFFFFu) return false;
    uint32_t pe = bu32(in, pt + 8ull);
    uint32_t gid = bu32(in, pe), idx = bu32(in, pe + 4ull);
    if (gid >= r->gcnt) return false;
    uint32_t ge = bu32(in, r->gtab + 8 + gid * 8ull);
    uint32_t gd = bu32(in, ge + 0x10ull), gw = bu32(in, ge + 0x18ull);
    uint32_t it = bu32(in, ge + 0x24ull), ic = bu32(in, it);
    if (idx >= ic || ic > in->n / 8) return false;
    uint32_t ie = bu32(in, it + 8 + idx * 8ull);
    *d = (uint64_t)gd + bu32(in, ie + 4ull);
    *dn = bu32(in, ie + 8ull);
    *w = (uint64_t)gw + bu32(in, ie + 0xCull);
    *wn = bu32(in, ie + 0x10ull);
    return true;
}

/* The name as the BRSAR browser shows it: '/', '|', '\' and control characters as '_'. */
static bool same_name(const char *a, const char *clean)
{
    for (; *a && *clean; a++, clean++) {
        char c = *a;
        if (c == '/' || c == '|' || c == '\\' || (unsigned char)c < 0x20 || (unsigned char)c == 0x7F) c = '_';
        if (c != *clean) return false;
    }
    return !*a && !*clean;
}

/* ---------------------------------------------------------------- waves */

static uint32_t nib2smp(uint32_t n) { return n / 16 * 14 + (n % 16 > 2 ? n % 16 - 2 : 0); }

/* Decodes channel 0 of a wave described by WaveInfo `wi` (n bytes available from it) whose
 * samples start at `data`. */
static void decode_wave(wave_t *w, const uint8_t *wi, uint32_t wn, const uint8_t *base, uint32_t base_n, uint32_t data)
{
    w->bad = true;
    if (wn < 0x18) return;
    int fmt = wi[0];
    w->loop = wi[1] != 0;
    w->rate = (uint32_t)wi[3] << 16 | be16(wi + 4);
    uint32_t ls = be32(wi + 8), le = be32(wi + 12), ct = be32(wi + 16);
    if (!wi[2] || !w->rate || ct + 4 > wn) return;
    uint32_t ci = be32(wi + ct);
    if (ci + 8 > wn) return;
    uint32_t start = data + be32(wi + ci);
    uint32_t n = fmt == 2 ? nib2smp(le) : le;
    w->loop_start = fmt == 2 ? nib2smp(ls) : ls;
    if (fmt == 2) n++;                  /* the end address is the last sample's */
    if (!n || n > (8u << 20) || start > base_n) return;
    int16_t *o = malloc((n + 2) * sizeof(int16_t));
    if (!o) return;
    const uint8_t *p = base + start;
    uint32_t avail = base_n - start, i = 0;
    if (fmt == 2) {
        uint32_t ai = be32(wi + ci + 4);
        if (!ai || ai + 0x28 > wn) { free(o); return; }
        int16_t coef[16];
        for (int k = 0; k < 16; k++) coef[k] = (int16_t)be16(wi + ai + k * 2);
        int32_t h1 = (int16_t)be16(wi + ai + 0x24), h2 = (int16_t)be16(wi + ai + 0x26);
        for (uint32_t f = 0; i < n && f * 8 + 8 <= avail; f++) {
            const uint8_t *fr = p + f * 8;
            int scale = 1 << (fr[0] & 15), ix = (fr[0] >> 4) & 7;
            int32_t c1 = coef[ix * 2], c2 = coef[ix * 2 + 1];
            for (int k = 0; k < 14 && i < n; k++) {
                int nib = k & 1 ? fr[1 + k / 2] & 15 : fr[1 + k / 2] >> 4;
                if (nib >= 8) nib -= 16;
                int32_t s = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2;
                s >>= 11;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                o[i++] = (int16_t)s;
                h2 = h1;
                h1 = s;
            }
        }
    } else if (fmt == 1) {
        for (; i < n && i * 2 + 2 <= avail; i++) o[i] = (int16_t)be16(p + i * 2);
    } else {
        for (; i < n && i < avail; i++) o[i] = (int16_t)((int8_t)p[i] * 256);
    }
    if (!i) { free(o); return; }
    o[i] = o[i + 1] = 0;
    w->pcm = o;
    w->n = i;
    if (w->loop_start >= i) w->loop = false;
    w->bad = false;
}

static const wave_t *get_wave(seq_t *s, uint32_t idx)
{
    if (idx >= s->nwaves) return NULL;
    wave_t *w = &s->waves[idx];
    if (w->pcm) return w;
    if (w->bad) return NULL;
    w->bad = true;
    if (s->wtab) {                      /* infos in the bank, samples in the group's wave data */
        blk_t k = { s->rbnk + s->wtab, s->wtab_n };
        uint32_t o = bu32(&k, 8 + idx * 8ull);
        if (o == 0xFFFFFFFFu || o + 0x18 > k.n || !s->wdat) return NULL;
        const uint8_t *wi = k.b + o;
        decode_wave(w, wi, k.n - o, s->wdat, s->wdat_n, be32(wi + 0x14));
    } else {                            /* an RWAR of RWAVs */
        if (!s->wdat || s->wdat_n < 0x20 || memcmp(s->wdat, "RWAR", 4)) return NULL;
        blk_t k = { s->wdat, s->wdat_n };
        uint32_t tabl = bu32(&k, 0x10), dat = bu32(&k, 0x18);
        uint32_t off = bu32(&k, tabl + 12 + idx * 12ull + 4);
        uint64_t rw = (uint64_t)dat + off;
        if (off == 0xFFFFFFFFu || rw + 0x20 > k.n || memcmp(k.b + rw, "RWAV", 4)) return NULL;
        const uint8_t *r = k.b + rw;
        uint32_t rn = k.n - (uint32_t)rw;
        uint32_t info = be32(r + 0x10), data = be32(r + 0x18);
        if (info + 8 > rn || data + 8 > rn || memcmp(r + info, "INFO", 4)) return NULL;
        decode_wave(w, r + info + 8, rn - info - 8, r, rn, data + 8);
    }
    return w->pcm ? w : NULL;
}

/* ---------------------------------------------------------------- instruments */

/* One level of an RBNK region table: the reference to follow for value v. */
static int32_t sub_ref(const seq_t *s, uint32_t ref, int v)
{
    blk_t k = { s->rbnk + s->inst, s->inst_n };
    if (ref + 8 > k.n) return -1;
    int type = k.b[ref + 1];
    uint32_t off = bu32(&k, ref + 4ull);
    if (type == 1) return (int32_t)ref;                     /* direct */
    if (off >= k.n) return -1;
    if (type == 2) {                                        /* range: highest key / velocity of each */
        int n = k.b[off];
        uint32_t refs = (off + 1 + n + 3) & ~3u;
        for (int i = 0; i < n && off + 1 + i < k.n; i++)
            if (v <= k.b[off + 1 + i]) return refs + i * 8ull + 8 <= k.n ? (int32_t)(refs + i * 8) : -1;
        return -1;
    }
    if (type == 3) {                                        /* index: min, max */
        int lo = k.b[off], hi = bu8(&k, off + 1ull);
        if (v < lo || v > hi) return -1;
        uint32_t r = off + 4 + (uint32_t)(v - lo) * 8;
        return r + 8 <= k.n ? (int32_t)r : -1;
    }
    return -1;
}

/* The region for program / key / velocity: its offset in the instrument data, or -1. */
static int32_t find_region(const seq_t *s, int prg, int key, int vel)
{
    blk_t k = { s->rbnk + s->inst, s->inst_n };
    uint32_t cnt = bu32(&k, 0);
    if (prg < 0 || (uint32_t)prg >= cnt || 4 + prg * 8ull + 8 > k.n) return -1;
    int32_t r = sub_ref(s, 4 + prg * 8, key);
    if (r >= 0) r = sub_ref(s, (uint32_t)r, vel);
    if (r < 0) return -1;
    uint32_t rg = bu32(&k, r + 4ull);
    return rg + 0x14 <= k.n ? (int32_t)rg : -1;
}

/* ---------------------------------------------------------------- open */

static void free_files(seq_t *s)
{
    if (s->waves)
        for (uint32_t i = 0; i < s->nwaves; i++) free(s->waves[i].pcm);
    free(s->waves);
    free(s->rseq);
    free(s->rbnk);
    free(s->wdat);
}

void seq_close(seq_t *s)
{
    if (!s) return;
    free_files(s);
    free(s);
}

static void track_reset(track_t *t, uint32_t pc)
{
    memset(t, 0, sizeof(*t));
    t->on = true;
    t->pc = pc;
    t->vol = 127;
    t->vol2 = 127;
    t->pan = 64;
    t->range = 2;
    t->note_wait = true;
    t->att = t->dec = t->sus = t->rel = -1;
    t->tie_voice = -1;
}

seq_t *seq_open(seq_read_fn rd, void *ctx, uint64_t size, const char *name, const char **err)
{
    rsar_t r;
    seq_t *s = NULL;
    *err = "This sound archive couldn't be read";
    if (!rsar_load(&r, rd, ctx, size)) goto fail;
    const blk_t *in = &r.in;
    uint32_t se = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < r.scnt; i++) {
        uint32_t e = bu32(in, r.stab + 8 + i * 8ull);
        if (bu8(in, e + 0x16ull) != 1) continue;            /* sequences only */
        const char *n = rsar_name(&r, bu32(in, e));
        char def[24];
        if (!n) { snprintf(def, sizeof(def), "sound%04u", (unsigned)i); n = def; }
        if (same_name(n, name)) { se = e; break; }
    }
    *err = "This sequence isn't in the sound archive";
    if (se == 0xFFFFFFFFu) goto fail;
    uint32_t si = bu32(in, se + 0x1Cull);
    uint32_t label = bu32(in, si), bank = bu32(in, si + 4ull);
    s = calloc(1, sizeof(*s));
    if (!s) { *err = "Not enough memory"; goto fail; }
    s->sound_vol = bu8(in, se + 0x14ull) / 127.0f;
    if (s->sound_vol <= 0) s->sound_vol = 1;

    /* the RSEQ */
    uint64_t d, w;
    uint32_t dn, wn;
    *err = "The sequence's file couldn't be found in the archive";
    if (!rsar_file(&r, bu32(in, se + 4ull), &d, &dn, &w, &wn)) goto fail;
    s->rseq = grab(rd, ctx, size, d, dn);
    s->rseq_n = dn;
    *err = "The sequence file is damaged";
    if (!s->rseq || dn < 0x20 || memcmp(s->rseq, "RSEQ", 4)) goto fail;
    uint32_t db = be32(s->rseq + 0x10);
    if (db + 12 > dn || memcmp(s->rseq + db, "DATA", 4)) goto fail;
    s->code = db + be32(s->rseq + db + 8);
    if (s->code >= dn || label >= dn - s->code) goto fail;
    s->code_n = dn - s->code;

    /* the bank and its waves */
    *err = "The sequence's instrument bank couldn't be found";
    if (bank >= r.bcnt) goto fail;
    uint32_t be = bu32(in, r.btab + 8 + bank * 8ull);
    const char *bn = rsar_name(&r, bu32(in, be));
    snprintf(s->bank, sizeof(s->bank), "%s", bn ? bn : "");
    if (!rsar_file(&r, bu32(in, be + 4ull), &d, &dn, &w, &wn)) goto fail;
    s->rbnk = grab(rd, ctx, size, d, dn);
    s->rbnk_n = dn;
    if (!s->rbnk || dn < 0x20 || memcmp(s->rbnk, "RBNK", 4)) goto fail;
    uint32_t ib = be32(s->rbnk + 0x10), wb = be32(s->rbnk + 0x18);
    if (ib + 8 > dn || memcmp(s->rbnk + ib, "DATA", 4)) goto fail;
    s->inst = ib + 8;
    s->inst_n = be32(s->rbnk + ib + 4) - 8;
    if (s->inst_n > dn - s->inst) s->inst_n = dn - s->inst;
    if (wb && wb + 12 <= dn && !memcmp(s->rbnk + wb, "WAVE", 4) && be32(s->rbnk + wb + 4) > 12) {
        s->wtab = wb + 8;
        s->wtab_n = be32(s->rbnk + wb + 4) - 8;
        if (s->wtab_n > dn - s->wtab) s->wtab_n = dn - s->wtab;
        s->nwaves = be32(s->rbnk + s->wtab);
    }
    *err = "The instrument bank's waves couldn't be read";
    if (wn && wn != 0xFFFFFFFFu) {
        s->wdat = grab(rd, ctx, size, w, wn);
        s->wdat_n = wn;
    }
    if (!s->wdat) goto fail;
    if (!s->wtab) {
        if (wn < 0x20 || memcmp(s->wdat, "RWAR", 4)) goto fail;
        uint32_t tabl = be32(s->wdat + 0x10);
        if (tabl + 12 > wn) goto fail;
        s->nwaves = be32(s->wdat + tabl + 8);
    }
    if (s->nwaves > MAX_WAVES) s->nwaves = MAX_WAVES;
    s->waves = calloc(s->nwaves ? s->nwaves : 1, sizeof(wave_t));
    *err = "Not enough memory";
    if (!s->waves) goto fail;

    rsar_free(&r);
    s->tempo = 120;
    s->timebase = 48;
    s->main_vol = 127;
    s->rng = 0x12345678u;
    s->fade = 1;
    s->loop_track = -1;
    track_reset(&s->t[0], label);
    *err = NULL;
    return s;
fail:
    rsar_free(&r);
    seq_close(s);
    return NULL;
}

/* ---------------------------------------------------------------- voices */

static float db_to_gain(float db10) { return db10 <= ENV_MIN ? 0 : powf(10.0f, db10 / 200.0f); }

static void release_voice(voice_t *v)
{
    if (v->on && v->phase < ENV_RELEASE) v->phase = ENV_RELEASE;
}

static void set_pitch(seq_t *s, voice_t *v)
{
    const track_t *t = &s->t[v->track];
    float semi = (float)(v->key - v->orig_key) + t->bend * t->range / 128.0f;
    v->step = v->base_ratio * powf(2.0f, semi / 12.0f);
}

static void note_on(seq_t *s, int ti, int key, int vel, int32_t len)
{
    track_t *t = &s->t[ti];
    if (t->mute) return;
    key += t->transpose;
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    if (t->tie && t->tie_voice >= 0 && s->v[t->tie_voice].on && s->v[t->tie_voice].track == ti &&
        s->v[t->tie_voice].phase < ENV_RELEASE) {
        voice_t *v = &s->v[t->tie_voice];       /* tied: the sounding note moves to the new key */
        v->key = key;
        v->len = -1;
        set_pitch(s, v);
        return;
    }
    if (t->mono)
        for (int i = 0; i < MAX_VOICES; i++)
            if (s->v[i].on && s->v[i].track == ti) release_voice(&s->v[i]);
    int32_t rg = find_region(s, t->prg, key, vel);
    if (rg < 0) return;
    const uint8_t *r = s->rbnk + s->inst + rg;
    const wave_t *w = get_wave(s, be32(r));
    if (!w) return;
    /* a free voice, else the quietest releasing one, else the oldest */
    int best = -1;
    float best_env = 1e9f;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < MAX_VOICES; i++) {
        voice_t *v = &s->v[i];
        if (!v->on) { best = i; break; }
        if (v->phase == ENV_RELEASE && v->env < best_env) { best_env = v->env; best = i; oldest = 0; }
        else if (best_env > 1e8f && v->age < oldest) { oldest = v->age; best = i; }
    }
    voice_t *v = &s->v[best];
    memset(v, 0, sizeof(*v));
    v->on = true;
    v->track = ti;
    v->w = w;
    v->key = key;
    v->orig_key = r[0x0C];
    union { uint32_t u; float f; } tune = { be32(r + 0x10) };
    float tf = tune.f > 0.01f && tune.f < 100.0f ? tune.f : 1.0f;
    v->base_ratio = (float)w->rate / SEQ_RATE * tf;
    int a = t->att >= 0 ? t->att : r[4], d = t->dec >= 0 ? t->dec : r[5];
    int su = t->sus >= 0 ? t->sus : r[6], re = t->rel >= 0 ? t->rel : r[7];
    v->att = attack_table[a & 127];
    v->dec = falling_rate((uint8_t)d);
    v->sus = sustain_table[su & 127];
    v->rel = falling_rate((uint8_t)re);
    v->env = v->att > 0 ? ENV_MIN : 0;
    v->phase = v->att > 0 ? ENV_ATTACK : ENV_DECAY;
    float fv = vel / 127.0f, rv = r[0x0D] / 127.0f;
    v->gain = fv * fv * rv * rv;
    v->rpan = (int)r[0x0E] - 64;
    v->len = t->tie ? -1 : len;
    v->age = ++s->age;
    set_pitch(s, v);
    t->tie_voice = best;
}

static void env_step(voice_t *v)
{
    switch (v->phase) {
    case ENV_ATTACK:
        v->env *= v->att;
        if (v->env > -1.0f / 32) { v->env = 0; v->phase = ENV_DECAY; }
        break;
    case ENV_DECAY:
        v->env -= v->dec;
        if (v->env <= v->sus) { v->env = v->sus; v->phase = ENV_SUSTAIN; }
        break;
    case ENV_RELEASE:
        v->env -= v->rel;
        if (v->env <= ENV_MIN) v->on = false;
        break;
    }
}

/* ---------------------------------------------------------------- sequencer */

static uint32_t rnd(seq_t *s) { s->rng = s->rng * 1103515245u + 12345u; return s->rng >> 16; }

static int16_t *var_ref(seq_t *s, track_t *t, int i)
{
    if (i < 16) return &s->lvar[i];
    if (i < 32) return &g_var[i - 16];
    if (i < 48) return &t->var[i - 32];
    return NULL;
}

enum { A_U8, A_S8, A_S16, A_VAR };

/* Reads one byte of the command stream; past the end the track stops (0xFF). */
static int cb(seq_t *s, track_t *t)
{
    if (t->pc >= s->code_n) { t->on = false; return 0xFF; }
    return s->rseq[s->code + t->pc++];
}

static uint32_t varlen(seq_t *s, track_t *t)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int c = cb(s, t);
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

static int32_t s16(seq_t *s, track_t *t) { int h = cb(s, t); return (int16_t)(h << 8 | cb(s, t)); }
static uint32_t u24(seq_t *s, track_t *t) { uint32_t a = cb(s, t); a = a << 8 | cb(s, t); return a << 8 | cb(s, t); }

/* An argument of the default kind, or the kind a random / variable prefix asked for. */
static int32_t arg(seq_t *s, track_t *t, int kind, int prefix)
{
    if (prefix == 0xA0) {
        int32_t lo = s16(s, t), hi = s16(s, t);
        return hi > lo ? lo + (int32_t)(rnd(s) % (uint32_t)(hi - lo + 1)) : lo;
    }
    if (prefix == 0xA1) {
        int16_t *v = var_ref(s, t, cb(s, t));
        return v ? *v : 0;
    }
    switch (kind) {
    case A_U8:  return cb(s, t);
    case A_S8:  return (int8_t)cb(s, t);
    case A_S16: return s16(s, t);
    default:    return (int32_t)varlen(s, t);
    }
}

static void jump(seq_t *s, int ti, uint32_t to)
{
    track_t *t = &s->t[ti];
    if (to >= s->code_n) { t->on = false; return; }
    if (to < t->pc) {                   /* backwards: a loop. The first track that loops counts. */
        if (s->loop_track < 0) s->loop_track = ti;
        if (s->loop_track == ti && ++s->loops >= 2 && !s->repeat && s->fade_step == 0)
            s->fade_step = 1.0f / (4 * SEQ_RATE);
    }
    t->pc = to;
}

/* Runs track ti until it waits or stops. */
static void run_track(seq_t *s, int ti)
{
    track_t *t = &s->t[ti];
    for (int guard = 0; t->on && t->wait <= 0 && guard < 10000; guard++) {
        int c = cb(s, t), prefix = 0;
        bool doit = true;
        while (c == 0xA0 || c == 0xA1 || c == 0xA2 || c == 0xA3 || c == 0xA4 || c == 0xA5) {
            if (c == 0xA2) doit = t->cmp;
            else if (c <= 0xA1) prefix = c;
            /* 0xA3 - 0xA5 (time, ramp versions of a command) are read as the plain command */
            c = cb(s, t);
        }
        if (!t->on) break;
        if (c < 0x80) {
            int vel = cb(s, t);
            int32_t len = arg(s, t, A_VAR, prefix);
            if (doit) {
                note_on(s, ti, c, vel, len);
                if (t->note_wait) t->wait = len;
            }
            continue;
        }
        switch (c) {
        case 0x80: { int32_t w = arg(s, t, A_VAR, prefix); if (doit) t->wait = w; break; }
        case 0x81: { int32_t p = arg(s, t, A_VAR, prefix); if (doit) t->prg = p & 0xFFFF; break; }
        case 0x88: {
            int n = cb(s, t);
            uint32_t to = u24(s, t);
            if (doit && n > 0 && n < MAX_TRACKS && to < s->code_n) {
                track_reset(&s->t[n], to);
                memcpy(s->t[n].var, t->var, sizeof(t->var));
            }
            break;
        }
        case 0x89: { uint32_t to = u24(s, t); if (doit) jump(s, ti, to); break; }
        case 0x8A: {
            uint32_t to = u24(s, t);
            if (doit && t->sp < CALL_DEPTH) {
                t->stack[t->sp].ret = t->pc;
                t->stack[t->sp].loop = false;
                t->sp++;
                t->pc = to < s->code_n ? to : t->pc;
            }
            break;
        }
        case 0xB0: { int v = arg(s, t, A_U8, prefix); if (doit && v) s->timebase = v; break; }
        case 0xB2: { int v = arg(s, t, A_U8, prefix); if (doit) t->mono = v != 0; break; }
        case 0xC0: { int v = arg(s, t, A_U8, prefix); if (doit) t->pan = v; break; }
        case 0xC1: { int v = arg(s, t, A_U8, prefix); if (doit) t->vol = v; break; }
        case 0xC2: { int v = arg(s, t, A_U8, prefix); if (doit) s->main_vol = v; break; }
        case 0xC3: { int v = arg(s, t, A_S8, prefix); if (doit) t->transpose = v; break; }
        case 0xC4: {
            int v = arg(s, t, A_S8, prefix);
            if (doit) {
                t->bend = v;
                for (int i = 0; i < MAX_VOICES; i++)
                    if (s->v[i].on && s->v[i].track == ti) set_pitch(s, &s->v[i]);
            }
            break;
        }
        case 0xC5: { int v = arg(s, t, A_U8, prefix); if (doit) t->range = v; break; }
        case 0xC7: { int v = arg(s, t, A_U8, prefix); if (doit) t->note_wait = v != 0; break; }
        case 0xC8: {
            int v = arg(s, t, A_U8, prefix);
            if (doit) {
                t->tie = v != 0;
                if (!t->tie && t->tie_voice >= 0 && s->v[t->tie_voice].track == ti) release_voice(&s->v[t->tie_voice]);
            }
            break;
        }
        case 0xD0: { int v = arg(s, t, A_U8, prefix); if (doit) t->att = v; break; }
        case 0xD1: { int v = arg(s, t, A_U8, prefix); if (doit) t->dec = v; break; }
        case 0xD2: { int v = arg(s, t, A_U8, prefix); if (doit) t->sus = v; break; }
        case 0xD3: { int v = arg(s, t, A_U8, prefix); if (doit) t->rel = v; break; }
        case 0xD4: {
            int n = arg(s, t, A_U8, prefix);
            if (doit && t->sp < CALL_DEPTH) {
                t->stack[t->sp].ret = t->pc;
                t->stack[t->sp].count = n;
                t->stack[t->sp].loop = true;
                t->sp++;
            }
            break;
        }
        case 0xD5: { int v = arg(s, t, A_U8, prefix); if (doit) t->vol2 = v; break; }
        case 0xDD: { int v = arg(s, t, A_U8, prefix); if (doit) t->mute = v != 0; break; }
        case 0xE0: case 0xE3: arg(s, t, A_S16, prefix); break;
        case 0xE1: { int v = arg(s, t, A_S16, prefix); if (doit && v > 0) s->tempo = v; break; }
        case 0xF0: {
            int x = cb(s, t);
            if (x >= 0x80 && x <= 0x95) {
                int vi = cb(s, t);
                int32_t val = arg(s, t, A_S16, prefix);
                int16_t *v = var_ref(s, t, vi);
                if (!doit || !v) break;
                switch (x) {
                case 0x80: *v = (int16_t)val; break;
                case 0x81: *v = (int16_t)(*v + val); break;
                case 0x82: *v = (int16_t)(*v - val); break;
                case 0x83: *v = (int16_t)(*v * val); break;
                case 0x84: if (val) *v = (int16_t)(*v / val); break;
                case 0x85: *v = (int16_t)(val >= 0 ? *v << val : *v >> -val); break;
                case 0x86: *v = (int16_t)(val > 0 ? (int32_t)(rnd(s) % (uint32_t)(val + 1)) : 0); break;
                case 0x87: *v = (int16_t)(*v & val); break;
                case 0x88: *v = (int16_t)(*v | val); break;
                case 0x89: *v = (int16_t)(*v ^ val); break;
                case 0x8A: *v = (int16_t)~val; break;
                case 0x8B: if (val) *v = (int16_t)(*v % val); break;
                case 0x90: t->cmp = *v == val; break;
                case 0x91: t->cmp = *v >= val; break;
                case 0x92: t->cmp = *v > val; break;
                case 0x93: t->cmp = *v <= val; break;
                case 0x94: t->cmp = *v < val; break;
                case 0x95: t->cmp = *v != val; break;
                }
            } else if ((x & 0xF0) == 0xE0) {
                cb(s, t); cb(s, t);
            } else
                t->on = false;          /* unknown: stop rather than misread */
            break;
        }
        case 0xFC:                      /* loop end */
            if (doit && t->sp && t->stack[t->sp - 1].loop) {
                if (t->stack[t->sp - 1].count == 0 || --t->stack[t->sp - 1].count > 0) {
                    if (t->stack[t->sp - 1].count == 0) jump(s, ti, t->stack[t->sp - 1].ret);
                    else t->pc = t->stack[t->sp - 1].ret;
                } else
                    t->sp--;
            }
            break;
        case 0xFD:
            if (doit && t->sp) { t->sp--; t->pc = t->stack[t->sp].ret; }
            break;
        case 0xFE: cb(s, t); cb(s, t); break;
        case 0xFF:
            if (doit) {
                t->on = false;
                for (int i = 0; i < MAX_VOICES; i++)
                    if (s->v[i].on && s->v[i].track == ti && s->v[i].len < 0) release_voice(&s->v[i]);
            }
            break;
        default:
            if (c >= 0xB0 && c <= 0xDF) arg(s, t, A_U8, prefix);    /* settings without an effect here */
            else t->on = false;
            break;
        }
    }
}

static void tick(seq_t *s)
{
    for (int i = 0; i < MAX_TRACKS; i++) {
        track_t *t = &s->t[i];
        if (!t->on) continue;
        if (t->wait > 0) t->wait--;
        if (t->wait <= 0) run_track(s, i);
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        voice_t *v = &s->v[i];
        if (v->on && v->len > 0 && --v->len == 0) release_voice(v);
    }
}

/* ---------------------------------------------------------------- rendering */

void seq_set_repeat(seq_t *s, bool repeat)
{
    s->repeat = repeat;
    if (repeat) { s->fade_step = 0; s->fade = 1; }
}

uint32_t seq_render(seq_t *s, int16_t *out, uint32_t frames)
{
    uint32_t done = 0;
    while (done < frames && !s->ended) {
        if (s->sub == 0) {
            /* a new millisecond: the sequencer, then the envelopes */
            s->ticks += s->tempo * s->timebase / 60000.0;
            while (s->ticks >= 1) { tick(s); s->ticks -= 1; }
            bool any = false;
            for (int i = 0; i < MAX_TRACKS; i++) any |= s->t[i].on;
            for (int i = 0; i < MAX_VOICES; i++) {
                if (s->v[i].on) env_step(&s->v[i]);
                any |= s->v[i].on;
            }
            if (!any || s->fade <= 0) { s->ended = true; break; }
        }
        uint32_t n = MS_FRAMES - s->sub;
        if (n > frames - done) n = frames - done;
        float mix[MS_FRAMES * 2];
        memset(mix, 0, n * 2 * sizeof(float));
        float master = s->sound_vol * (s->main_vol / 127.0f) * (s->main_vol / 127.0f) * 0.5f;
        for (int i = 0; i < MAX_TRACKS; i++) if (s->t[i].on) s->seen |= 1u << i;
        for (int i = 0; i < MAX_VOICES; i++) {
            voice_t *v = &s->v[i];
            if (!v->on) continue;
            const track_t *t = &s->t[v->track];
            float tv = t->vol / 127.0f, ev = t->vol2 / 127.0f;
            float g = master * v->gain * tv * tv * ev * ev * db_to_gain(v->env);
            if (g > s->level[v->track]) s->level[v->track] = g;
            if (s->muted >> v->track & 1) g = 0;    /* turned off: keeps its place, so it comes back in time */
            int p = t->pan + v->rpan;
            if (p < 0) p = 0;
            if (p > 127) p = 127;
            float ang = p / 127.0f * 1.5707963f;
            float gl = g * cosf(ang) * 1.41421356f, gr = g * sinf(ang) * 1.41421356f;
            const wave_t *w = v->w;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ip = (uint32_t)v->pos;
                if (ip >= w->n) {
                    if (w->loop && w->n > w->loop_start) {
                        v->pos -= w->n - w->loop_start;
                        ip = (uint32_t)v->pos;
                    } else { v->on = false; break; }
                }
                float fr = (float)(v->pos - ip);
                int32_t a = w->pcm[ip];
                int32_t b = ip + 1 < w->n ? w->pcm[ip + 1] : (w->loop ? w->pcm[w->loop_start] : 0);
                float x = a + (b - a) * fr;
                mix[k * 2] += x * gl;
                mix[k * 2 + 1] += x * gr;
                v->pos += v->step;
            }
        }
        for (uint32_t k = 0; k < n; k++) {
            if (s->fade_step > 0) { s->fade -= s->fade_step; if (s->fade < 0) s->fade = 0; }
            for (int c = 0; c < 2; c++) {
                float x = mix[k * 2 + c] * s->fade;
                x += x < 0 ? -0.5f : 0.5f;
                out[(done + k) * 2 + c] = x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x;
            }
        }
        done += n;
        s->sub = (s->sub + n) % MS_FRAMES;
    }
    s->frames += done;
    return done;
}

const char *seq_bank_name(const seq_t *s) { return s->bank; }
uint64_t seq_frames(const seq_t *s) { return s->frames; }
int seq_tempo(const seq_t *s) { return s->tempo; }

int seq_tracks(const seq_t *s)
{
    int n = 0;
    for (int i = 0; i < MAX_TRACKS; i++) n += s->t[i].on;
    return n;
}

int seq_voices(const seq_t *s)
{
    int n = 0;
    for (int i = 0; i < MAX_VOICES; i++) n += s->v[i].on;
    return n;
}

void seq_set_muted(seq_t *s, uint32_t mask) { s->muted = mask; }

int seq_track(seq_t *s, int i, int *prg, int *notes, float *level)
{
    if (i < 0 || i >= MAX_TRACKS) return 0;
    int n = 0;
    for (int k = 0; k < MAX_VOICES; k++) n += s->v[k].on && s->v[k].track == i;
    if (prg) *prg = s->t[i].prg;
    if (notes) *notes = n;
    if (level) { *level = s->level[i]; s->level[i] *= 0.5f; }
    return s->t[i].on ? 1 : (s->seen >> i & 1) || n ? 2 : 0;
}

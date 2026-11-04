/*
 * Audio player for Nintendo stream / wave formats. See media.h.
 *
 * Formats and layouts follow quatric's mobipeg (libavformat brstm.c, rwav.c,
 * bns.c, dsp.c, astdec.c, btsnd.c) and nintoolbox:
 *   BRSTM / BCSTM / BFSTM     block-interleaved DSP-ADPCM, PCM16 or PCM8
 *   BRWAV / BCWAV / BFWAV     one wave per channel (DSP-ADPCM, PCM16, PCM8)
 *   BNS                       Wii banner sound (DSP-ADPCM)
 *   DSP                       standard DSP-ADPCM header(s)
 *   AST                       GameCube / Wii AFC ADPCM or PCM16 blocks
 *   BTSND                     Wii U boot sound, 48 kHz stereo PCM16
 * Everything is decoded block by block while playing (libogc's ASND), so
 * long streams never need to fit in memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <asndlib.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include "archive.h"

#define MAXCH      16
#define OUT_FRAMES 4096                     /* frames per ASND buffer */
#define FIFO_FRAMES (65536 + OUT_FRAMES)
#define MAX_BLOCK  0x20000                  /* bytes per channel per block */
#define PLANAR_BYTES 0x2000                 /* virtual block for one-wave-per-channel formats */

enum { CO_ADPCM, CO_PCM16, CO_PCM8, CO_AFC, CO_PCM16_IL };
enum { L_INTERLEAVED, L_PLANAR, L_AST };

typedef struct {
    vfs_file f;
    bool le;
    const char *fmt;
    int codec, nch, rate;
    u32 total;
    bool loop;
    u32 loop_start;

    int layout;
    u64 data;                   /* interleaved: first block; planar: unused */
    u32 bs, nblocks, spb, last_used, last_samples, last_pad;
    u64 choff[MAXCH];           /* planar: each channel's data */
    u64 *ast_off;               /* AST: each block's data (after BLCK header) */
    u32 *ast_size, *ast_first;  /* AST: per-channel bytes, first sample */

    s16 coef[MAXCH][16];
    s32 h1[MAXCH], h2[MAXCH];
} snd_t;

/* ---------------------------------------------------------------- reading */

static u16 rd16(const snd_t *s, const u8 *p) { return s->le ? (u16)(p[0] | p[1] << 8) : be16(p); }
static u32 rd32(const snd_t *s, const u8 *p) { return s->le ? (u32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) : be32(p); }

static bool read_at(vfs_file *f, u64 off, void *buf, u32 len)
{
    if (!vfs_seek(f, off)) return false;
    u8 *p = buf;
    while (len) {
        s32 r = vfs_read(f, p, len);
        if (r <= 0) return false;
        p += r;
        len -= r;
    }
    return true;
}

/* Reads [off, off+len) of the file into a fresh aligned buffer. */
static u8 *grab(snd_t *s, u64 off, u32 len)
{
    if (!len || off + len > s->f.size) return NULL;
    u8 *b = memalign(32, (len + 31) & ~31u);
    if (b && !read_at(&s->f, off, b, len)) { free(b); b = NULL; }
    return b;
}

static u32 adpcm_bytes(u32 samples) { return samples / 14 * 8 + (samples % 14 ? (samples % 14 + 1) / 2 + 1 : 0); }
static u32 nibbles_to_samples(u32 n) { return n / 16 * 14 + (n % 16 > 2 ? n % 16 - 2 : 0); }

static void load_coefs(snd_t *s, int ch, const u8 *p)
{
    for (int i = 0; i < 16; i++)
        s->coef[ch][i] = (s16)rd16(s, p + i * 2);
}

/* ---------------------------------------------------------------- formats */

/* RSTM / CSTM / FSTM. `pos` is the HEAD / INFO block, `start` the audio data. */
static bool open_stm(snd_t *s, u8 *h, u32 hlen, bool nw4c)
{
    s->le = be16(h + 4) == 0xFFFE;
    u64 pos, start = 0;
    if (!nw4c) {
        u32 hs = rd16(s, h + 0x0C);
        pos = hs;
        s->fmt = "BRSTM";
    } else {
        u32 hs = rd16(s, h + 6), nsec = rd16(s, h + 0x10);
        pos = 0;
        for (u32 i = 0; i < nsec && 0x14 + i * 12 + 12 <= hs && 0x14 + i * 12 + 12 <= hlen; i++) {
            const u8 *e = h + 0x14 + i * 12;
            u16 id = rd16(s, e);
            if (id == 0x4000) pos = rd32(s, e + 4);
            else if (id == 0x4002) start = rd32(s, e + 4) + 0x20;
        }
        s->fmt = !memcmp(h, "CSTM", 4) ? "BCSTM" : "BFSTM";
        if (!pos || !start) return false;
    }
    u8 *hd = grab(s, pos, 8);
    if (!hd) return false;
    u32 size = rd32(s, hd + 4);
    bool ok_magic = !memcmp(hd, nw4c ? "INFO" : "HEAD", 4);
    free(hd);
    if (!ok_magic || size < 0x40 || size > 0x100000) return false;
    u8 *b = grab(s, pos, size);
    if (!b) return false;
    bool ok = false;
    u32 h1 = rd32(s, b + 0x0C), toff = rd32(s, b + 0x1C) + 16;
    if (h1 + 8 + 0x38 > size || toff > size) goto out;
    const u8 *c = b + h1 + 8;
    int codec = c[0];
    s->loop = c[1] != 0;
    s->nch = c[2];
    if (!s->nch || s->nch > MAXCH) goto out;
    const u8 *q;
    if (nw4c) { s->rate = rd32(s, c + 4); q = c + 8; }
    else      { s->rate = rd16(s, c + 4); q = c + 8; }
    s->loop_start = rd32(s, q);
    s->total = rd32(s, q + 4);
    if (!nw4c) { start = rd32(s, q + 8); q += 12; } else q += 8;
    s->nblocks = rd32(s, q);
    s->bs = rd32(s, q + 4);
    s->spb = rd32(s, q + 8);
    s->last_used = rd32(s, q + 12);
    s->last_samples = rd32(s, q + 16);
    s->last_pad = rd32(s, q + 20);
    if (!s->nblocks || !s->bs || s->bs > MAX_BLOCK || s->last_pad > MAX_BLOCK || !s->rate) goto out;
    s->codec = codec == 0 ? CO_PCM8 : codec == 1 ? CO_PCM16 : codec == 2 ? CO_ADPCM : -1;
    if (s->codec < 0) goto out;
    s->layout = L_INTERLEAVED;
    s->data = start;

    if (s->codec == CO_ADPCM) {                    /* channel table -> channel info -> coefficients */
        u32 table = toff - 8;
        if (table + 4 + 8 * s->nch > size) goto out;
        const u8 *t = b + table;
        u32 count = nw4c ? rd32(s, t) : t[0];
        if (count != (u32)s->nch) goto out;
        for (int ch = 0; ch < s->nch; ch++) {
            u32 o = (nw4c ? table : 8) + rd32(s, t + 4 + ch * 8 + 4);
            if (o + 8 > size) goto out;
            u32 o2 = (nw4c ? o : 8) + rd32(s, b + o + 4);
            if (o2 + 32 > size) goto out;
            load_coefs(s, ch, b + o2);
        }
    }
    ok = true;
out:
    free(b);
    return ok;
}

static bool open_wav(snd_t *s, u8 *h, u32 hlen)
{
    s->le = be16(h + 4) == 0xFFFE;
    bool rw = !memcmp(h, "RWAV", 4);
    s->fmt = rw ? "BRWAV" : !memcmp(h, "CWAV", 4) ? "BCWAV" : "BFWAV";
    u32 info = 0, data = 0;
    if (rw) {
        info = rd32(s, h + 0x10);
        data = rd32(s, h + 0x18);
    } else {
        u32 n = rd16(s, h + 0x10);
        for (u32 i = 0; i < n && 0x14 + i * 12 + 12 <= hlen; i++) {
            const u8 *e = h + 0x14 + i * 12;
            if (rd16(s, e) == 0x7000) info = rd32(s, e + 4);
            else if (rd16(s, e) == 0x7001) data = rd32(s, e + 4);
        }
    }
    if (!info || !data) return false;
    u8 *hd = grab(s, info, 8);
    if (!hd) return false;
    u32 size = rd32(s, hd + 4);
    bool okm = !memcmp(hd, "INFO", 4);
    free(hd);
    if (!okm || size < 0x20 || size > 0x100000) return false;
    u8 *b = grab(s, info, size);
    if (!b) return false;
    const u8 *w = b + 8;
    bool ok = false;
    int enc = w[0];
    s->loop = w[1] != 0;
    if (rw) {
        s->nch = w[2];
        s->rate = rd16(s, w + 4);
        u32 ls = rd32(s, w + 8), end = rd32(s, w + 12), tab = rd32(s, w + 16);
        s->total = enc == 2 ? nibbles_to_samples(end) : end;
        s->loop_start = enc == 2 ? nibbles_to_samples(ls) : ls;
        if (!s->nch || s->nch > MAXCH) goto out;
        for (int ch = 0; ch < s->nch; ch++) {
            if (8 + tab + 4 * ch + 4 > size) goto out;
            u32 ci = rd32(s, w + tab + 4 * ch);
            if (8 + ci + 8 > size) goto out;
            s->choff[ch] = data + 8 + rd32(s, w + ci);
            if (enc == 2) {
                u32 ai = rd32(s, w + ci + 4);
                if (8 + ai + 32 > size) goto out;
                load_coefs(s, ch, w + ai);
            }
        }
    } else {
        s->rate = rd32(s, w + 4);
        s->loop_start = rd32(s, w + 8);
        s->total = rd32(s, w + 12);
        s->nch = rd32(s, w + 0x14);
        if (!s->nch || s->nch > MAXCH) goto out;
        u32 anchor = 0x14;                      /* relative to w */
        for (int ch = 0; ch < s->nch; ch++) {
            if (8 + anchor + 4 + 8 * ch + 8 > size) goto out;
            u32 ci = anchor + rd32(s, w + anchor + 4 + 8 * ch + 4);
            if (8 + ci + 16 > size) goto out;
            s->choff[ch] = data + 8 + rd32(s, w + ci + 4);
            u32 ai = rd32(s, w + ci + 12);
            if (enc == 2 && ai != 0xFFFFFFFF) {
                if (8 + ci + ai + 32 > size) goto out;
                load_coefs(s, ch, w + ci + ai);
            }
        }
    }
    s->codec = enc == 0 ? CO_PCM8 : enc == 1 ? CO_PCM16 : enc == 2 ? CO_ADPCM : -1;
    ok = s->codec >= 0 && s->rate > 0;
out:
    free(b);
    return ok;
}

static bool open_bns(snd_t *s, u64 base)
{
    s->fmt = "BNS";
    u8 *h = grab(s, base, 0x20);
    if (!h) return false;
    u32 hs = be16(h + 0x0C), n = be16(h + 0x0E);
    free(h);
    if (hs < 0x10 || hs > 0x100 || n > 8) return false;
    h = grab(s, base, hs);
    if (!h) return false;
    u64 info = 0;
    u64 data = 0;
    for (u32 i = 0; i < n && 0x10 + i * 8 + 8 <= hs; i++) {
        u32 off = be32(h + 0x10 + i * 8);
        u8 *m = grab(s, base + off, 4);
        if (m && !memcmp(m, "INFO", 4)) info = base + off + 8;
        if (m && !memcmp(m, "DATA", 4)) data = base + off + 8;
        free(m);
    }
    free(h);
    if (!info || !data) return false;
    u8 *b = grab(s, info, 0x200 > s->f.size - info ? (u32)(s->f.size - info) : 0x200);
    if (!b || b[0] != 0) { free(b); return false; }
    bool ok = false;
    s->loop = b[1] != 0;
    s->nch = b[2];
    s->rate = be16(b + 4);
    s->loop_start = be32(b + 8);
    s->total = be32(b + 0x0C);
    u32 list = be32(b + 0x10);
    if (!s->nch || s->nch > 2 || list + 4 * s->nch > 0x200) goto out;
    for (int ch = 0; ch < s->nch; ch++) {
        u32 ci = be32(b + list + 4 * ch);
        if (ci + 8 > 0x200) goto out;
        s->choff[ch] = data + be32(b + ci);
        u32 co = be32(b + ci + 4);
        if (co + 32 > 0x200) goto out;
        load_coefs(s, ch, b + co);
    }
    s->codec = CO_ADPCM;
    ok = s->rate > 0;
out:
    free(b);
    return ok;
}

static bool open_dsp(snd_t *s)
{
    s->fmt = "DSP";
    u8 *h = grab(s, 0, 0x60);
    if (!h) return false;
    u32 samples = be32(h), rate = be32(h + 8);
    if (!samples || !rate || rate > 96000 || be16(h + 0x0E) != 0) { free(h); return false; }
    int declared = be16(h + 0x4A);
    u32 fpb = be16(h + 0x4C);
    s->nch = 1;
    s->rate = rate;
    s->total = samples;
    s->loop = be16(h + 0x0C) != 0;
    s->loop_start = nibbles_to_samples(be32(h + 0x10));
    s->codec = CO_ADPCM;
    load_coefs(s, 0, h + 0x1C);
    free(h);
    if (declared > 1 && declared <= MAXCH && fpb) {        /* interleaved multi-channel DSP */
        for (int ch = 1; ch < declared; ch++) {
            u8 *x = grab(s, ch * 0x60, 0x60);
            if (!x || be32(x) != samples || be32(x + 8) != rate) { free(x); return false; }
            load_coefs(s, ch, x + 0x1C);
            free(x);
        }
        s->nch = declared;
        s->layout = L_INTERLEAVED;
        s->data = 0x60 * declared;
        s->bs = fpb * 8;
        s->spb = fpb * 14;
        s->nblocks = (samples + s->spb - 1) / s->spb;
        s->last_samples = samples - (s->nblocks - 1) * s->spb;
        s->last_used = adpcm_bytes(s->last_samples);
        s->last_pad = s->bs;
        return s->bs <= MAX_BLOCK;
    }
    s->choff[0] = 0x60;
    return true;
}

static bool open_ast(snd_t *s, const u8 *h)
{
    s->fmt = "AST";
    int fmt = be16(h + 8);
    if (be16(h + 0x0A) != 16 || (fmt != 0 && fmt != 1)) return false;
    s->codec = fmt == 0 ? CO_AFC : CO_PCM16;
    s->nch = be16(h + 0x0C);
    s->rate = be32(h + 0x10);
    s->total = be32(h + 0x14);
    s->loop_start = be32(h + 0x18);
    s->loop = be16(h + 0x0E) == 0xFFFF;
    if (!s->nch || s->nch > MAXCH || !s->rate) return false;
    s->layout = L_AST;
    u32 cap = 64, n = 0, first = 0;
    s->ast_off = malloc(cap * sizeof(u64));
    s->ast_size = malloc(cap * sizeof(u32));
    s->ast_first = malloc(cap * sizeof(u32));
    u64 pos = 0x40;
    u8 *bh = memalign(32, 32);
    while (bh && s->ast_off && s->ast_size && s->ast_first && pos + 0x20 <= s->f.size && first < s->total) {
        if (!read_at(&s->f, pos, bh, 8) || memcmp(bh, "BLCK", 4)) break;
        u32 sz = be32(bh + 4);
        if (!sz || sz > MAX_BLOCK) break;
        if (n == cap) {
            cap *= 2;
            void *a = realloc(s->ast_off, cap * sizeof(u64)), *b = realloc(s->ast_size, cap * sizeof(u32)),
                 *c = realloc(s->ast_first, cap * sizeof(u32));
            if (a) s->ast_off = a;
            if (b) s->ast_size = b;
            if (c) s->ast_first = c;
            if (!a || !b || !c) break;
        }
        s->ast_off[n] = pos + 0x20;
        s->ast_size[n] = sz;
        s->ast_first[n] = first;
        first += s->codec == CO_AFC ? sz / 9 * 16 : sz / 2;
        n++;
        pos += 0x20 + (u64)sz * s->nch;
    }
    free(bh);
    s->nblocks = n;
    return n > 0;
}

static bool open_btsnd(snd_t *s)
{
    s->fmt = "BTSND";
    if (s->f.size <= 8) return false;
    u8 *h = grab(s, 0, 8);
    if (!h) return false;
    s->loop_start = be32(h + 4);
    free(h);
    s->codec = CO_PCM16_IL;
    s->nch = 2;
    s->rate = 48000;
    s->total = (u32)((s->f.size - 8) / 4);
    s->loop = s->loop_start > 0 && s->loop_start < s->total;
    s->choff[0] = 8;
    return true;
}

static void snd_close(snd_t *s)
{
    vfs_close(&s->f);
    free(s->ast_off);
    free(s->ast_size);
    free(s->ast_first);
    memset(s, 0, sizeof(*s));
}

/* Detects the format from the first bytes (and the name for DSP / BTSND). */
static bool snd_open(snd_t *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    if (!vfs_open_read(&s->f, path)) return false;
    if (s->f.size < 0x20) { vfs_close(&s->f); return false; }
    u32 hlen = s->f.size < 0x100 ? (u32)s->f.size : 0x100;
    u8 *h = grab(s, 0, hlen);
    bool ok = false;
    if (!h) goto done;
    if (!memcmp(h, "RSTM", 4)) ok = open_stm(s, h, hlen, false);
    else if (!memcmp(h, "CSTM", 4) || !memcmp(h, "FSTM", 4)) ok = open_stm(s, h, hlen, true);
    else if (!memcmp(h, "RWAV", 4) || !memcmp(h, "CWAV", 4) || !memcmp(h, "FWAV", 4)) { s->layout = L_PLANAR; ok = open_wav(s, h, hlen); }
    else if (!memcmp(h, "STRM", 4)) ok = open_ast(s, h);
    else if (!memcmp(h, "BNS ", 4)) { s->layout = L_PLANAR; ok = open_bns(s, 0); }
    else if (!memcmp(h, "IMD5", 4) && hlen >= 0x24 && !memcmp(h + 0x20, "BNS ", 4)) { s->layout = L_PLANAR; ok = open_bns(s, 0x20); }
    else if (arc_name_has_ext(path, ".dsp")) { s->layout = L_PLANAR; ok = open_dsp(s); }
    else if (arc_name_has_ext(path, ".btsnd")) { s->layout = L_PLANAR; ok = open_btsnd(s); }
done:
    free(h);
    if (ok && s->layout == L_PLANAR) {
        u32 per = s->codec == CO_ADPCM ? PLANAR_BYTES / 8 * 14 : s->codec == CO_PCM8 ? PLANAR_BYTES
                : s->codec == CO_PCM16_IL ? PLANAR_BYTES / 4 : PLANAR_BYTES / 2;
        s->spb = per;
        s->nblocks = (s->total + per - 1) / per;
    }
    if (ok && (!s->total || !s->nblocks || s->rate <= 0 || s->rate > 96000)) ok = false;
    if (ok && s->loop_start >= s->total) s->loop = false;
    if (!ok) snd_close(s);
    return ok;
}

/* ---------------------------------------------------------------- decoding */

static u32 block_first(const snd_t *s, u32 b)
{
    return s->layout == L_AST ? s->ast_first[b] : b * s->spb;
}

static u32 block_samples(const snd_t *s, u32 b)
{
    u32 n;
    if (s->layout == L_INTERLEAVED)
        n = b + 1 == s->nblocks ? s->last_samples : s->spb;
    else if (s->layout == L_AST)
        n = s->codec == CO_AFC ? s->ast_size[b] / 9 * 16 : s->ast_size[b] / 2;
    else
        n = s->spb;
    u32 first = block_first(s, b);
    if (first >= s->total) return 0;
    return n > s->total - first ? s->total - first : n;
}

static void block_chan(const snd_t *s, u32 b, int ch, u64 *off, u32 *len)
{
    u32 n = block_samples(s, b);
    switch (s->layout) {
    case L_INTERLEAVED: {
        bool last = b + 1 == s->nblocks;
        *off = s->data + (u64)b * s->bs * s->nch + (u64)ch * (last ? s->last_pad : s->bs);
        *len = last ? s->last_used : s->bs;
        break;
    }
    case L_AST:
        *off = s->ast_off[b] + (u64)ch * s->ast_size[b];
        *len = s->ast_size[b];
        break;
    default:
        if (s->codec == CO_PCM16_IL) { *off = s->choff[0] + (u64)b * PLANAR_BYTES; *len = n * 4; break; }
        *off = s->choff[ch] + (u64)b * PLANAR_BYTES;
        *len = s->codec == CO_ADPCM ? adpcm_bytes(n) : s->codec == CO_PCM8 ? n : n * 2;
        break;
    }
    if (*off + *len > s->f.size)
        *len = *off < s->f.size ? (u32)(s->f.size - *off) : 0;
}

static inline s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

static void dec_adpcm(snd_t *s, int ch, const u8 *in, u32 len, s16 *out, u32 n)
{
    s32 h1 = s->h1[ch], h2 = s->h2[ch];
    u32 k = 0;
    for (u32 fo = 0; fo + 1 < len && k < n; fo += 8) {
        u8 ps = in[fo];
        s32 scale = 1 << (ps & 15);
        s32 c1 = s->coef[ch][(ps >> 4) * 2 & 15], c2 = s->coef[ch][((ps >> 4) * 2 + 1) & 15];
        for (int i = 0; i < 14 && k < n; i++) {
            u32 bi = fo + 1 + i / 2;
            if (bi >= len) break;
            s32 nib = i & 1 ? in[bi] & 15 : in[bi] >> 4;
            if (nib >= 8) nib -= 16;
            s32 v = nib * scale * 2048 + 1024 + c1 * h1 + c2 * h2;   /* rounding as the DSP hardware does it */
            s16 smp = clamp16(v >> 11);
            h2 = h1;
            h1 = smp;
            out[k++] = smp;
        }
    }
    while (k < n) out[k++] = 0;
    s->h1[ch] = h1;
    s->h2[ch] = h2;
}

static const s16 AFC_COEF[2][16] = {
    { 0, 2048, 0, 1024, 4096, 3584, 3072, 4608, 4200, 4800, 5120, 2048, 1024, -1024, -1024, -2048 },
    { 0, 0, 2048, 1024, -2048, -1536, -1024, -2560, -2248, -2300, -3072, -2048, -1024, 1024, 0, 0 },
};

static void dec_afc(snd_t *s, int ch, const u8 *in, u32 len, s16 *out, u32 n)
{
    s32 h1 = s->h1[ch], h2 = s->h2[ch];
    u32 k = 0;
    for (u32 fo = 0; fo + 9 <= len && k < n; fo += 9) {
        s32 scale = 1 << (in[fo] >> 4);
        int idx = in[fo] & 15;
        for (int i = 0; i < 16 && k < n; i++) {
            s32 nib = i & 1 ? in[fo + 1 + i / 2] & 15 : in[fo + 1 + i / 2] >> 4;
            if (nib >= 8) nib -= 16;
            s32 v = ((h1 * AFC_COEF[0][idx] + h2 * AFC_COEF[1][idx]) >> 11) + nib * scale;
            s16 smp = clamp16(v);
            h2 = h1;
            h1 = smp;
            out[k++] = smp;
        }
    }
    while (k < n) out[k++] = 0;
    s->h1[ch] = h1;
    s->h2[ch] = h2;
}

/* Decodes block `b` into planar[ch][0..n). Returns n. */
static u32 decode_block(snd_t *s, u32 b, u8 *raw, s16 *planar[3], int chans[2])
{
    u32 n = block_samples(s, b);
    if (!n) return 0;
    if (s->codec == CO_PCM16_IL) {
        u64 off; u32 len;
        block_chan(s, b, 0, &off, &len);
        if (!len || !read_at(&s->f, off, raw, len)) return 0;
        for (u32 i = 0; i < n; i++) {
            planar[0][i] = (s16)be16(raw + i * 4);
            planar[1][i] = (s16)be16(raw + i * 4 + 2);
        }
        return n;
    }
    /* ADPCM history runs through every channel, even ones not heard. */
    for (int ch = 0; ch < s->nch; ch++) {
        bool heard = ch == chans[0] || ch == chans[1];
        if (!heard && s->codec != CO_ADPCM && s->codec != CO_AFC) continue;
        u64 off; u32 len;
        block_chan(s, b, ch, &off, &len);
        s16 *dst = heard ? planar[ch == chans[0] ? 0 : 1] : planar[2];
        if (!len || len > MAX_BLOCK || !read_at(&s->f, off, raw, len)) { memset(dst, 0, n * 2); continue; }
        switch (s->codec) {
        case CO_ADPCM: dec_adpcm(s, ch, raw, len, dst, n); break;
        case CO_AFC:   dec_afc(s, ch, raw, len, dst, n); break;
        case CO_PCM8:  for (u32 i = 0; i < n; i++) dst[i] = i < len ? (s16)((s8)raw[i] << 8) : 0; break;
        default:       for (u32 i = 0; i < n; i++) dst[i] = i * 2 + 1 < len ? (s16)rd16(s, raw + i * 2) : 0; break;
        }
        if (ch == chans[0] && chans[1] == chans[0])
            memcpy(planar[1], dst, n * 2);
    }
    return n;
}

/* ---------------------------------------------------------------- player */

static void fmt_time(char *o, size_t n, u32 samples, int rate)
{
    u32 t = rate ? samples / (u32)rate : 0;
    snprintf(o, n, "%u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
}

static void draw(const snd_t *s, const char *name, u32 played, bool paused, bool loop, int pair)
{
    int cols = ui_cols(), rows = ui_rows();
    ui_clear();
    ui_bg(C_BLUE);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " AUDIO  %s", name);
    ui_reset();
    ui_color(C_WHITE, false);
    ui_fill(1, 0, cols - 1, '-');
    ui_color(C_WHITE, true);
    const char *codec = s->codec == CO_ADPCM ? "DSP-ADPCM" : s->codec == CO_AFC ? "AFC ADPCM"
                      : s->codec == CO_PCM8 ? "PCM8" : "PCM16";
    ui_field(3, 2, cols - 4, "Format:    %s (%s)", s->fmt, codec);
    ui_field(4, 2, cols - 4, "Channels:  %d   Sample rate: %d Hz", s->nch, s->rate);
    if (s->nch > 2)
        ui_field(5, 2, cols - 4, "Playing:   channels %d + %d", pair * 2 + 1, pair * 2 + 2 <= s->nch ? pair * 2 + 2 : pair * 2 + 1);
    char t1[16], t2[16], t3[16];
    fmt_time(t1, sizeof(t1), played, s->rate);
    fmt_time(t2, sizeof(t2), s->total, s->rate);
    fmt_time(t3, sizeof(t3), s->loop_start, s->rate);
    ui_field(7, 2, cols - 4, "%s  %s / %s   %s", paused ? "PAUSED " : "PLAYING", t1, t2,
             s->loop ? (loop ? "" : "(loop off)") : "");
    if (s->loop)
        ui_field(8, 2, cols - 4, "Loops back to %s", t3);
    int pct = s->total ? (int)((u64)(played % (s->total + 1)) * 100 / s->total) : 0;
    ui_bar(10, 2, cols - 6, pct, C_GREEN);
    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2, "A Pause  B Stop  1 Loop on/off%s", s->nch > 2 ? "  +/- Channels" : "");
    ui_reset();
}

static bool play(const char *path)
{
    snd_t s;
    if (!snd_open(&s, path))
        return false;

    u8 *raw = memalign(32, MAX_BLOCK + 32);
    u32 maxn = 0;
    for (u32 b = 0; b < s.nblocks; b++) {
        u32 n = block_samples(&s, b);
        if (n > maxn) maxn = n;
        if (s.layout != L_AST) break;           /* fixed size: the first block is the largest */
    }
    if (s.layout == L_INTERLEAVED && s.last_samples > maxn) maxn = s.last_samples;
    s16 *pl[3] = { malloc(maxn * 2 + 64), malloc(maxn * 2 + 64), malloc(maxn * 2 + 64) };
    s16 *fifo = malloc(FIFO_FRAMES * 4);
    s16 *out[2] = { memalign(32, OUT_FRAMES * 4), memalign(32, OUT_FRAMES * 4) };
    if (!raw || !pl[0] || !pl[1] || !pl[2] || !fifo || !out[0] || !out[1] || maxn > FIFO_FRAMES - OUT_FRAMES) {
        free(raw); free(pl[0]); free(pl[1]); free(pl[2]); free(fifo); free(out[0]); free(out[1]);
        snd_close(&s);
        ui_message("AUDIO", C_RED, "Not enough memory to play this file.");
        return true;
    }

    ASND_Init();
    ASND_Pause(0);
    s32 voice = ASND_GetFirstUnusedVoice();

    u32 block = 0, fifo_n = 0, skip = 0, decoded_to = 0, submitted = 0;
    bool loop = s.loop, paused = false, started = false, ended = false;
    int pair = 0, chans[2] = { 0, s.nch > 1 ? 1 : 0 };
    u32 loop_block = 0;
    s32 loop_h1[MAXCH], loop_h2[MAXCH];
    bool have_loop_state = false;
    if (s.loop)
        for (loop_block = 0; loop_block + 1 < s.nblocks && block_first(&s, loop_block + 1) <= s.loop_start; loop_block++) ;
    int cur = 0, frame = 0;
    const char *name = vfs_basename(path);

    for (;;) {
        /* keep the FIFO topped up */
        while (!ended && fifo_n < OUT_FRAMES * 2) {
            if (block >= s.nblocks || decoded_to >= s.total) {
                if (loop && have_loop_state) {
                    block = loop_block;
                    memcpy(s.h1, loop_h1, sizeof(loop_h1));
                    memcpy(s.h2, loop_h2, sizeof(loop_h2));
                    skip = s.loop_start - block_first(&s, block);
                    decoded_to = block_first(&s, block);
                    continue;
                }
                ended = true;
                break;
            }
            if (s.loop && block == loop_block && !have_loop_state) {
                memcpy(loop_h1, s.h1, sizeof(loop_h1));
                memcpy(loop_h2, s.h2, sizeof(loop_h2));
                have_loop_state = true;
            }
            u32 n = decode_block(&s, block, raw, pl, chans);
            block++;
            decoded_to += n;
            for (u32 i = skip; i < n && fifo_n < FIFO_FRAMES; i++) {
                fifo[fifo_n * 2] = pl[0][i];
                fifo[fifo_n * 2 + 1] = pl[1][i];
                fifo_n++;
            }
            skip = skip > n ? skip - n : 0;
            if (!n) { ended = true; break; }
        }

        /* hand full buffers to ASND */
        if (!paused && (fifo_n >= OUT_FRAMES || (ended && fifo_n))) {
            bool ready = !started || ASND_TestVoiceBufferReady(voice);
            if (ready) {
                u32 take = fifo_n < OUT_FRAMES ? fifo_n : OUT_FRAMES;
                memcpy(out[cur], fifo, take * 4);
                if (take < OUT_FRAMES) memset((u8 *)out[cur] + take * 4, 0, (OUT_FRAMES - take) * 4);
                memmove(fifo, fifo + take * 2, (fifo_n - take) * 4);
                fifo_n -= take;
                DCFlushRange(out[cur], OUT_FRAMES * 4);
                if (!started) {
                    ASND_SetVoice(voice, VOICE_STEREO_16BIT, s.rate, 0, out[cur], OUT_FRAMES * 4, 255, 255, NULL);
                    started = true;
                } else {
                    ASND_AddVoice(voice, out[cur], OUT_FRAMES * 4);
                }
                submitted += take;
                cur ^= 1;
                continue;               /* fill the second buffer straight away */
            }
        }
        if (ended && !fifo_n && started && ASND_StatusVoice(voice) == SND_UNUSED)
            break;

        u32 p = DetectInput(IN_DOWN);
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            break;
        if (p & WPAD_BUTTON_A) {
            paused = !paused;
            if (started) ASND_PauseVoice(voice, paused);
        }
        if ((p & WPAD_BUTTON_1) && s.loop)
            loop = !loop;
        if ((p & (WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS)) && s.nch > 2) {
            int pairs = (s.nch + 1) / 2;
            pair = (pair + (p & WPAD_BUTTON_PLUS ? 1 : pairs - 1)) % pairs;
            chans[0] = pair * 2;
            chans[1] = pair * 2 + 1 < s.nch ? pair * 2 + 1 : pair * 2;
        }
        if (p || ++frame % 15 == 0) {
            u32 played = submitted > OUT_FRAMES * 2 ? submitted - OUT_FRAMES * 2 : 0;
            if (s.loop && loop && played > s.total)
                played = s.loop_start + (played - s.total) % (s.total - s.loop_start ? s.total - s.loop_start : 1);
            draw(&s, name, played > s.total ? s.total : played, paused, loop, pair);
        }
    }

    ASND_StopVoice(voice);
    ASND_End();
    free(raw); free(pl[0]); free(pl[1]); free(pl[2]); free(fifo); free(out[0]); free(out[1]);
    snd_close(&s);
    return true;
}

bool audio_is_name(const char *name)
{
    static const char *const ext[] = { ".brstm", ".bcstm", ".bfstm", ".brwav", ".rwav", ".bcwav", ".cwav", ".bfwav",
                                       ".fwav", ".bns", ".dsp", ".ast", ".btsnd", NULL };
    for (int i = 0; ext[i]; i++)
        if (arc_name_has_ext(name, ext[i]))
            return true;
    return false;
}

bool audio_play(const char *path)
{
    if (play(path))
        return true;
    return false;
}

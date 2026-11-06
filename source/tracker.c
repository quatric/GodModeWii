/*
 * Tracker module player: ProTracker .mod, ScreamTracker .s3m, FastTracker 2 .xm, Impulse Tracker .it.
 *
 * Every format is loaded into one model (patterns of cells, instruments, 16-bit samples) and played by
 * one engine, so a pitch is always a note number and effects are translated once, on load. Pitch is
 * kept in semitones; the Amiga formats slide in period units (converted around each slide), XM and IT
 * in linear steps. Mixing is linear-interpolated, with a short volume ramp against clicks.
 *
 * Supported: arpeggio, slides (fine and extra-fine), tone portamento, vibrato, tremolo, volume slides,
 * sample offset, retrigger, note cut / delay / key off, pattern loop and delay, jump, break, speed,
 * tempo, global volume, panning, XM / IT volume columns, XM / IT volume envelopes with fadeout,
 * ping-pong loops, compressed IT samples. Not supported: NNA / duplicate checks, pitch and panning
 * envelopes, auto-vibrato, filters, channel volume, tremor.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

#include "tracker.h"

#define MAX_CHAN 64
#define MAX_ROWS 256
#define MAX_SAMPLE_BYTES (24u << 20)
#define MAX_SECONDS 3600

enum {
    FX_NONE, FX_ARP, FX_SLIDEUP, FX_SLIDEDN, FX_PORTA, FX_VIB, FX_PORTAVOL, FX_VIBVOL, FX_TREM, FX_PAN,
    FX_OFFSET, FX_VOLSLIDE, FX_JUMP, FX_BREAK, FX_SETVOL, FX_SPEED, FX_TEMPO, FX_FINEUP, FX_FINEDN,
    FX_FINEVOLUP, FX_FINEVOLDN, FX_CUT, FX_DELAY, FX_RETRIG, FX_GVOL, FX_PATDELAY, FX_VIBDEPTH,
    FX_LOOP, FX_KEYOFF, FX_COUNT
};

enum { FMT_MOD, FMT_S3M, FMT_XM, FMT_IT };

typedef struct {
    uint8_t note;               /* 0 none, pitch + 1, 254 cut, 255 key off */
    uint8_t inst;
    uint8_t c1, p1;             /* volume column */
    uint8_t c2, p2;             /* effect column */
} cell_t;

typedef struct {
    int16_t *data;
    uint32_t len, loop_start, loop_end;
    uint8_t loop;               /* 0 none, 1 forward, 2 ping-pong */
    uint8_t vol;                /* 0..64 */
    uint8_t gv;                 /* IT global volume 0..64, plus one; 0 = full */
    int pan;                    /* -1 none, 0..255 */
    float c5;                   /* Hz at note 60 */
    float tune;                 /* semitones added to the note (relative note, fine tune) */
} smp_t;

typedef struct {
    uint8_t on, sus, loop;
    uint8_t n, sus_beg, sus_end, loop_beg, loop_end;
    uint16_t tick[25];
    uint8_t val[25];
} env_t;

typedef struct {
    uint16_t map_smp[120];      /* sample for each note; 0 = none, else index + 1 */
    uint8_t map_note[120];      /* note played instead (IT keyboard table) */
    env_t env;
    uint16_t fadeout;           /* per tick, out of 65536 */
    uint8_t gv;                 /* IT global volume 0..128, plus one; 0 = full */
} inst_t;

typedef struct { int rows; cell_t *cells; } pat_t;

typedef struct {
    const smp_t *smp;
    const inst_t *inst;
    int inst_no;
    int64_t pos;                /* 16.16 */
    int dir;
    bool playing;
    float pitch, target, offs;  /* note, porta target, temporary offset (vibrato, arpeggio) */
    int vol, pan, trem;
    bool key_on;
    int fade, env_tick;
    uint8_t vib_pos, vib_spd, vib_dep, trem_pos, trem_spd, trem_dep;
    uint8_t mem[FX_COUNT];
    uint8_t porta;
    int loop_row, loop_cnt;
    int cut_at, delay_at, key_at, retrig;
    cell_t pending;
    int curl, curr;             /* Q12 gains actually used, ramping to ... */
    int dstl, dstr;             /* ... these */
    float freq;
    int chan_pan;
} chan_t;

struct trk {
    int fmt, rate;
    char title[32];
    int nchan, norders, npat, ninst, nsmp;
    uint8_t *orders;
    pat_t *pats;
    inst_t *insts;              /* [ninst + 1] */
    smp_t *smps;                /* [nsmp] */
    bool linear;
    int init_speed, init_tempo, init_gvol;
    uint8_t init_pan[MAX_CHAN];
    uint64_t total;

    /* play state */
    chan_t ch[MAX_CHAN];
    int speed, tempo, gvol;
    int order, row, tick, patdelay, delaying;
    int nxt_order, nxt_row;
    bool loop_back;
    bool ended;
    float tick_frac;
    int tick_left;
    uint8_t *visited;           /* norders * MAX_ROWS bits */
    int master;
};

/* ---------------------------------------------------------------- helpers */

typedef struct { const uint8_t *d; uint32_t n; } buf_t;
static inline uint8_t r8(buf_t b, uint32_t o) { return o < b.n ? b.d[o] : 0; }
static inline uint16_t r16(buf_t b, uint32_t o) { return (uint16_t)(r8(b, o) | r8(b, o + 1) << 8); }
static inline uint32_t r32(buf_t b, uint32_t o) { return r16(b, o) | (uint32_t)r16(b, o + 2) << 16; }
static inline uint16_t r16b(buf_t b, uint32_t o) { return (uint16_t)(r8(b, o) << 8 | r8(b, o + 1)); }
static bool in_range(buf_t b, uint64_t o, uint64_t len) { return o <= b.n && len <= b.n - o; }

static void copy_name(char *out, size_t cap, buf_t b, uint32_t o, uint32_t len)
{
    size_t i = 0;
    for (; i < len && i + 1 < cap && o + i < b.n; i++) {
        uint8_t c = b.d[o + i];
        if (!c) break;
        out[i] = c >= 32 && c < 127 ? (char)c : ' ';
    }
    out[i] = 0;
}

static float semi_to_hz(float c5, float semis) { return c5 * exp2f(semis / 12.0f); }
static float pitch_to_period(float pitch) { return 428.0f * exp2f((60.0f - pitch) / 12.0f); }
static float period_to_pitch(float period) { return 60.0f - 12.0f * log2f(period / 428.0f); }

static void free_module(trk_t *t)
{
    if (!t) return;
    if (t->pats)
        for (int i = 0; i < t->npat; i++) free(t->pats[i].cells);
    free(t->pats);
    free(t->orders);
    free(t->insts);
    if (t->smps)
        for (int i = 0; i < t->nsmp; i++) free(t->smps[i].data);
    free(t->smps);
    free(t->visited);
    free(t);
}

/* Reserve sample memory against a budget so a damaged header can't take the console's heap. */
static int16_t *alloc_pcm(uint32_t frames, uint32_t *budget)
{
    if (!frames || frames * 2 > *budget) return NULL;
    int16_t *p = calloc(frames, 2);
    if (p) *budget -= frames * 2;
    return p;
}

static void set_loop(smp_t *s, uint32_t start, uint32_t end, int kind)
{
    if (kind && end > start + 1 && end <= s->len) {
        s->loop = (uint8_t)kind;
        s->loop_start = start;
        s->loop_end = end;
    }
}

/* ---------------------------------------------------------------- MOD */

static uint8_t mod_vol_pan(int c) { return (c & 3) == 1 || (c & 3) == 2 ? 0xB0 : 0x50; }

static bool mod_sig_channels(const uint8_t *s, int *chans)
{
    if (!memcmp(s, "M.K.", 4) || !memcmp(s, "M!K!", 4) || !memcmp(s, "FLT4", 4) || !memcmp(s, "4CHN", 4)) *chans = 4;
    else if (!memcmp(s, "6CHN", 4)) *chans = 6;
    else if (!memcmp(s, "8CHN", 4) || !memcmp(s, "FLT8", 4) || !memcmp(s, "CD81", 4) || !memcmp(s, "OKTA", 4) ||
             !memcmp(s, "OCTA", 4)) *chans = 8;
    else if (s[0] >= '1' && s[0] <= '9' && !memcmp(s + 1, "CHN", 3)) *chans = s[0] - '0';
    else if (s[0] >= '1' && s[0] <= '3' && s[1] >= '0' && s[1] <= '9' && !memcmp(s + 2, "CH", 2)) {
        *chans = (s[0] - '0') * 10 + (s[1] - '0');
        if (*chans > 32) return false;
    } else
        return false;
    return true;
}

static bool mod_channels(buf_t b, int *chans) { return b.n >= 1084 && mod_sig_channels(b.d + 1080, chans); }

bool trk_is_mod_sig(const uint8_t *sig) { int c; return mod_sig_channels(sig, &c); }

static void fx_mod(cell_t *c, int eff, int par)
{
    uint8_t code = FX_NONE, p = (uint8_t)par;
    switch (eff) {
    case 0x0: if (par) code = FX_ARP; break;
    case 0x1: code = FX_SLIDEUP; break;
    case 0x2: code = FX_SLIDEDN; break;
    case 0x3: code = FX_PORTA; break;
    case 0x4: code = FX_VIB; break;
    case 0x5: code = FX_PORTAVOL; break;
    case 0x6: code = FX_VIBVOL; break;
    case 0x7: code = FX_TREM; break;
    case 0x8: code = FX_PAN; break;
    case 0x9: code = FX_OFFSET; break;
    case 0xA: code = FX_VOLSLIDE; break;
    case 0xB: code = FX_JUMP; break;
    case 0xC: code = FX_SETVOL; p = par > 64 ? 64 : (uint8_t)par; break;
    case 0xD: code = FX_BREAK; p = (uint8_t)((par >> 4) * 10 + (par & 15)); break;
    case 0xE:
        p = (uint8_t)(par & 15);
        switch (par >> 4) {
        case 1: code = FX_FINEUP; break;
        case 2: code = FX_FINEDN; break;
        case 6: code = FX_LOOP; break;
        case 9: code = FX_RETRIG; break;
        case 0xA: code = FX_FINEVOLUP; break;
        case 0xB: code = FX_FINEVOLDN; break;
        case 0xC: code = FX_CUT; break;
        case 0xD: code = FX_DELAY; break;
        case 0xE: code = FX_PATDELAY; break;
        }
        break;
    case 0xF: if (par) { code = par < 32 ? FX_SPEED : FX_TEMPO; } break;
    }
    c->c2 = code;
    c->p2 = p;
}

static bool load_mod(trk_t *t, buf_t b)
{
    int chans;
    if (!mod_channels(b, &chans)) return false;
    t->fmt = FMT_MOD;
    t->nchan = chans;
    t->init_speed = 6;
    t->init_tempo = 125;
    t->init_gvol = 64;
    copy_name(t->title, sizeof(t->title), b, 0, 20);
    int song_len = r8(b, 950);
    if (song_len < 1 || song_len > 128) song_len = 128;
    t->norders = song_len;
    t->orders = malloc(song_len);
    int maxpat = 0;
    for (int i = 0; i < 128; i++) {
        uint8_t o = r8(b, 952 + i);
        if (o > maxpat) maxpat = o;
        if (i < song_len) t->orders[i] = o;
    }
    t->npat = maxpat + 1;
    t->pats = calloc(t->npat, sizeof(pat_t));
    uint32_t pos = 1084;
    for (int p = 0; p < t->npat; p++) {
        t->pats[p].rows = 64;
        t->pats[p].cells = calloc(64 * chans, sizeof(cell_t));
        if (!t->pats[p].cells) return false;
        for (int i = 0; i < 64 * chans; i++, pos += 4) {
            cell_t *c = &t->pats[p].cells[i];
            uint8_t b0 = r8(b, pos), b1 = r8(b, pos + 1), b2 = r8(b, pos + 2), b3 = r8(b, pos + 3);
            int period = (b0 & 15) << 8 | b1;
            c->inst = (uint8_t)((b0 & 0xF0) | (b2 >> 4));
            if (period) {
                int pitch = 48 + (int)lroundf(12.0f * log2f(856.0f / period));
                c->note = (uint8_t)(pitch < 0 ? 1 : pitch > 119 ? 120 : pitch + 1);
            }
            fx_mod(c, b2 & 15, b3);
        }
    }
    t->nsmp = 31;
    t->ninst = 31;
    t->smps = calloc(31, sizeof(smp_t));
    t->insts = calloc(32, sizeof(inst_t));
    uint32_t budget = MAX_SAMPLE_BYTES;
    for (int i = 0; i < 31; i++) {
        uint32_t h = 20 + 30 * i;
        smp_t *s = &t->smps[i];
        uint32_t len = r16b(b, h + 22) * 2u;
        int fine = r8(b, h + 24) & 15;
        if (fine > 7) fine -= 16;
        s->vol = r8(b, h + 25) > 64 ? 64 : r8(b, h + 25);
        s->pan = -1;
        s->c5 = 8363.0f;
        s->tune = fine / 8.0f;
        uint32_t ls = r16b(b, h + 26) * 2u, ll = r16b(b, h + 28) * 2u;
        if (!in_range(b, pos, 0) || pos >= b.n) len = 0;
        if (pos + len > b.n) len = b.n - pos;
        s->data = len > 2 ? alloc_pcm(len, &budget) : NULL;
        if (s->data) {
            s->len = len;
            for (uint32_t k = 0; k < len; k++) s->data[k] = (int16_t)((int8_t)b.d[pos + k] * 256);
            if (ll > 2) set_loop(s, ls, ls + ll > len ? len : ls + ll, 1);
        }
        pos += len;
        inst_t *in = &t->insts[i + 1];
        for (int n = 0; n < 120; n++) { in->map_smp[n] = (uint16_t)(i + 1); in->map_note[n] = (uint8_t)n; }
    }
    for (int c = 0; c < chans; c++) t->init_pan[c] = mod_vol_pan(c);
    return true;
}

/* ---------------------------------------------------------------- S3M */

static void fx_s3m(cell_t *c, int cmd, int par, uint8_t *last)
{
    uint8_t code = FX_NONE, p = (uint8_t)par;
    int x = par >> 4, y = par & 15;
    if (cmd == 0) return;
    char letter = (char)('A' + cmd - 1);
    /* most S3M effects remember the last parameter of their own letter */
    if (par) last[cmd & 31] = (uint8_t)par; else if (cmd != 1 && cmd != 2 && cmd != 3 && cmd != 20) p = par = last[cmd & 31], x = par >> 4, y = par & 15;
    switch (letter) {
    case 'A': if (par) code = FX_SPEED; break;
    case 'B': code = FX_JUMP; break;
    case 'C': code = FX_BREAK; p = (uint8_t)(x * 10 + y); break;
    case 'D':
        if (y == 0xF && x) { code = FX_FINEVOLUP; p = (uint8_t)x; }
        else if (x == 0xF && y) { code = FX_FINEVOLDN; p = (uint8_t)y; }
        else code = FX_VOLSLIDE;
        break;
    case 'E':
        if (par >= 0xF0) { code = FX_FINEDN; p = (uint8_t)y; }
        else if (par >= 0xE0) { code = FX_FINEDN; p = (uint8_t)(y / 4); }
        else code = FX_SLIDEDN;
        break;
    case 'F':
        if (par >= 0xF0) { code = FX_FINEUP; p = (uint8_t)y; }
        else if (par >= 0xE0) { code = FX_FINEUP; p = (uint8_t)(y / 4); }
        else code = FX_SLIDEUP;
        break;
    case 'G': code = FX_PORTA; break;
    case 'H': code = FX_VIB; break;
    case 'J': code = FX_ARP; break;
    case 'K': code = FX_VIBVOL; break;
    case 'L': code = FX_PORTAVOL; break;
    case 'O': code = FX_OFFSET; break;
    case 'Q': code = FX_RETRIG; p = (uint8_t)y; break;
    case 'R': code = FX_TREM; break;
    case 'S':
        p = (uint8_t)y;
        switch (x) {
        case 8: code = FX_PAN; p = (uint8_t)(y * 17); break;
        case 0xB: code = FX_LOOP; break;
        case 0xC: code = FX_CUT; break;
        case 0xD: code = FX_DELAY; break;
        case 0xE: code = FX_PATDELAY; break;
        }
        break;
    case 'T': if (par >= 32) code = FX_TEMPO; break;
    case 'V': code = FX_GVOL; p = par > 64 ? 64 : (uint8_t)par; break;
    case 'X': code = FX_PAN; p = (uint8_t)par; break;
    }
    c->c2 = code;
    c->p2 = p;
}

static bool load_s3m(trk_t *t, buf_t b)
{
    if (b.n < 0x60 || memcmp(b.d + 0x2C, "SCRM", 4)) return false;
    t->fmt = FMT_S3M;
    copy_name(t->title, sizeof(t->title), b, 0, 28);
    int ordnum = r16(b, 0x20), insnum = r16(b, 0x22), patnum = r16(b, 0x24);
    int ffi = r16(b, 0x2A);
    t->init_gvol = r8(b, 0x30) > 64 ? 64 : r8(b, 0x30);
    t->init_speed = r8(b, 0x31) ? r8(b, 0x31) : 6;
    t->init_tempo = r8(b, 0x32) >= 32 ? r8(b, 0x32) : 125;
    bool stereo = r8(b, 0x33) & 0x80;
    if (ordnum < 1 || ordnum > 256 || insnum > 99 || patnum > 256) return false;
    t->orders = malloc(ordnum);
    t->norders = 0;
    for (int i = 0; i < ordnum; i++) {
        uint8_t o = r8(b, 0x60 + i);
        if (o == 255) break;
        if (o < 254) t->orders[t->norders++] = o;
    }
    if (!t->norders) return false;
    uint32_t ins_at = 0x60 + ordnum, pat_at = ins_at + insnum * 2;
    int map[32], nch = 0;
    for (int c = 0; c < 32; c++) {
        uint8_t cs = r8(b, 0x40 + c);
        map[c] = cs < 16 ? nch : -1;
        if (cs < 16) {
            if (nch < MAX_CHAN) t->init_pan[nch] = !stereo ? 0x80 : cs < 8 ? 0x30 : 0xC0;
            nch++;
        }
    }
    if (nch < 1) return false;
    t->nchan = nch;
    t->npat = patnum;
    t->pats = calloc(patnum ? patnum : 1, sizeof(pat_t));
    for (int p = 0; p < patnum; p++) {
        pat_t *pt = &t->pats[p];
        pt->rows = 64;
        pt->cells = calloc(64 * nch, sizeof(cell_t));
        if (!pt->cells) return false;
        uint32_t o = r16(b, pat_at + 2 * p) * 16u;
        if (!o || !in_range(b, o, 2)) continue;
        uint32_t end = o + 2 + r16(b, o), pos = o + 2;
        if (end > b.n) end = b.n;
        int row = 0;
        uint8_t last[32] = { 0 };
        while (pos < end && row < 64) {
            uint8_t v = r8(b, pos++);
            if (!v) { row++; continue; }
            int chn = v & 31;
            cell_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (v & 0x20) {
                uint8_t n = r8(b, pos), in = r8(b, pos + 1);
                pos += 2;
                tmp.inst = in;
                if (n == 254) tmp.note = 254;
                else if (n != 255) {
                    int pitch = (n >> 4) * 12 + (n & 15) + 12;
                    tmp.note = (uint8_t)(pitch > 119 ? 120 : pitch + 1);
                }
            }
            if (v & 0x40) {
                uint8_t vol = r8(b, pos++);
                if (vol <= 64) { tmp.c1 = FX_SETVOL; tmp.p1 = vol; }
            }
            if (v & 0x80) {
                uint8_t cmd = r8(b, pos), par = r8(b, pos + 1);
                pos += 2;
                fx_s3m(&tmp, cmd, par, last);
            }
            if (map[chn] >= 0) pt->cells[row * nch + map[chn]] = tmp;
        }
    }
    t->nsmp = insnum;
    t->ninst = insnum;
    t->smps = calloc(insnum ? insnum : 1, sizeof(smp_t));
    t->insts = calloc(insnum + 1, sizeof(inst_t));
    uint32_t budget = MAX_SAMPLE_BYTES;
    for (int i = 0; i < insnum; i++) {
        uint32_t o = r16(b, ins_at + 2 * i) * 16u;
        smp_t *s = &t->smps[i];
        s->pan = -1;
        s->c5 = 8363.0f;
        if (!o || !in_range(b, o, 0x50) || r8(b, o) != 1) continue;
        uint32_t mem = ((r8(b, o + 0x0D) << 16) | r16(b, o + 0x0E)) * 16u;
        uint32_t len = r32(b, o + 0x10), ls = r32(b, o + 0x14), le = r32(b, o + 0x18);
        uint8_t flags = r8(b, o + 0x1F);
        bool sixteen = flags & 4, stereo_s = flags & 2;
        s->vol = r8(b, o + 0x1C) > 64 ? 64 : r8(b, o + 0x1C);
        uint32_t c2 = r32(b, o + 0x20);
        s->c5 = c2 ? (float)c2 : 8363.0f;
        uint32_t bytes = len * (sixteen ? 2 : 1) * (stereo_s ? 2 : 1);
        if (!len || !mem || !in_range(b, mem, bytes)) continue;
        s->data = alloc_pcm(len, &budget);
        if (!s->data) continue;
        s->len = len;
        for (uint32_t k = 0; k < len; k++) {
            if (sixteen) {
                int v = (int16_t)r16(b, mem + k * 2);
                s->data[k] = (int16_t)(ffi == 2 ? v - 32768 : v);
            } else {
                int v = b.d[mem + k];
                s->data[k] = (int16_t)((ffi == 2 ? v - 128 : (int8_t)v) * 256);
            }
        }
        if (flags & 1) set_loop(s, ls, le > len ? len : le, 1);
        inst_t *in = &t->insts[i + 1];
        for (int n = 0; n < 120; n++) { in->map_smp[n] = (uint16_t)(i + 1); in->map_note[n] = (uint8_t)n; }
    }
    return true;
}

/* ---------------------------------------------------------------- XM */

static void fx_xm(cell_t *c, int cmd, int par)
{
    uint8_t code = FX_NONE, p = (uint8_t)par;
    switch (cmd) {
    case 0x0: if (par) code = FX_ARP; break;
    case 0x1: code = FX_SLIDEUP; break;
    case 0x2: code = FX_SLIDEDN; break;
    case 0x3: code = FX_PORTA; break;
    case 0x4: code = FX_VIB; break;
    case 0x5: code = FX_PORTAVOL; break;
    case 0x6: code = FX_VIBVOL; break;
    case 0x7: code = FX_TREM; break;
    case 0x8: code = FX_PAN; break;
    case 0x9: code = FX_OFFSET; break;
    case 0xA: code = FX_VOLSLIDE; break;
    case 0xB: code = FX_JUMP; break;
    case 0xC: code = FX_SETVOL; p = par > 64 ? 64 : (uint8_t)par; break;
    case 0xD: code = FX_BREAK; p = (uint8_t)((par >> 4) * 10 + (par & 15)); break;
    case 0xE:
        p = (uint8_t)(par & 15);
        switch (par >> 4) {
        case 1: code = FX_FINEUP; break;
        case 2: code = FX_FINEDN; break;
        case 6: code = FX_LOOP; break;
        case 9: code = FX_RETRIG; break;
        case 0xA: code = FX_FINEVOLUP; break;
        case 0xB: code = FX_FINEVOLDN; break;
        case 0xC: code = FX_CUT; break;
        case 0xD: code = FX_DELAY; break;
        case 0xE: code = FX_PATDELAY; break;
        }
        break;
    case 0xF: if (par) code = par < 32 ? FX_SPEED : FX_TEMPO; break;
    case 16: code = FX_GVOL; p = par > 64 ? 64 : (uint8_t)par; break;           /* G */
    case 20: code = FX_KEYOFF; break;                                          /* K */
    case 27: code = FX_RETRIG; p = (uint8_t)(par & 15); break;                 /* R */
    }
    c->c2 = code;
    c->p2 = p;
}

static void vol_xm(cell_t *c, int v)
{
    int x = v >> 4, y = v & 15;
    if (v >= 0x10 && v <= 0x50) { c->c1 = FX_SETVOL; c->p1 = (uint8_t)(v - 0x10); return; }
    switch (x) {
    case 0x6: c->c1 = FX_VOLSLIDE; c->p1 = (uint8_t)y; break;                   /* down */
    case 0x7: c->c1 = FX_VOLSLIDE; c->p1 = (uint8_t)(y << 4); break;            /* up */
    case 0x8: c->c1 = FX_FINEVOLDN; c->p1 = (uint8_t)y; break;
    case 0x9: c->c1 = FX_FINEVOLUP; c->p1 = (uint8_t)y; break;
    case 0xA: c->c1 = FX_VIB; c->p1 = (uint8_t)(y << 4); break;                 /* speed */
    case 0xB: c->c1 = FX_VIBDEPTH; c->p1 = (uint8_t)y; break;
    case 0xC: c->c1 = FX_PAN; c->p1 = (uint8_t)(y * 17); break;
    case 0xF: c->c1 = FX_PORTA; c->p1 = (uint8_t)(y << 4); break;
    }
}

static void env_xm(env_t *e, buf_t b, uint32_t pts, int n, int sus, int ls, int le, int type)
{
    if (n > 12) n = 12;
    e->on = (type & 1) && n >= 2;
    e->sus = type & 2;
    e->loop = type & 4;
    e->n = (uint8_t)n;
    e->sus_beg = e->sus_end = (uint8_t)sus;
    e->loop_beg = (uint8_t)ls;
    e->loop_end = (uint8_t)le;
    for (int i = 0; i < n; i++) {
        e->tick[i] = r16(b, pts + 4 * i);
        int v = r16(b, pts + 4 * i + 2);
        e->val[i] = (uint8_t)(v > 64 ? 64 : v);
    }
}

static bool load_xm(trk_t *t, buf_t b)
{
    if (b.n < 80 || memcmp(b.d, "Extended Module: ", 17)) return false;
    t->fmt = FMT_XM;
    copy_name(t->title, sizeof(t->title), b, 17, 20);
    uint32_t hsize = r32(b, 60);
    int song_len = r16(b, 64), chans = r16(b, 68), npat = r16(b, 70), ninst = r16(b, 72);
    t->linear = r16(b, 74) & 1;
    t->init_speed = r16(b, 76) ? r16(b, 76) : 6;
    t->init_tempo = r16(b, 78) >= 32 ? r16(b, 78) : 125;
    t->init_gvol = 64;
    if (chans < 1 || chans > 32 || song_len < 1 || song_len > 256 || npat > 256 || ninst > 128) return false;
    t->nchan = chans;
    t->norders = song_len;
    t->orders = malloc(song_len);
    for (int i = 0; i < song_len; i++) t->orders[i] = r8(b, 80 + i);
    uint32_t pos = 60 + hsize;
    t->npat = npat > 0 ? npat : 1;
    t->pats = calloc(t->npat, sizeof(pat_t));
    for (int p = 0; p < npat; p++) {
        uint32_t plen = r32(b, pos);
        int rows = r16(b, pos + 5);
        uint32_t psize = r16(b, pos + 7);
        if (plen < 9 || rows < 1 || rows > 256) return false;
        pat_t *pt = &t->pats[p];
        pt->rows = rows;
        pt->cells = calloc((size_t)rows * chans, sizeof(cell_t));
        if (!pt->cells) return false;
        uint32_t q = pos + plen, end = q + psize;
        if (end > b.n) end = b.n;
        for (int i = 0; i < rows * chans && q < end; i++) {
            cell_t *c = &pt->cells[i];
            uint8_t f = r8(b, q++), note = 0, inst = 0, vol = 0, cmd = 0, par = 0;
            if (f & 0x80) {
                if (f & 1) note = r8(b, q++);
                if (f & 2) inst = r8(b, q++);
                if (f & 4) vol = r8(b, q++);
                if (f & 8) cmd = r8(b, q++);
                if (f & 16) par = r8(b, q++);
            } else {
                note = f; inst = r8(b, q); vol = r8(b, q + 1); cmd = r8(b, q + 2); par = r8(b, q + 3);
                q += 4;
            }
            if (note == 97) c->note = 255;
            else if (note >= 1 && note <= 96) c->note = (uint8_t)(note - 1 + 12 + 1);
            c->inst = inst;
            vol_xm(c, vol);
            fx_xm(c, cmd, par);
        }
        pos = end;
    }
    t->ninst = ninst;
    t->insts = calloc(ninst + 1, sizeof(inst_t));
    smp_t *all = NULL;
    int nall = 0, capall = 0;
    uint32_t budget = MAX_SAMPLE_BYTES;
    for (int i = 0; i < ninst; i++) {
        if (pos + 29 > b.n) break;
        uint32_t isize = r32(b, pos);
        int ns = r16(b, pos + 27);
        if (isize < 29) break;
        inst_t *in = &t->insts[i + 1];
        uint32_t next = pos + isize;
        if (ns > 0) {
            uint32_t ssize = r32(b, pos + 29);
            if (ssize < 18) ssize = 40;
            uint8_t keymap[96];
            for (int k = 0; k < 96; k++) keymap[k] = r8(b, pos + 33 + k);
            if (isize >= 239) {
                env_xm(&in->env, b, pos + 129, r8(b, pos + 225), r8(b, pos + 227), r8(b, pos + 228),
                       r8(b, pos + 229), r8(b, pos + 233));
                in->fadeout = (uint16_t)(r16(b, pos + 239) * 2);
            }
            uint32_t sp = pos + isize, data_at = sp + ssize * (uint32_t)ns;
            int base = nall;
            for (int s = 0; s < ns; s++) {
                if (nall == capall) {
                    capall = capall ? capall * 2 : 64;
                    all = realloc(all, capall * sizeof(smp_t));
                }
                smp_t *sm = &all[nall++];
                memset(sm, 0, sizeof(*sm));
                uint32_t h = sp + ssize * (uint32_t)s;
                uint32_t len = r32(b, h), ls = r32(b, h + 4), ll = r32(b, h + 8);
                uint8_t type = r8(b, h + 14);
                bool sixteen = type & 16;
                int8_t rel = (int8_t)r8(b, h + 16), fine = (int8_t)r8(b, h + 13);
                sm->vol = r8(b, h + 12) > 64 ? 64 : r8(b, h + 12);
                sm->pan = r8(b, h + 15);
                sm->c5 = 8363.0f;
                sm->tune = rel + fine / 128.0f;
                uint32_t frames = sixteen ? len / 2 : len;
                if (data_at > b.n) frames = 0;
                if (data_at + (uint64_t)frames * (sixteen ? 2 : 1) > b.n)
                    frames = (b.n - data_at) / (sixteen ? 2 : 1);
                sm->data = alloc_pcm(frames, &budget);
                if (sm->data) {
                    sm->len = frames;
                    int acc = 0;
                    for (uint32_t k = 0; k < frames; k++) {
                        if (sixteen) acc += (int16_t)r16(b, data_at + k * 2);
                        else acc += (int8_t)b.d[data_at + k];
                        if (sixteen) sm->data[k] = (int16_t)acc;
                        else sm->data[k] = (int16_t)((int8_t)acc * 256);
                    }
                    if (sixteen) { ls /= 2; ll /= 2; }
                    set_loop(sm, ls, ls + ll > frames ? frames : ls + ll, type & 3);
                }
                data_at += len;
            }
            for (int k = 0; k < 96; k++) {
                int note = k + 12;
                in->map_smp[note] = keymap[k] < ns ? (uint16_t)(base + keymap[k] + 1) : 0;
                in->map_note[note] = (uint8_t)note;
            }
            next = data_at;
        }
        pos = next;
    }
    /* sample indexes were assigned while the list grew; keep it */
    t->smps = all;
    t->nsmp = nall;
    return true;
}

/* ---------------------------------------------------------------- IT */

static void fx_it(cell_t *c, int cmd, int par, uint8_t *last)
{
    uint8_t code = FX_NONE, p = (uint8_t)par;
    int x = par >> 4, y = par & 15;
    if (cmd == 0 || cmd > 26) return;
    if (par) last[cmd] = (uint8_t)par; else if (cmd != 1 && cmd != 2 && cmd != 3 && cmd != 20) p = par = last[cmd], x = par >> 4, y = par & 15;
    switch ('A' + cmd - 1) {
    case 'A': if (par) code = FX_SPEED; break;
    case 'B': code = FX_JUMP; break;
    case 'C': code = FX_BREAK; break;
    case 'D':
        if (y == 0xF && x) { code = FX_FINEVOLUP; p = (uint8_t)x; }
        else if (x == 0xF && y) { code = FX_FINEVOLDN; p = (uint8_t)y; }
        else code = FX_VOLSLIDE;
        break;
    case 'E':
        if (par >= 0xF0) { code = FX_FINEDN; p = (uint8_t)y; }
        else if (par >= 0xE0) { code = FX_FINEDN; p = (uint8_t)(y / 4); }
        else code = FX_SLIDEDN;
        break;
    case 'F':
        if (par >= 0xF0) { code = FX_FINEUP; p = (uint8_t)y; }
        else if (par >= 0xE0) { code = FX_FINEUP; p = (uint8_t)(y / 4); }
        else code = FX_SLIDEUP;
        break;
    case 'G': code = FX_PORTA; break;
    case 'H': code = FX_VIB; break;
    case 'J': code = FX_ARP; break;
    case 'K': code = FX_VIBVOL; break;
    case 'L': code = FX_PORTAVOL; break;
    case 'O': code = FX_OFFSET; break;
    case 'Q': code = FX_RETRIG; p = (uint8_t)y; break;
    case 'R': code = FX_TREM; break;
    case 'S':
        p = (uint8_t)y;
        switch (x) {
        case 8: code = FX_PAN; p = (uint8_t)(y * 17); break;
        case 0xB: code = FX_LOOP; break;
        case 0xC: code = FX_CUT; break;
        case 0xD: code = FX_DELAY; break;
        case 0xE: code = FX_PATDELAY; break;
        }
        break;
    case 'T': if (par >= 32) code = FX_TEMPO; break;
    case 'V': code = FX_GVOL; p = par > 128 ? 64 : (uint8_t)(par / 2); break;
    case 'X': code = FX_PAN; p = (uint8_t)par; break;
    }
    c->c2 = code;
    c->p2 = p;
}

static void vol_it(cell_t *c, int v)
{
    static const uint8_t porta[10] = { 0, 1, 4, 8, 16, 32, 64, 96, 128, 255 };
    if (v <= 64) { c->c1 = FX_SETVOL; c->p1 = (uint8_t)v; }
    else if (v <= 74) { c->c1 = FX_FINEVOLUP; c->p1 = (uint8_t)(v - 65); }
    else if (v <= 84) { c->c1 = FX_FINEVOLDN; c->p1 = (uint8_t)(v - 75); }
    else if (v <= 94) { c->c1 = FX_VOLSLIDE; c->p1 = (uint8_t)((v - 85) << 4); }
    else if (v <= 104) { c->c1 = FX_VOLSLIDE; c->p1 = (uint8_t)(v - 95); }
    else if (v >= 128 && v <= 192) { c->c1 = FX_PAN; c->p1 = (uint8_t)((v - 128) * 255 / 64); }
    else if (v >= 193 && v <= 202) { c->c1 = FX_PORTA; c->p1 = porta[v - 193]; }
    else if (v >= 203 && v <= 212) { c->c1 = FX_VIBDEPTH; c->p1 = (uint8_t)(v - 203); }
}

/* IT's compressed samples (IT 2.14 / 2.15): bit-packed deltas with a variable width. */
typedef struct { const uint8_t *p; uint32_t n, pos; uint32_t bitbuf; int bits; } bits_t;

static uint32_t get_bits(bits_t *b, int n)
{
    while (b->bits < n) {
        uint32_t v = b->pos < b->n ? b->p[b->pos] : 0;
        b->pos++;
        b->bitbuf |= v << b->bits;
        b->bits += 8;
    }
    uint32_t r = b->bitbuf & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
    b->bitbuf = n >= 32 ? 0 : b->bitbuf >> n;
    b->bits -= n;
    return r;
}

static bool it_decompress(buf_t b, uint32_t at, int16_t *out, uint32_t len, bool sixteen, bool it215)
{
    uint32_t done = 0;
    while (done < len) {
        if (!in_range(b, at, 2)) return false;
        uint32_t clen = r16(b, at);
        at += 2;
        if (!in_range(b, at, clen)) clen = b.n > at ? b.n - at : 0;
        bits_t br = { b.d + at, clen, 0, 0, 0 };
        uint32_t block = len - done < (sixteen ? 0x4000u : 0x8000u) ? len - done : (sixteen ? 0x4000u : 0x8000u);
        int width = sixteen ? 17 : 9;
        int d1 = 0, d2 = 0;
        uint32_t k = 0;
        while (k < block) {
            if (br.pos > br.n + 4) return false;
            uint32_t v = get_bits(&br, width);
            int topbit = 1 << (width - 1);
            if (width < 7) {
                if ((int)v == topbit) {
                    int nw = (int)get_bits(&br, sixteen ? 4 : 3) + 1;
                    width = nw < width ? nw : nw + 1;
                    continue;
                }
            } else if (width < (sixteen ? 17 : 9)) {
                int border = (sixteen ? (0xFFFF >> (17 - width)) - 8 : (0xFF >> (9 - width)) - 4);
                int span = sixteen ? 16 : 8;
                if ((int)v > border && (int)v <= border + span) {
                    int nw = (int)v - border;
                    width = nw < width ? nw : nw + 1;
                    continue;
                }
            } else {
                if (v & (uint32_t)topbit) {
                    width = (int)((v + 1) & 0xFF);
                    if (width < 1) width = 1;
                    continue;
                }
            }
            int val;
            if (sixteen) {
                if (width < 16) { int sh = 16 - width; val = (int16_t)(v << sh) >> sh; }
                else val = (int16_t)v;
            } else {
                if (width < 8) { int sh = 8 - width; val = (int8_t)(v << sh) >> sh; }
                else val = (int8_t)v;
            }
            d1 += val;
            d2 += d1;
            int outv = it215 ? d2 : d1;
            out[done + k++] = sixteen ? (int16_t)outv : (int16_t)((int8_t)outv * 256);
        }
        done += block;
        at += clen;
    }
    return true;
}

static bool load_it(trk_t *t, buf_t b)
{
    if (b.n < 0xC0 || memcmp(b.d, "IMPM", 4)) return false;
    t->fmt = FMT_IT;
    copy_name(t->title, sizeof(t->title), b, 4, 26);
    int ordnum = r16(b, 32), insnum = r16(b, 34), smpnum = r16(b, 36), patnum = r16(b, 38);
    int cmwt = r16(b, 42), flags = r16(b, 44);
    bool use_inst = flags & 4;
    t->linear = flags & 8;
    t->init_gvol = r8(b, 48) / 2;
    t->init_speed = r8(b, 50) ? r8(b, 50) : 6;
    t->init_tempo = r8(b, 51) >= 32 ? r8(b, 51) : 125;
    if (ordnum < 1 || ordnum > 256 || insnum > 255 || smpnum > 255 || patnum > 255) return false;
    t->orders = malloc(ordnum);
    t->norders = 0;
    for (int i = 0; i < ordnum; i++) {
        uint8_t o = r8(b, 192 + i);
        if (o == 255) break;
        if (o < 254) t->orders[t->norders++] = o;
    }
    if (!t->norders) return false;
    uint32_t base = 192 + ordnum;
    uint32_t ins_at = base, smp_at = ins_at + 4u * insnum, pat_at = smp_at + 4u * smpnum;
    t->nchan = 0;
    t->npat = patnum ? patnum : 1;
    t->pats = calloc(t->npat, sizeof(pat_t));
    /* the channel count is the highest channel any pattern uses: find it first */
    for (int p = 0; p < patnum; p++) {
        uint32_t o = r32(b, pat_at + 4u * p);
        if (!o || !in_range(b, o, 8)) continue;
        uint32_t size = r16(b, o), pos = o + 8, end = pos + size;
        if (end > b.n) end = b.n;
        uint8_t mask[64] = { 0 };
        while (pos < end) {
            uint8_t v = r8(b, pos++);
            if (!v) continue;
            int chn = (v - 1) & 63;
            if (v & 128) mask[chn] = r8(b, pos++);
            if (chn + 1 > t->nchan) t->nchan = chn + 1;
            pos += (mask[chn] & 1) + ((mask[chn] >> 1) & 1) + ((mask[chn] >> 2) & 1) + ((mask[chn] >> 3) & 1) * 2;
        }
    }
    if (t->nchan < 1) t->nchan = 1;
    int nch = t->nchan;
    for (int c = 0; c < nch && c < MAX_CHAN; c++) {
        uint8_t pn = r8(b, 64 + c);
        t->init_pan[c] = pn <= 64 ? (uint8_t)(pn * 255 / 64) : 0x80;
    }
    for (int p = 0; p < patnum; p++) {
        pat_t *pt = &t->pats[p];
        uint32_t o = r32(b, pat_at + 4u * p);
        int rows = 64;
        if (o && in_range(b, o, 8)) rows = r16(b, o + 2);
        if (rows < 1 || rows > 256) rows = 64;
        pt->rows = rows;
        pt->cells = calloc((size_t)rows * nch, sizeof(cell_t));
        if (!pt->cells) return false;
        if (!o || !in_range(b, o, 8)) continue;
        uint32_t size = r16(b, o), pos = o + 8, end = pos + size;
        if (end > b.n) end = b.n;
        uint8_t mask[64] = { 0 }, ln[64] = { 0 }, li[64] = { 0 }, lv[64] = { 0 }, lc[64] = { 0 }, lp[64] = { 0 };
        uint8_t mem[26 + 1];
        memset(mem, 0, sizeof(mem));
        int row = 0;
        while (pos < end && row < rows) {
            uint8_t v = r8(b, pos++);
            if (!v) { row++; continue; }
            int chn = (v - 1) & 63;
            if (v & 128) mask[chn] = r8(b, pos++);
            uint8_t m = mask[chn];
            if (m & 1) ln[chn] = r8(b, pos++);
            if (m & 2) li[chn] = r8(b, pos++);
            if (m & 4) lv[chn] = r8(b, pos++);
            if (m & 8) { lc[chn] = r8(b, pos); lp[chn] = r8(b, pos + 1); pos += 2; }
            if (chn >= nch) continue;
            cell_t *c = &pt->cells[row * nch + chn];
            if (m & 17) {
                if (ln[chn] == 255 || ln[chn] == 246 || (ln[chn] >= 120 && ln[chn] < 254)) c->note = 255;
                else if (ln[chn] == 254) c->note = 254;
                else c->note = (uint8_t)(ln[chn] + 1);
            }
            if (m & 34) c->inst = li[chn];
            if (m & 68) vol_it(c, lv[chn]);
            if (m & 136) fx_it(c, lc[chn], lp[chn], mem);
        }
    }
    (void)cmwt;

    /* samples */
    t->nsmp = smpnum;
    t->smps = calloc(smpnum ? smpnum : 1, sizeof(smp_t));
    uint32_t budget = MAX_SAMPLE_BYTES;
    for (int i = 0; i < smpnum; i++) {
        uint32_t o = r32(b, smp_at + 4u * i);
        smp_t *s = &t->smps[i];
        s->pan = -1;
        s->c5 = 8363.0f;
        if (!o || !in_range(b, o, 0x50) || memcmp(b.d + o, "IMPS", 4)) continue;
        uint8_t fl = r8(b, o + 0x12), cvt = r8(b, o + 0x2E);
        uint32_t len = r32(b, o + 0x30), ls = r32(b, o + 0x34), le = r32(b, o + 0x38);
        uint32_t c5 = r32(b, o + 0x3C), ptr = r32(b, o + 0x48);
        s->vol = r8(b, o + 0x13) > 64 ? 64 : r8(b, o + 0x13);
        s->gv = (uint8_t)((r8(b, o + 0x11) > 64 ? 64 : r8(b, o + 0x11)) + 1);
        s->c5 = c5 ? (float)c5 : 8363.0f;
        uint8_t dfp = r8(b, o + 0x2F);
        if (dfp & 128) s->pan = (dfp & 127) * 255 / 64;
        bool sixteen = fl & 2;
        if (!(fl & 1) || !len || !ptr || fl & 4) continue;      /* stereo samples: skipped */
        s->data = alloc_pcm(len, &budget);
        if (!s->data) continue;
        s->len = len;
        if (fl & 8) {
            it_decompress(b, ptr, s->data, len, sixteen, (cvt & 4) != 0);       /* a failed unpack keeps what it got */
        } else if (in_range(b, ptr, (uint64_t)len * (sixteen ? 2 : 1))) {
            for (uint32_t k = 0; k < len; k++) {
                if (sixteen) {
                    int v = (int16_t)r16(b, ptr + k * 2);
                    s->data[k] = (int16_t)((cvt & 1) ? v : v - 32768);
                } else {
                    int v = b.d[ptr + k];
                    s->data[k] = (int16_t)(((cvt & 1) ? (int8_t)v : v - 128) * 256);
                }
            }
        } else {
            free(s->data);
            s->data = NULL;
            s->len = 0;
            continue;
        }
        if (fl & 16) set_loop(s, ls, le > len ? len : le, (fl & 64) ? 2 : 1);
    }

    /* instruments (or, in sample mode, one per sample) */
    t->ninst = use_inst ? insnum : smpnum;
    t->insts = calloc(t->ninst + 1, sizeof(inst_t));
    if (!use_inst) {
        for (int i = 0; i < smpnum; i++)
            for (int n = 0; n < 120; n++) {
                t->insts[i + 1].map_smp[n] = (uint16_t)(i + 1);
                t->insts[i + 1].map_note[n] = (uint8_t)n;
            }
        return true;
    }
    for (int i = 0; i < insnum; i++) {
        uint32_t o = r32(b, ins_at + 4u * i);
        inst_t *in = &t->insts[i + 1];
        if (!o || !in_range(b, o, 0x130) || memcmp(b.d + o, "IMPI", 4)) continue;
        in->fadeout = (uint16_t)(r16(b, o + 0x14) * 64);
        in->gv = (uint8_t)((r8(b, o + 0x18) > 128 ? 128 : r8(b, o + 0x18)) + 1);
        for (int n = 0; n < 120; n++) {
            in->map_note[n] = r8(b, o + 0x40 + 2 * n);
            uint8_t sn = r8(b, o + 0x41 + 2 * n);
            in->map_smp[n] = sn && sn <= smpnum ? sn : 0;
            if (in->map_note[n] >= 120) in->map_note[n] = (uint8_t)n;
        }
        uint8_t ef = r8(b, o + 0x130), cnt = r8(b, o + 0x131);
        if (cnt > 25) cnt = 25;
        env_t *e = &in->env;
        e->on = (ef & 1) && cnt >= 2;
        e->loop = ef & 2;
        e->sus = ef & 4;
        e->n = cnt;
        e->loop_beg = r8(b, o + 0x132);
        e->loop_end = r8(b, o + 0x133);
        e->sus_beg = r8(b, o + 0x134);
        e->sus_end = r8(b, o + 0x135);
        for (int k = 0; k < cnt; k++) {
            int v = (int8_t)r8(b, o + 0x136 + 3 * k);
            e->val[k] = (uint8_t)(v < 0 ? 0 : v > 64 ? 64 : v);
            e->tick[k] = r16(b, o + 0x137 + 3 * k);
        }
    }
    return true;
}

/* ---------------------------------------------------------------- engine */

static bool vis_test_set(trk_t *t, int order, int row)
{
    uint32_t bit = (uint32_t)order * MAX_ROWS + (uint32_t)row;
    bool was = t->visited[bit >> 3] & (1 << (bit & 7));
    t->visited[bit >> 3] |= (uint8_t)(1 << (bit & 7));
    return was;
}

static void vis_clear_order(trk_t *t, int order)
{
    memset(t->visited + (size_t)order * MAX_ROWS / 8, 0, MAX_ROWS / 8);
}

static const pat_t *cur_pat(const trk_t *t)
{
    if (t->order >= t->norders) return NULL;
    int p = t->orders[t->order];
    return p < t->npat && t->pats[p].cells ? &t->pats[p] : NULL;
}

static void state_reset(trk_t *t)
{
    memset(t->ch, 0, sizeof(t->ch));
    for (int c = 0; c < t->nchan && c < MAX_CHAN; c++) {
        t->ch[c].chan_pan = t->init_pan[c];
        t->ch[c].pan = t->init_pan[c];
        t->ch[c].cut_at = t->ch[c].delay_at = t->ch[c].key_at = -1;
    }
    t->speed = t->init_speed;
    t->tempo = t->init_tempo;
    t->gvol = t->init_gvol;
    t->order = 0;
    t->row = 0;
    t->tick = 0;
    t->patdelay = 0;
    t->delaying = 0;
    t->nxt_order = t->nxt_row = -1;
    t->loop_back = false;
    t->ended = false;
    t->tick_frac = 0;
    t->tick_left = 0;
    memset(t->visited, 0, (size_t)t->norders * MAX_ROWS / 8);
    vis_test_set(t, 0, 0);
}

static const int8_t SINE[32] = { 0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                                 255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24 };

static int wave(int pos)
{
    int v = SINE[pos & 31];
    return (pos & 32) ? -v : v;                         /* -255 .. 255 */
}

static void slide(trk_t *t, chan_t *c, float units, bool up)
{
    if (t->linear || t->fmt == FMT_IT || t->fmt == FMT_XM) {
        c->pitch += (up ? 1 : -1) * units / 16.0f;
    } else {
        float p = pitch_to_period(c->pitch) + (up ? -units : units);
        if (p < 56) p = 56;
        if (p > 1712) p = 1712;
        c->pitch = period_to_pitch(p);
    }
    if (c->pitch < 0) c->pitch = 0;
    if (c->pitch > 130) c->pitch = 130;
}

static void porta_toward(trk_t *t, chan_t *c, float units)
{
    if (c->pitch == c->target) return;
    bool up = c->target > c->pitch;
    slide(t, c, units, up);
    if ((up && c->pitch > c->target) || (!up && c->pitch < c->target)) c->pitch = c->target;
}

static void restart(chan_t *c)
{
    c->pos = 0;
    c->dir = 1;
    c->playing = c->smp && c->smp->data;
    c->key_on = true;
    c->fade = 65536;
    c->env_tick = 0;
    c->offs = 0;
}

static void key_off(trk_t *t, chan_t *c)
{
    c->key_on = false;
    if (c->inst && c->inst->env.on) return;
    if (t->fmt == FMT_XM || (t->fmt == FMT_IT && !(c->inst && c->inst->fadeout))) {
        if (t->fmt == FMT_XM && (!c->inst || !c->inst->fadeout)) c->vol = 0;
        else if (t->fmt == FMT_IT) c->vol = 0;
    }
}

static bool has_porta(const cell_t *cell)
{
    return cell->c2 == FX_PORTA || cell->c2 == FX_PORTAVOL || cell->c1 == FX_PORTA;
}

static void trigger(trk_t *t, chan_t *c, const cell_t *cell)
{
    bool new_inst = cell->inst != 0;
    int note = cell->note;
    if (new_inst) {
        c->inst_no = cell->inst;
        c->inst = cell->inst <= t->ninst ? &t->insts[cell->inst] : NULL;
    }
    if (note == 254) { c->vol = 0; c->playing = false; c->key_on = false; return; }
    if (note == 255) { key_off(t, c); return; }
    if (note) {
        int pitch = note - 1;
        const smp_t *s = NULL;
        if (c->inst) {
            int idx = pitch < 120 ? pitch : 119;
            int si = c->inst->map_smp[idx];
            if (si && si <= t->nsmp) s = &t->smps[si - 1];
            if (t->fmt == FMT_IT) pitch = c->inst->map_note[idx];
        }
        if (!s) {
            if (has_porta(cell) && c->smp) s = c->smp;
            else { c->playing = false; return; }
        }
        if (has_porta(cell) && c->playing) {
            c->target = (float)pitch;
            if (new_inst) { c->vol = s->vol; }
            return;
        }
        c->smp = s;
        c->pitch = c->target = (float)pitch;
        restart(c);
        c->vol = s->vol;
        if (s->pan >= 0 && t->fmt != FMT_MOD && t->fmt != FMT_S3M) c->pan = s->pan;
        c->vib_pos = 0;
        c->trem_pos = 0;
        if (cell->c2 == FX_OFFSET) {
            uint8_t p = cell->p2 ? cell->p2 : c->mem[FX_OFFSET];
            int64_t at = (int64_t)p * 256;
            if (at >= (int64_t)s->len) at = s->len ? s->len - 1 : 0;
            c->pos = at << 16;
        }
    } else if (new_inst && c->inst) {
        /* an instrument number with no note: reset the volume (and pan) of what is playing */
        int idx = 60;
        int si = c->inst->map_smp[idx];
        const smp_t *s = si && si <= t->nsmp ? &t->smps[si - 1] : c->smp;
        if (s) {
            c->vol = s->vol;
            if (s->pan >= 0 && t->fmt != FMT_MOD && t->fmt != FMT_S3M) c->pan = s->pan;
        }
    }
}

static void fx_row(trk_t *t, chan_t *c, int code, int p)
{
    if (code == FX_NONE) return;
    if (p) c->mem[code] = (uint8_t)p;
    switch (code) {
    case FX_SETVOL: c->vol = p > 64 ? 64 : p; break;
    case FX_PAN: c->pan = p; break;
    case FX_SPEED: if (p) t->speed = p; break;
    case FX_TEMPO: if (p >= 32) t->tempo = p; break;
    case FX_JUMP: t->nxt_order = p; if (t->nxt_row < 0) t->nxt_row = 0; break;
    case FX_BREAK: t->nxt_row = p; if (t->nxt_order < 0) t->nxt_order = t->order + 1; break;
    case FX_GVOL: t->gvol = p > 64 ? 64 : p; break;
    case FX_FINEUP: slide(t, c, (float)(p ? p : c->mem[code]), true); break;
    case FX_FINEDN: slide(t, c, (float)(p ? p : c->mem[code]), false); break;
    case FX_FINEVOLUP: c->vol += p ? p : c->mem[code]; if (c->vol > 64) c->vol = 64; break;
    case FX_FINEVOLDN: c->vol -= p ? p : c->mem[code]; if (c->vol < 0) c->vol = 0; break;
    case FX_CUT: c->cut_at = p; break;
    case FX_KEYOFF: if (p == 0) key_off(t, c); else c->key_at = p; break;
    case FX_PATDELAY: if (!t->delaying) t->patdelay = p; break;
    case FX_RETRIG: c->retrig = p; break;
    case FX_VIB:
        if (p >> 4) c->vib_spd = (uint8_t)(p >> 4);
        if (p & 15) c->vib_dep = (uint8_t)(p & 15);
        break;
    case FX_VIBDEPTH: if (p) c->vib_dep = (uint8_t)p; break;
    case FX_TREM:
        if (p >> 4) c->trem_spd = (uint8_t)(p >> 4);
        if (p & 15) c->trem_dep = (uint8_t)(p & 15);
        break;
    case FX_PORTA: if (p) c->porta = (uint8_t)p; break;
    case FX_VIBVOL: break;
    case FX_LOOP:
        if (p == 0) c->loop_row = t->row;
        else {
            if (c->loop_cnt == 0) c->loop_cnt = p; else c->loop_cnt--;
            if (c->loop_cnt > 0) { t->nxt_row = c->loop_row; t->nxt_order = t->order; t->loop_back = true; }
        }
        break;
    default: break;
    }
}

static void vol_slide(chan_t *c, int p)
{
    int up = p >> 4, down = p & 15;
    if (up && !down) c->vol += up;
    else c->vol -= down;
    if (c->vol < 0) c->vol = 0;
    if (c->vol > 64) c->vol = 64;
}

static void fx_tick(trk_t *t, chan_t *c, int code, int p)
{
    if (code == FX_NONE) return;
    if (!p && code < FX_COUNT) p = c->mem[code];
    switch (code) {
    case FX_SLIDEUP: slide(t, c, (float)p, true); break;
    case FX_SLIDEDN: slide(t, c, (float)p, false); break;
    case FX_PORTA: porta_toward(t, c, (float)c->porta); break;
    case FX_PORTAVOL: porta_toward(t, c, (float)c->porta); vol_slide(c, p); break;
    case FX_VIB: case FX_VIBVOL: case FX_VIBDEPTH: break;      /* handled below, every tick */
    case FX_VOLSLIDE: vol_slide(c, p); break;
    case FX_ARP: {
        int step = t->tick % 3;
        c->offs = step == 0 ? 0 : step == 1 ? (float)(p >> 4) : (float)(p & 15);
        break;
    }
    case FX_RETRIG: break;
    default: break;
    }
    if (code == FX_VIBVOL) vol_slide(c, p);
}

static void channel_tick(trk_t *t, chan_t *c, const cell_t *cell, bool row_start)
{
    int codes[2] = { cell ? cell->c1 : 0, cell ? cell->c2 : 0 };
    int pars[2] = { cell ? cell->p1 : 0, cell ? cell->p2 : 0 };
    if (!row_start) {
        c->offs = 0;
        for (int i = 0; i < 2; i++) {
            int code = codes[i];
            int p = pars[i];
            /* vibrato and tremolo run every tick, from the remembered speed and depth */
            fx_tick(t, c, code, p);
            if (code == FX_VIB || code == FX_VIBVOL) {
                if (code == FX_VIB && p) fx_row(t, c, FX_VIB, p);
            }
        }
        for (int i = 0; i < 2; i++) {
            int code = codes[i], p = pars[i];
            if (code == FX_VIB || code == FX_VIBVOL || code == FX_VIBDEPTH) {
                c->vib_pos = (uint8_t)(c->vib_pos + c->vib_spd * 4);
                (void)p;
            }
            if (code == FX_TREM) c->trem_pos = (uint8_t)(c->trem_pos + c->trem_spd * 4);
        }
        if (c->retrig > 0 && t->tick % c->retrig == 0 && c->smp) restart(c);
        if (c->cut_at >= 0 && t->tick == c->cut_at) { c->vol = 0; c->cut_at = -1; }
        if (c->key_at >= 0 && t->tick == c->key_at) { key_off(t, c); c->key_at = -1; }
        if (c->delay_at >= 0 && t->tick == c->delay_at) {
            cell_t pend = c->pending;
            c->delay_at = -1;
            trigger(t, c, &pend);
            fx_row(t, c, pend.c1, pend.p1);
        }
    }
}

static void apply_row(trk_t *t)
{
    const pat_t *pt = cur_pat(t);
    for (int ci = 0; ci < t->nchan && ci < MAX_CHAN; ci++) {
        chan_t *c = &t->ch[ci];
        c->cut_at = c->delay_at = c->key_at = -1;
        c->retrig = 0;
        c->offs = 0;
        if (!pt || t->row >= pt->rows) continue;
        const cell_t *cell = &pt->cells[t->row * t->nchan + ci];
        if (t->delaying) {
            /* a pattern delay repeats the row's effects, not its notes */
            continue;
        }
        if (cell->c2 == FX_DELAY && cell->p2 > 0) {
            c->pending = *cell;
            c->pending.c2 = FX_NONE;
            c->delay_at = cell->p2;
            c->pending.c1 = cell->c1;
            continue;
        }
        trigger(t, c, cell);
        fx_row(t, c, cell->c1, cell->p1);
        fx_row(t, c, cell->c2, cell->p2);
        if (cell->c2 == FX_DELAY && cell->p2 == 0) { /* SD0: plays at once */ }
    }
}

static void update_channel(trk_t *t, chan_t *c, int ci)
{
    (void)ci;
    if (!c->playing || !c->smp) {
        c->dstl = c->dstr = 0;
        return;
    }
    const inst_t *in = c->inst;
    int env = 64;
    if (in && in->env.on) {
        const env_t *e = &in->env;
        int n = e->n, k = 0;
        while (k + 1 < n && c->env_tick >= e->tick[k + 1]) k++;
        if (k + 1 < n) {
            int t0 = e->tick[k], t1 = e->tick[k + 1];
            env = t1 > t0 ? e->val[k] + (e->val[k + 1] - e->val[k]) * (c->env_tick - t0) / (t1 - t0) : e->val[k];
        } else env = e->val[n - 1];
        /* advance */
        int next = c->env_tick + 1;
        if (e->sus && c->key_on && k >= e->sus_end && next > e->tick[e->sus_end]) {
            next = e->tick[e->sus_beg];
        } else if (e->loop && (!c->key_on || !e->sus) && next > e->tick[e->loop_end] && e->loop_end < n) {
            next = e->tick[e->loop_beg];
        } else if (e->sus && c->key_on && next > e->tick[e->sus_end]) {
            next = e->tick[e->sus_beg];
        }
        c->env_tick = next;
    }
    if (!c->key_on && in) {
        if (in->fadeout) c->fade -= in->fadeout;
        else if (in->env.on && c->env_tick >= in->env.tick[in->env.n - 1]) c->fade = 0;
        if (c->fade <= 0) { c->fade = 0; c->playing = false; }
    }
    float semis = c->pitch + c->offs + c->smp->tune;
    if (c->vib_dep) {
        float depth = (t->fmt == FMT_IT ? 1.0f / 16.0f : 1.0f / 12.0f) * c->vib_dep;
        semis += wave(c->vib_pos) / 255.0f * depth;
    }
    c->freq = semi_to_hz(c->smp->c5, semis - 60.0f);
    float vol = c->vol;
    if (c->trem_dep) {
        vol += wave(c->trem_pos) / 255.0f * c->trem_dep;
        if (vol < 0) vol = 0;
        if (vol > 64) vol = 64;
    }
    float gain = vol / 64.0f * env / 64.0f * (c->fade / 65536.0f) * t->gvol / 64.0f;
    if (c->smp->gv) gain *= (c->smp->gv - 1) / 64.0f;
    if (in && in->gv) gain *= (in->gv - 1) / 128.0f;
    float p = c->pan / 255.0f;
    c->dstl = (int)(gain * sqrtf(1.0f - p) * 4096.0f);
    c->dstr = (int)(gain * sqrtf(p) * 4096.0f);
}

/* Advance the song by one tick; returns false at the end. */
static bool advance_tick(trk_t *t)
{
    if (t->ended) return false;
    const pat_t *pt = cur_pat(t);
    if (!pt) { t->ended = true; return false; }
    bool row_start = t->tick == 0;
    if (row_start) {
        t->nxt_order = t->nxt_row = -1;
        t->loop_back = false;
        apply_row(t);
    }
    for (int ci = 0; ci < t->nchan && ci < MAX_CHAN; ci++) {
        const cell_t *cell = t->row < pt->rows ? &pt->cells[t->row * t->nchan + ci] : NULL;
        channel_tick(t, &t->ch[ci], row_start || t->delaying ? NULL : cell, row_start || t->delaying);
        update_channel(t, &t->ch[ci], ci);
    }
    t->tick++;
    if (t->tick >= t->speed + (t->delaying ? 0 : 0)) {
        t->tick = 0;
        if (t->patdelay > 0) {
            t->patdelay--;
            t->delaying = 1;
            return true;
        }
        t->delaying = 0;
        if (t->loop_back) {
            t->row = t->nxt_row;
            vis_clear_order(t, t->order);
        } else if (t->nxt_order >= 0 || t->nxt_row >= 0 || t->row + 1 >= pt->rows) {
            t->order = t->nxt_order >= 0 ? t->nxt_order : t->order + 1;
            t->row = t->nxt_row >= 0 ? t->nxt_row : 0;
            if (t->order >= t->norders) { t->ended = true; return true; }
            const pat_t *np = cur_pat(t);
            if (!np) { t->ended = true; return true; }
            if (t->row >= np->rows) t->row = 0;
        } else
            t->row++;
        if (vis_test_set(t, t->order, t->row)) t->ended = true;
    }
    return true;
}

static int ticks_to_frames(trk_t *t)
{
    float spt = t->rate * 2.5f / (t->tempo ? t->tempo : 125) + t->tick_frac;
    int n = (int)spt;
    t->tick_frac = spt - n;
    return n > 0 ? n : 1;
}

/* ---------------------------------------------------------------- public API */

bool trk_probe(const uint8_t *h, uint32_t n, const char *name)
{
    if (n >= 17 && !memcmp(h, "Extended Module: ", 17)) return true;
    if (n >= 4 && !memcmp(h, "IMPM", 4)) return true;
    if (n >= 0x30 && !memcmp(h + 0x2C, "SCRM", 4)) return true;
    if (name) {
        size_t l = strlen(name);
        if (l >= 4 && !strcasecmp(name + l - 4, ".mod")) return true;
    }
    return false;
}

trk_t *trk_open(const uint8_t *data, uint32_t len, int rate)
{
    buf_t b = { data, len };
    trk_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->rate = rate > 0 ? rate : 32000;
    bool ok = false;
    if (len >= 17 && !memcmp(data, "Extended Module: ", 17)) ok = load_xm(t, b);
    else if (len >= 4 && !memcmp(data, "IMPM", 4)) ok = load_it(t, b);
    else if (len >= 0x30 && !memcmp(data + 0x2C, "SCRM", 4)) ok = load_s3m(t, b);
    else ok = load_mod(t, b);
    if (!ok || t->nchan < 1 || t->norders < 1) { free_module(t); return NULL; }
    if (t->nchan > MAX_CHAN) t->nchan = MAX_CHAN;
    t->visited = calloc(1, (size_t)t->norders * MAX_ROWS / 8 + 1);
    if (!t->visited) { free_module(t); return NULL; }
    t->master = (int)(4096.0f * 0.5f * fminf(1.0f, sqrtf(4.0f / (t->nchan < 4 ? 4 : t->nchan))));

    /* the length: run the sequencer without mixing */
    state_reset(t);
    uint64_t frames = 0;
    while (!t->ended && frames < (uint64_t)t->rate * MAX_SECONDS) {
        int n = ticks_to_frames(t);
        if (!advance_tick(t)) break;
        frames += n;
    }
    t->total = frames;
    state_reset(t);
    return t;
}

const char *trk_format(const trk_t *t)
{
    static const char *const names[] = { "MOD", "S3M", "XM", "IT" };
    return names[t->fmt];
}
const char *trk_title(const trk_t *t) { return t->title; }
int trk_channels(const trk_t *t) { return t->nchan; }
uint64_t trk_frames(const trk_t *t) { return t->total; }
void trk_rewind(trk_t *t) { state_reset(t); }
void trk_close(trk_t *t) { free_module(t); }

static inline int clip16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

static void mix_chunk(trk_t *t, int32_t *mix, int frames)
{
    for (int ci = 0; ci < t->nchan && ci < MAX_CHAN; ci++) {
        chan_t *c = &t->ch[ci];
        if (!c->playing || !c->smp || !c->smp->data) {
            c->curl = c->curr = 0;
            continue;
        }
        const smp_t *s = c->smp;
        const int16_t *d = s->data;
        int64_t step = (int64_t)(c->freq / t->rate * 65536.0f);
        if (step < 1) step = 1;
        uint32_t end = s->loop ? s->loop_end : s->len;
        int64_t pos = c->pos;
        int curl = c->curl, curr = c->curr;
        for (int i = 0; i < frames; i++) {
            if (curl != c->dstl) curl += curl < c->dstl ? (c->dstl - curl > 16 ? 16 : c->dstl - curl) : -(curl - c->dstl > 16 ? 16 : curl - c->dstl);
            if (curr != c->dstr) curr += curr < c->dstr ? (c->dstr - curr > 16 ? 16 : c->dstr - curr) : -(curr - c->dstr > 16 ? 16 : curr - c->dstr);
            uint32_t idx = (uint32_t)(pos >> 16);
            if (idx >= end || pos < 0) { c->playing = false; break; }
            uint32_t nx = idx + 1;
            if (nx >= end) nx = s->loop ? s->loop_start : idx;
            int a = d[idx], bb = d[nx];
            int sm = a + (((bb - a) * (int)((pos & 0xFFFF) >> 1)) >> 15);
            mix[i * 2] += (sm * curl) >> 12;
            mix[i * 2 + 1] += (sm * curr) >> 12;
            pos += c->dir * step;
            if (c->dir > 0) {
                if ((uint64_t)(pos >> 16) >= end) {
                    if (s->loop == 1) {
                        uint32_t span = s->loop_end - s->loop_start;
                        uint64_t over = (uint64_t)pos - ((uint64_t)end << 16);
                        pos = ((int64_t)s->loop_start << 16) + (int64_t)(over % ((uint64_t)span << 16));
                    } else if (s->loop == 2) {
                        pos = ((int64_t)end << 16) * 2 - pos - 65536;
                        c->dir = -1;
                        if (pos < ((int64_t)s->loop_start << 16)) pos = (int64_t)s->loop_start << 16;
                    } else { c->playing = false; break; }
                }
            } else if (pos < ((int64_t)s->loop_start << 16)) {
                pos = ((int64_t)s->loop_start << 16) * 2 - pos;
                c->dir = 1;
                if ((uint64_t)(pos >> 16) >= end) pos = (int64_t)s->loop_start << 16;
            }
        }
        c->pos = pos;
        c->curl = curl;
        c->curr = curr;
    }
}

uint32_t trk_read(trk_t *t, int16_t *out, uint32_t frames)
{
    uint32_t done = 0;
    int32_t mix[512 * 2];
    while (done < frames) {
        if (t->tick_left <= 0) {
            if (t->ended) break;
            int n = ticks_to_frames(t);
            if (!advance_tick(t) && t->ended) break;
            t->tick_left = n;
        }
        uint32_t take = frames - done < (uint32_t)t->tick_left ? frames - done : (uint32_t)t->tick_left;
        if (take > 512) take = 512;
        memset(mix, 0, take * 2 * sizeof(int32_t));
        mix_chunk(t, mix, (int)take);
        for (uint32_t i = 0; i < take * 2; i++) out[done * 2 + i] = (int16_t)clip16((mix[i] * t->master) >> 12);
        done += take;
        t->tick_left -= (int)take;
    }
    return done;
}

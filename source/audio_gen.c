/*
 * The audio player's common formats, decoded to 16-bit PCM while playing (see audio.c):
 *   WAV / AIFF / AIFC   dr_wav: PCM, float, A-law, mu-law, IMA and MS ADPCM
 *   MP3 (and MP2)       dr_mp3
 *   FLAC                dr_flac
 *   Ogg Vorbis          Tremor (devkitPro's libvorbisidec)
 *   M4A (AAC-LC)        FAAD2 (devkitPro's ppc-faad2), when it is installed; the Wii Photo Channel's music
 *   MOD / S3M / XM / IT tracker modules, mixed by tracker.c
 * dr_wav, dr_mp3 and dr_flac are David Reid's dr_libs (public domain / MIT-0, source/drlibs).
 * Files are read through a buffer: vfs reads want 32-byte aligned memory, the decoders don't
 * promise it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#include "drlibs/dr_wav.h"
#include "drlibs/dr_mp3.h"
#include "drlibs/dr_flac.h"
#include <tremor/ivorbisfile.h>
#if __has_include(<neaacdec.h>)
#define HAVE_AAC 1
#include <neaacdec.h>
#include "qt.h"
#endif

#include "audio_gen.h"
#include "ui.h"
#include "tracker.h"
#include "vfs.h"
#include "archive.h"

#define BUF 0x8000

typedef struct {
    vfs_file f;
    u8 *buf;                    /* 32-byte aligned */
    u64 buf_at;                 /* file offset of buf[0] */
    u32 buf_len;
    u64 pos;
} bio_t;

static bool bio_open(bio_t *b, const char *path)
{
    memset(b, 0, sizeof(*b));
    b->buf = memalign(32, BUF);
    if (!b->buf) return false;
    if (!vfs_open_read(&b->f, path)) {
        free(b->buf);
        b->buf = NULL;
        return false;
    }
    return true;
}

static void bio_close(bio_t *b)
{
    if (b->buf) {
        vfs_close(&b->f);
        free(b->buf);
    }
    b->buf = NULL;
}

static size_t bio_read(bio_t *b, void *out, size_t n)
{
    size_t got = 0;
    while (got < n && b->pos < b->f.size) {
        if (b->pos < b->buf_at || b->pos >= b->buf_at + b->buf_len) {
            u64 at = b->pos & ~(u64)31;
            u32 want = b->f.size - at < BUF ? (u32)(b->f.size - at) : BUF;
            if (!vfs_seek(&b->f, at)) break;
            u32 have = 0;
            while (have < want) {
                s32 r = vfs_read(&b->f, b->buf + have, want - have);
                if (r <= 0) break;
                have += (u32)r;
            }
            b->buf_at = at;
            b->buf_len = have;
            if (b->pos >= at + have) break;
        }
        u32 in = (u32)(b->pos - b->buf_at), k = b->buf_len - in;
        if (k > n - got) k = (u32)(n - got);
        memcpy((u8 *)out + got, b->buf + in, k);
        got += k;
        b->pos += k;
    }
    return got;
}

static bool bio_seek(bio_t *b, s64 off, int whence)
{
    s64 t = whence == SEEK_SET ? off :
            whence == SEEK_CUR ? (s64)b->pos + off : (s64)b->f.size + off;
    if (t < 0 || (u64)t > b->f.size) return false;
    b->pos = (u64)t;
    return true;
}

/* ---------------------------------------------------------------- decoders */

enum { K_WAV, K_MP3, K_FLAC, K_OGG, K_TRK, K_M4A };

#define TRK_RATE 32000
#define TRK_MAX_FILE (32u << 20)

#ifdef HAVE_AAC
typedef struct {
    NeAACDecHandle dec;
    spans pk;                               /* the AAC packets, in order */
    int next;                               /* the packet to decode next */
    u32 per;                                /* frames a packet decodes to */
    u64 skip;                               /* frames to drop (the lead-in after a seek) */
    s16 *pcm;
    u32 n, pos, ch;                         /* decoded frames, the next one to hand out */
    u8 *buf;
    u32 cap;
} aac_t;
#endif

struct gen {
    bio_t io;
    int kind;
    union {
        drwav wav;
        drmp3 mp3;
        drflac *flac;
        OggVorbis_File ogg;
        trk_t *trk;
#ifdef HAVE_AAC
        aac_t *aac;
#endif
    } d;
    gen_info info;
    drmp3_seek_point *mp3_pts;              /* the MP3 seek table, made at the first seek */
    bool mp3_tried;
};

static size_t dr_read(void *u, void *out, size_t n) { return bio_read(u, out, n); }
static int whence_of(int origin)
{
    return origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
}
static drwav_bool32 wav_seek(void *u, int off, drwav_seek_origin o)
{
    return bio_seek(u, off, whence_of(o));
}
static drwav_bool32 wav_tell(void *u, drwav_int64 *c)
{
    *c = (drwav_int64)((bio_t *)u)->pos;
    return DRWAV_TRUE;
}
static drmp3_bool32 mp3_seek(void *u, int off, drmp3_seek_origin o)
{
    return bio_seek(u, off, whence_of(o));
}
static drmp3_bool32 mp3_tell(void *u, drmp3_int64 *c)
{
    *c = (drmp3_int64)((bio_t *)u)->pos;
    return DRMP3_TRUE;
}
static drflac_bool32 flac_seek(void *u, int off, drflac_seek_origin o)
{
    return bio_seek(u, off, whence_of(o));
}
static drflac_bool32 flac_tell(void *u, drflac_int64 *c)
{
    *c = (drflac_int64)((bio_t *)u)->pos;
    return DRFLAC_TRUE;
}
static size_t ogg_read(void *p, size_t sz, size_t nm, void *u)
{
    return sz ? bio_read(u, p, sz * nm) / sz : 0;
}
static int ogg_seek(void *u, ogg_int64_t off, int wh) { return bio_seek(u, off, wh) ? 0 : -1; }
static int ogg_close(void *u)
{
    (void)u;
    return 0;
}
static long ogg_tell(void *u) { return (long)((bio_t *)u)->pos; }

#ifdef HAVE_AAC
/* The AudioSpecificConfig inside an 'mp4a' sample entry's esds: ES_Descriptor (3) > DecoderConfig (4) >
 * DecoderSpecificInfo (5), each tag followed by a 1-4 byte length. */
static const u8 *esds_asc(const u8 *e, u32 n, u32 *len)
{
    u32 p = 0;
    while (p + 4 <= n && memcmp(e + p, "esds", 4)) p++;
    p += 8;                                 /* "esds", then version and flags */
    for (int depth = 0; p + 2 <= n && depth < 3; depth++) {
        u8 tag = e[p++];
        u32 l = 0;
        for (int i = 0; i < 4 && p < n; i++) {
            u8 b = e[p++];
            l = l << 7 | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        if (tag == 5) {
            if (p + l > n) return NULL;
            *len = l;
            return e + p;
        }
        if (tag == 3) {                     /* ES_ID, flags, then optional dependency / URL / OCR fields */
            if (p + 3 > n) return NULL;
            u8 fl = e[p + 2];
            p += 3 + (fl & 0x80 ? 2 : 0) + (fl & 0x20 ? 2 : 0);
            if (fl & 0x40) p += 1 + e[p];
        } else if (tag == 4) p += 13;
        else return NULL;
    }
    return NULL;
}

static void aac_close(aac_t *a)
{
    if (!a) return;
    if (a->dec) NeAACDecClose(a->dec);
    free(a->pk.v);
    free(a->pcm);
    free(a->buf);
    free(a);
}

/* Decode the next good packet into a->pcm; false at the end. */
static bool aac_fill(gen_t *g)
{
    aac_t *a = g->d.aac;
    while (a->next < a->pk.n) {
        span sp = a->pk.v[a->next++];
        if (sp.sz > a->cap) {
            u8 *nb = realloc(a->buf, sp.sz);
            if (!nb) return false;
            a->buf = nb;
            a->cap = sp.sz;
        }
        if (!bio_seek(&g->io, sp.off, SEEK_SET) || bio_read(&g->io, a->buf, sp.sz) != sp.sz) return false;
        NeAACDecFrameInfo fi;
        s16 *o = NeAACDecDecode(a->dec, &fi, a->buf, sp.sz);
        if (fi.error || !o || fi.channels != a->ch || fi.samples > 2048 * a->ch) continue;   /* a bad packet: skip it */
        a->n = fi.samples / a->ch;
        memcpy(a->pcm, o, fi.samples * sizeof(s16));
        a->pos = a->skip < a->n ? (u32)a->skip : a->n;
        a->skip -= a->pos;
        if (a->pos < a->n) return true;
    }
    return false;
}

/* Open the first AAC track of an M4A. shortcut: AAC-LC (what the Photo Channel plays); HE-AAC would
 * play at half the rate, upgrade by reading the real rate out of the first frame. */
static aac_t *aac_open(gen_t *g)
{
    bio_t *io = &g->io;
    u8 h[16];
    u8 *moov = NULL;
    u32 mn = 0;
    u64 pos = 0;
    while (pos + 8 <= io->f.size && !moov) {
        bio_seek(io, pos, SEEK_SET);
        if (bio_read(io, h, 8) != 8) break;
        u64 sz = be32(h);
        if (sz == 1) {
            if (bio_read(io, h + 8, 8) != 8) break;
            sz = (u64)be32(h + 8) << 32 | be32(h + 12);
        } else if (!sz) sz = io->f.size - pos;
        if (sz < 8 || pos + sz > io->f.size) break;
        if (!memcmp(h + 4, "moov", 4)) {
            u32 hdr = be32(h) == 1 ? 16 : 8;
            if (sz - hdr > (16u << 20)) break;
            mn = (u32)(sz - hdr);
            moov = malloc(mn);
            bio_seek(io, pos + hdr, SEEK_SET);
            if (!moov || bio_read(io, moov, mn) != mn) {
                free(moov);
                moov = NULL;
            }
            break;
        }
        pos += sz;
    }
    if (!moov) return NULL;

    aac_t *a = calloc(1, sizeof(*a));
    bool ok = false;
    for (u32 o = 0; a && !ok && o + 8 <= mn;) {
        u32 sz = be32(moov + o);
        if (sz < 8 || sz > mn - o) break;
        if (!memcmp(moov + o + 4, "trak", 4)) {
            blob mdia = atom(moov + o + 8, sz - 8, "mdia");
            blob minf = atom(mdia.p, mdia.n, "minf"), hd = atom(mdia.p, mdia.n, "hdlr");
            blob stbl = atom(minf.p, minf.n, "stbl");
            blob stsd = atom(stbl.p, stbl.n, "stsd"), stts = atom(stbl.p, stbl.n, "stts");
            uint32_t total, fixed;
            if (hd.p && hd.n >= 12 && !memcmp(hd.p + 8, "soun", 4) && stsd.p && stsd.n >= 8 + 36 &&
                !memcmp(stsd.p + 12, "mp4a", 4) && stts.p && stts.n >= 16 &&
                track_spans(stbl, &a->pk, &total, &fixed) && !fixed && a->pk.n) {
                u32 asc_len = 0;
                const u8 *asc = esds_asc(stsd.p + 8 + 8, stsd.n - 16, &asc_len);
                unsigned long rate = 0;
                u8 ch = 0;
                a->dec = NeAACDecOpen();
                if (asc && a->dec) {
                    NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(a->dec);
                    c->outputFormat = FAAD_FMT_16BIT;
                    c->downMatrix = 1;
                    NeAACDecSetConfiguration(a->dec, c);
                    if (NeAACDecInit2(a->dec, (u8 *)asc, asc_len, &rate, &ch) == 0 && ch >= 1 && ch <= 2) {
                        g->info.channels = ch;
                        g->info.rate = (int)rate;
                        g->info.total = 0;
                        for (u32 i = 0; i < be32(stts.p + 4) && 16 + i * 8 <= stts.n; i++)
                            g->info.total += (u64)be32(stts.p + 8 + i * 8) * be32(stts.p + 12 + i * 8);
                        ok = true;
                    }
                }
            }
        }
        o += sz;
    }
    free(moov);
    if (ok) {
        a->ch = g->info.channels;
        a->pcm = malloc(2048 * a->ch * sizeof(s16));
        g->d.aac = a;
        ok = a->pcm && aac_fill(g);         /* the first packet says how long a packet is */
        if (ok) {
            a->per = a->n;
            a->next = 0;
            a->n = a->pos = 0;
            NeAACDecPostSeekReset(a->dec, 0);
        }
        g->d.aac = NULL;
    }
    if (!ok) {
        aac_close(a);
        return NULL;
    }
    return a;
}
#endif

static int kind_of(const u8 *h, u32 n, const char *name)
{
    if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return K_WAV;
    if (n >= 12 && (!memcmp(h, "RF64", 4) || !memcmp(h, "riff", 4))) return K_WAV;
    if (n >= 12 && !memcmp(h, "FORM", 4) &&
        (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4)))
        return K_WAV;
    if (n >= 4 && !memcmp(h, "fLaC", 4)) return K_FLAC;
    if (n >= 4 && !memcmp(h, "OggS", 4)) {
        /* Ogg holds Vorbis, FLAC or Opus: the first packet says which */
        if (n >= 0x23 && !memcmp(h + 0x1D, "FLAC", 4)) return K_FLAC;
        if (n >= 0x23 && !memcmp(h + 0x1C, "OpusHead", 8)) return -1;
        return K_OGG;
    }
    if (n >= 12 && !memcmp(h + 4, "ftyp", 4) && (!memcmp(h + 8, "M4A ", 4) || !memcmp(h + 8, "M4B ", 4)))
        return K_M4A;
    if (arc_name_has_ext(name, ".m4a") || arc_name_has_ext(name, ".m4b")) return K_M4A;
    if (trk_probe(h, n, name)) return K_TRK;
    if (n >= 3 && !memcmp(h, "ID3", 3)) return K_MP3;
    if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) return K_MP3;
    if (arc_name_has_ext(name, ".mp3") || arc_name_has_ext(name, ".mp2")) return K_MP3;
    return -1;
}

gen_t *gen_open(const char *path)
{
    gen_t *g = calloc(1, sizeof(*g));
    if (!g || !bio_open(&g->io, path)) {
        free(g);
        return NULL;
    }

    u8 h[0x40];
    u32 n = (u32)bio_read(&g->io, h, sizeof(h));
    bio_seek(&g->io, 0, SEEK_SET);
    g->kind = kind_of(h, n, path);
    gen_info *i = &g->info;
    bool ok = false;

    switch (g->kind) {
    case K_WAV:
        ok = drwav_init(&g->d.wav, dr_read, wav_seek, wav_tell, &g->io, NULL);
        if (!ok) break;
        i->channels = g->d.wav.channels;
        i->rate = (int)g->d.wav.sampleRate;
        i->total = g->d.wav.totalPCMFrameCount;
        i->format = g->d.wav.container == drwav_container_aiff ? "AIFF" : "WAV";
        switch (g->d.wav.translatedFormatTag) {
        case DR_WAVE_FORMAT_PCM: i->codec = "PCM"; break;
        case DR_WAVE_FORMAT_IEEE_FLOAT: i->codec = "float"; break;
        case DR_WAVE_FORMAT_ALAW: i->codec = "A-law"; break;
        case DR_WAVE_FORMAT_MULAW: i->codec = "mu-law"; break;
        case DR_WAVE_FORMAT_ADPCM: i->codec = "MS ADPCM"; break;
        case DR_WAVE_FORMAT_DVI_ADPCM: i->codec = "IMA ADPCM"; break;
        default: i->codec = "?"; break;
        }
        snprintf(i->detail, sizeof(i->detail), "%u-bit", (unsigned)g->d.wav.bitsPerSample);
        break;
    case K_MP3:
        ok = drmp3_init(&g->d.mp3, dr_read, mp3_seek, mp3_tell, NULL, &g->io, NULL);
        if (!ok) break;
        i->channels = (int)g->d.mp3.channels;
        i->rate = (int)g->d.mp3.sampleRate;
        /* the length means going through every frame header once (no decoding): a moment */
        {
            drmp3_uint64 pcm = 0;
            ui_busy_begin("Reading MP3");
            if (drmp3_get_mp3_and_pcm_frame_count(&g->d.mp3, NULL, &pcm)) i->total = (u64)pcm;
            ui_busy_end();
        }
        i->format = "MP3";
        i->codec = "MPEG audio";
        break;
    case K_FLAC:
        g->d.flac = drflac_open(dr_read, flac_seek, flac_tell, &g->io, NULL);
        ok = g->d.flac != NULL;
        if (!ok) break;
        i->channels = g->d.flac->channels;
        i->rate = (int)g->d.flac->sampleRate;
        i->total = g->d.flac->totalPCMFrameCount;
        i->format = "FLAC";
        i->codec = "lossless";
        snprintf(i->detail, sizeof(i->detail), "%u-bit", (unsigned)g->d.flac->bitsPerSample);
        break;
    case K_OGG: {
        ov_callbacks cb = { ogg_read, ogg_seek, ogg_close, ogg_tell };
        ok = ov_open_callbacks(&g->io, &g->d.ogg, NULL, 0, cb) == 0;
        if (!ok) break;
        vorbis_info *vi = ov_info(&g->d.ogg, -1);
        if (!vi) {
            ov_clear(&g->d.ogg);
            ok = false;
            break;
        }
        i->channels = vi->channels;
        i->rate = (int)vi->rate;
        ogg_int64_t t = ov_pcm_total(&g->d.ogg, -1);
        i->total = t > 0 ? (u64)t : 0;
        i->format = "Ogg";
        i->codec = "Vorbis";
        long br = ov_bitrate(&g->d.ogg, -1);
        if (br > 0) snprintf(i->detail, sizeof(i->detail), "%ld kbps", br / 1000);
        break;
    }
#ifdef HAVE_AAC
    case K_M4A:
        g->d.aac = aac_open(g);
        ok = g->d.aac != NULL;
        if (!ok) break;
        i->format = "M4A";
        i->codec = "AAC";
        break;
#endif
    case K_TRK: {
        /* modules are small and parsed whole: read the file, let the player copy what it keeps */
        u64 size = g->io.f.size;
        if (size < 0x30 || size > TRK_MAX_FILE) break;
        u8 *file = malloc((size_t)size);
        if (!file) break;
        bio_seek(&g->io, 0, SEEK_SET);
        size_t got = bio_read(&g->io, file, (size_t)size);
        g->d.trk = got == size ? trk_open(file, (u32)size, TRK_RATE) : NULL;
        free(file);
        ok = g->d.trk != NULL;
        if (!ok) break;
        i->channels = 2;
        i->rate = TRK_RATE;
        i->total = trk_frames(g->d.trk);
        i->format = trk_format(g->d.trk);
        i->codec = "tracker";
        snprintf(i->detail, sizeof(i->detail), "%d ch", trk_channels(g->d.trk));
        break;
    }
    default:
        break;
    }

    if (ok && (i->channels < 1 || i->channels > 8 || i->rate < 1000 || i->rate > 192000)) {
        gen_close(g);
        return NULL;
    }
    if (!ok) {
        bio_close(&g->io);
        free(g);
        return NULL;
    }
    return g;
}

const gen_info *gen_get_info(const gen_t *g) { return &g->info; }

u32 gen_read(gen_t *g, s16 *out, u32 frames)
{
    switch (g->kind) {
    case K_WAV:  return (u32)drwav_read_pcm_frames_s16(&g->d.wav, frames, out);
    case K_MP3:  return (u32)drmp3_read_pcm_frames_s16(&g->d.mp3, frames, out);
    case K_FLAC: return (u32)drflac_read_pcm_frames_s16(g->d.flac, frames, out);
    case K_TRK:  return trk_read(g->d.trk, out, frames);
#ifdef HAVE_AAC
    case K_M4A: {
        aac_t *a = g->d.aac;
        u32 got = 0;
        while (got < frames) {
            if (a->pos >= a->n && !aac_fill(g)) break;
            u32 t = a->n - a->pos < frames - got ? a->n - a->pos : frames - got;
            memcpy(out + got * a->ch, a->pcm + a->pos * a->ch, t * a->ch * sizeof(s16));
            a->pos += t;
            got += t;
        }
        return got;
    }
#endif
    case K_OGG: {
        u32 got = 0, fb = (u32)g->info.channels * 2;
        while (got < frames) {
            int bs;
            long r = ov_read(&g->d.ogg, (char *)(out + got * g->info.channels),
                             (int)((frames - got) * fb), &bs);
            if (r == OV_HOLE) continue;         /* a gap in the stream: carry on */
            if (r <= 0) break;
            /* a chained stream may change its channel count: stop rather than play it wrong */
            vorbis_info *vi = ov_info(&g->d.ogg, -1);
            if (vi && vi->channels != g->info.channels) break;
            got += (u32)r / fb;
        }
        return got;
    }
    }
    return 0;
}

bool gen_rewind(gen_t *g)
{
    switch (g->kind) {
    case K_WAV:  return drwav_seek_to_pcm_frame(&g->d.wav, 0);
    case K_MP3:  return drmp3_seek_to_pcm_frame(&g->d.mp3, 0);
    case K_FLAC: return drflac_seek_to_pcm_frame(g->d.flac, 0);
    case K_OGG:  return ov_pcm_seek(&g->d.ogg, 0) == 0;
    case K_TRK:  trk_rewind(g->d.trk); return true;
#ifdef HAVE_AAC
    case K_M4A:  return gen_seek(g, 0);
#endif
    }
    return false;
}

bool gen_seek(gen_t *g, u64 frame)
{
    switch (g->kind) {
    case K_WAV:  return drwav_seek_to_pcm_frame(&g->d.wav, frame);
    case K_MP3:
        if (!g->mp3_tried) {                /* without a table, seeking decodes from the start */
            g->mp3_tried = true;
            drmp3_uint32 n = 600;
            drmp3_seek_point *pts = malloc(n * sizeof(*pts));
            if (pts && drmp3_calculate_seek_points(&g->d.mp3, &n, pts) &&
                drmp3_bind_seek_table(&g->d.mp3, n, pts))
                g->mp3_pts = pts;
            else
                free(pts);
        }
        return drmp3_seek_to_pcm_frame(&g->d.mp3, frame);
    case K_FLAC: return drflac_seek_to_pcm_frame(g->d.flac, frame);
    case K_OGG:  return ov_pcm_seek(&g->d.ogg, (ogg_int64_t)frame) == 0;
    case K_TRK:  return false;              /* modules are generated: no seeking */
#ifdef HAVE_AAC
    case K_M4A: {
        aac_t *a = g->d.aac;
        u64 pk = frame / a->per;
        if (pk >= (u64)a->pk.n) return false;
        int start = pk ? (int)pk - 1 : 0;   /* one packet earlier: the transform overlaps the last one */
        a->next = start;
        a->n = a->pos = 0;
        a->skip = frame - (u64)start * a->per;
        NeAACDecPostSeekReset(a->dec, start);
        return true;
    }
#endif
    }
    return false;
}

void gen_close(gen_t *g)
{
    if (!g) return;
    free(g->mp3_pts);
    switch (g->kind) {
    case K_WAV:  drwav_uninit(&g->d.wav); break;
    case K_MP3:  drmp3_uninit(&g->d.mp3); break;
    case K_FLAC: drflac_close(g->d.flac); break;
    case K_OGG:  ov_clear(&g->d.ogg); break;
    case K_TRK:  trk_close(g->d.trk); break;
#ifdef HAVE_AAC
    case K_M4A:  aac_close(g->d.aac); break;
#endif
    }
    bio_close(&g->io);
    free(g);
}

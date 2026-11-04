/*
 * The audio player's common formats, decoded to 16-bit PCM while playing (see audio.c):
 *   WAV / AIFF / AIFC   dr_wav: PCM, float, A-law, mu-law, IMA and MS ADPCM
 *   MP3 (and MP2)       dr_mp3
 *   FLAC                dr_flac
 *   Ogg Vorbis          Tremor (devkitPro's libvorbisidec)
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

#include "audio_gen.h"
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

enum { K_WAV, K_MP3, K_FLAC, K_OGG };

struct gen {
    bio_t io;
    int kind;
    union {
        drwav wav;
        drmp3 mp3;
        drflac *flac;
        OggVorbis_File ogg;
    } d;
    gen_info info;
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
        i->total = 0;                       /* finding it means reading the whole file */
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
    }
    return false;
}

void gen_close(gen_t *g)
{
    if (!g) return;
    switch (g->kind) {
    case K_WAV:  drwav_uninit(&g->d.wav); break;
    case K_MP3:  drmp3_uninit(&g->d.mp3); break;
    case K_FLAC: drflac_close(g->d.flac); break;
    case K_OGG:  ov_clear(&g->d.ogg); break;
    }
    bio_close(&g->io);
    free(g);
}

/*
 * Motion-JPEG video in AVI and QuickTime (.mov) files: what the Wii Photo Channel plays.
 *
 * Frames are JPEGs (libjpeg-turbo, which also fills in the Huffman tables camera AVIs leave out) read
 * from storage on demand. Audio is uncompressed PCM
 * (AVI WAVE_FORMAT_PCM, MOV 'sowt' / 'twos' / 'raw '), decoded up front like THP's; other audio
 * codecs play silent.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

#include "rgb_source.h"
#include "mc_io.h"
#include "qt.h"

#define MAX_AUDIO_BYTES (16u << 20)     /* shortcut: PCM past 16 MB is dropped (it is all held in RAM) */

typedef struct {
    FILE *f;
    spans vid, aud;
    int w, h;
    double fps;
    int ach, arate, abits, abe, apcm;   /* audio: channels, rate, bits, big-endian, plain PCM */
    uint8_t *frame_buf;
    uint32_t frame_cap;
    int16_t *audio;
    long asamples;
    uint8_t *file_buf;
} Mj;

static int le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---- AVI ---- */

typedef struct { Mj *m; int stype[16], nstr, vstr, astr; } avi_t;

static int avi_walk(avi_t *a, long end, int in_movi)
{
    FILE *f = a->m->f;
    uint8_t h[12];
    while (ftell(f) + 8 <= end && fread(h, 1, 8, f) == 8) {
        uint32_t sz = le32(h + 4);
        long body = ftell(f), next = body + ((sz + 1) & ~1u);
        if (next > end) next = end;
        if (!memcmp(h, "LIST", 4) || !memcmp(h, "RIFF", 4)) {
            if (fread(h + 8, 1, 4, f) != 4) return 0;
            int movi = !memcmp(h + 8, "movi", 4);
            if (movi || !memcmp(h + 8, "hdrl", 4) || !memcmp(h + 8, "strl", 4) ||
                !memcmp(h + 8, "rec ", 4) || !memcmp(h + 8, "AVIX", 4) || !memcmp(h + 8, "AVI ", 4)) {
                if (!avi_walk(a, next, movi || in_movi)) return 0;
            }
        } else if (in_movi) {
            /* "00dc" / "01wb": two digits of stream number, then the kind */
            int sn = (h[0] - '0') * 10 + h[1] - '0';
            if (h[0] >= '0' && h[0] <= '9' && h[1] >= '0' && h[1] <= '9') {
                if (sn == a->vstr && (h[2] == 'd') && !push(&a->m->vid, body, sz)) return 0;
                if (sn == a->astr && h[2] == 'w' && !push(&a->m->aud, body, sz)) return 0;
            }
        } else if (!memcmp(h, "strh", 4) && sz >= 28 && a->nstr < 16) {
            uint8_t s[48];
            if (fread(s, 1, sz < 48 ? sz : 48, f) < 28) return 0;
            int i = a->nstr++;
            a->stype[i] = !memcmp(s, "vids", 4) ? 1 : !memcmp(s, "auds", 4) ? 2 : 0;
            if (a->stype[i] == 1 && a->vstr < 0 && le32(s + 24) && le32(s + 20)) {
                /* the codec: 'MJPG' / 'mjpg' (also DIV-less cameras write 'jpeg' / 'AVRn') */
                if (memcmp(s + 4, "MJPG", 4) && memcmp(s + 4, "mjpg", 4) && memcmp(s + 4, "jpeg", 4) &&
                    memcmp(s + 4, "JPEG", 4) && memcmp(s + 4, "AVRn", 4) && memcmp(s + 4, "dmb1", 4))
                    a->stype[i] = 0;
                else {
                    a->vstr = i;
                    a->m->fps = (double)le32(s + 24) / le32(s + 20);    /* dwRate / dwScale */
                }
            } else if (a->stype[i] == 2 && a->astr < 0) a->astr = i;
        } else if (!memcmp(h, "strf", 4) && a->nstr > 0) {
            int i = a->nstr - 1;
            uint8_t s[24];
            if (sz >= 16 && fread(s, 1, sz < 24 ? sz : 24, f) >= 16) {
                if (i == a->vstr) {
                    a->m->w = le32(s + 4);
                    a->m->h = abs((int32_t)le32(s + 8));
                } else if (i == a->astr && le16(s) == 1) {          /* WAVE_FORMAT_PCM only */
                    a->m->ach = le16(s + 2);
                    a->m->arate = le32(s + 4);
                    a->m->abits = le16(s + 14);
                    a->m->apcm = 1;
                }
            }
        }
        if (fseek(f, next, SEEK_SET)) return 0;
    }
    return 1;
}

static int avi_open(Mj *m)
{
    avi_t a = { .m = m, .vstr = -1, .astr = -1 };
    fseek(m->f, 0, SEEK_END);
    long end = ftell(m->f);
    fseek(m->f, 0, SEEK_SET);
    return avi_walk(&a, end, 0);
}

/* ---- QuickTime ---- */

static int mov_open(Mj *m)
{
    FILE *f = m->f;
    uint8_t h[16];
    long pos = 0, size;
    uint8_t *moov = NULL;
    uint32_t mn = 0;
    int ok = 0;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    while (pos + 8 <= size) {                 /* top-level atoms: find moov, skip mdat */
        uint64_t sz;
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 8, f) != 8) break;
        sz = be32(h);
        if (sz == 1) {
            if (fread(h + 8, 1, 8, f) != 8) break;
            sz = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
        } else if (sz == 0) sz = size - pos;
        if (sz < 8 || pos + (long)sz > size) break;
        if (!memcmp(h + 4, "moov", 4)) {
            uint32_t hdr = be32(h) == 1 ? 16 : 8;
            if (sz - hdr > (16u << 20)) break;
            mn = sz - hdr;
            moov = malloc(mn);
            fseek(f, pos + hdr, SEEK_SET);
            if (!moov || fread(moov, 1, mn, f) != mn) break;
            break;
        }
        pos += sz;
    }
    if (!moov) return 0;

    /* each trak: its handler says video or sound, stsd the codec */
    for (uint32_t o = 0; o + 8 <= mn;) {
        uint32_t sz = be32(moov + o);
        if (sz < 8 || sz > mn - o) break;
        if (!memcmp(moov + o + 4, "trak", 4)) {
            blob mdia = atom(moov + o + 8, sz - 8, "mdia");
            blob hdlr = atom(mdia.p, mdia.n, "hdlr"), mdhd = atom(mdia.p, mdia.n, "mdhd");
            blob minf = atom(mdia.p, mdia.n, "minf");
            blob stbl = atom(minf.p, minf.n, "stbl");
            blob stsd = atom(stbl.p, stbl.n, "stsd"), stts = atom(stbl.p, stbl.n, "stts");
            if (hdlr.p && hdlr.n >= 12 && stsd.p && stsd.n >= 16 + 36 && mdhd.p && mdhd.n >= 20) {
                const uint8_t *e = stsd.p + 8;                  /* the first sample description */
                uint32_t total, fixed;
                if (!memcmp(hdlr.p + 8, "vide", 4) && !m->vid.n &&
                    (!memcmp(e + 4, "jpeg", 4) || !memcmp(e + 4, "mjpa", 4))) {
                    m->w = be16(e + 8 + 24);
                    m->h = be16(e + 8 + 26);
                    if (stts.p && stts.n >= 16 && be32(stts.p + 12)) m->fps = (double)be32(mdhd.p + 12) / be32(stts.p + 12);
                    if (!track_spans(stbl, &m->vid, &total, &fixed) || fixed) m->vid.n = 0;
                } else if (!memcmp(hdlr.p + 8, "soun", 4) && !m->aud.n &&
                           (!memcmp(e + 4, "sowt", 4) || !memcmp(e + 4, "twos", 4) ||
                            !memcmp(e + 4, "raw ", 4))) {
                    m->ach = be16(e + 24);
                    m->abits = !memcmp(e + 4, "raw ", 4) ? 8 : be16(e + 26);
                    m->arate = be32(e + 32) >> 16;
                    m->abe = !memcmp(e + 4, "twos", 4);
                    m->apcm = 1;
                    if (!track_spans(stbl, &m->aud, &total, &fixed) || !fixed) m->aud.n = 0;
                    else if (fixed != (uint32_t)(m->ach * m->abits / 8)) m->aud.n = 0;
                }
            }
        }
        o += sz;
    }
    ok = m->vid.n > 0;
    free(moov);
    return ok;
}

/* ---- common ---- */

static void build_audio(Mj *m, RgbSource *out)
{
    if (!m->apcm || m->ach < 1 || m->ach > 2 || m->arate < 4000 || (m->abits != 8 && m->abits != 16)) return;
    int bps = m->abits / 8, fb = m->ach * bps;
    uint64_t bytes = 0;
    for (int i = 0; i < m->aud.n; i++) bytes += m->aud.v[i].sz;
    if (bytes > MAX_AUDIO_BYTES) bytes = MAX_AUDIO_BYTES;
    long frames = (long)(bytes / fb);
    if (frames <= 0) return;
    int16_t *pcm = malloc((size_t)frames * m->ach * sizeof(int16_t));
    uint8_t *buf = malloc(64 * 1024);
    if (!pcm || !buf) {
        free(pcm);
        free(buf);
        return;
    }
    long w = 0;                               /* samples written */
    long total = frames * m->ach;
    for (int i = 0; i < m->aud.n && w < total; i++) {
        uint32_t left = m->aud.v[i].sz;
        if (fseek(m->f, m->aud.v[i].off, SEEK_SET)) break;
        while (left && w < total) {
            uint32_t n = left < 65536 ? left : 65536;
            if (fread(buf, 1, n, m->f) != n) break;
            for (uint32_t k = 0; k + bps <= n && w < total; k += bps)
                pcm[w++] = bps == 1 ? (int16_t)((buf[k] - 128) << 8)
                         : (int16_t)(m->abe ? be16(buf + k) : le16(buf + k));
            left -= n;
        }
    }
    free(buf);
    m->audio = pcm;
    out->audio = pcm;
    out->audio_samples = w / m->ach;
    out->sample_rate = m->arate;
    out->channels = m->ach;
}

struct jerr { struct jpeg_error_mgr mgr; jmp_buf jb; };
static void jfail(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static int mj_get_frame(RgbSource *s, int idx, uint8_t *dst)
{
    Mj *m = s->priv;
    if (idx < 0 || idx >= m->vid.n) return -1;
    uint32_t size = m->vid.v[idx].sz;
    if (size < 4) return -1;
    if (size > m->frame_cap) {
        uint8_t *nb = realloc(m->frame_buf, size);
        if (!nb) return -1;
        m->frame_buf = nb;
        m->frame_cap = size;
    }
    if (fseek(m->f, m->vid.v[idx].off, SEEK_SET) || fread(m->frame_buf, 1, size, m->f) != size) return -1;

    struct jpeg_decompress_struct c;
    struct jerr err;
    int ret = -1;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jfail;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&c);
        return -1;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, m->frame_buf, size);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    if (c.output_width == (unsigned)s->w && c.output_height == (unsigned)s->h) {
        while (c.output_scanline < c.output_height) {
            JSAMPROW rp = dst + (size_t)c.output_scanline * s->w * 3;
            jpeg_read_scanlines(&c, &rp, 1);
        }
        ret = 0;
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return ret;
}

static void mj_close(RgbSource *s)
{
    Mj *m = s->priv;
    if (!m) return;
    if (m->f) fclose(m->f);
    free(m->vid.v); free(m->aud.v); free(m->frame_buf); free(m->audio); free(m->file_buf);
    free(m);
    s->priv = NULL;
}

int mjpeg_open(RgbSource *out, const char *path)
{
    memset(out, 0, sizeof(*out));
    Mj *m = calloc(1, sizeof(*m));
    if (!m) return -1;
    m->f = mc_fopen(path);
    if (!m->f) { free(m); return -1; }
    m->file_buf = malloc(128 * 1024);
    if (m->file_buf) setvbuf(m->f, (char *)m->file_buf, _IOFBF, 128 * 1024);

    uint8_t h[12] = { 0 };
    if (fread(h, 1, 12, m->f) != 12) goto fail;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "AVI ", 4)) {
        if (!avi_open(m)) goto fail;
    } else if (!memcmp(h + 4, "ftyp", 4) || !memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "mdat", 4) ||
               !memcmp(h + 4, "wide", 4) || !memcmp(h + 4, "free", 4)) {
        if (!mov_open(m)) goto fail;
    } else goto fail;

    if (m->vid.n < 1 || m->w < 8 || m->h < 8 || m->w > 4096 || m->h > 4096) goto fail;
    if (m->fps < 1.0 || m->fps > 120.0) m->fps = 30.0;

    out->w = m->w;
    out->h = m->h;
    out->frame_count = m->vid.n;
    out->fps = m->fps;
    out->independent_frames = 1;
    out->channels = 1;
    out->get_frame = mj_get_frame;
    out->close = mj_close;
    out->priv = m;
    build_audio(m, out);
    return 0;
fail:
    out->priv = m;
    mj_close(out);
    return -1;
}

/*
 * Mobiclip (.mo), THP and Flipnote (.kwz / .ppm) playback for GodModeWii.
 *
 * The playback loops below are quatric's wii-mobiclip-player (MIT, see
 * LICENSE and CREDITS_MOBICLIP.md in this folder), unchanged apart from
 * reading input through GodModeWii and borrowing the screen from the app's
 * console instead of owning VIDEO_Init.
 *
 * Controls: A / + pause, B / HOME stop.
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "video.h"
#include "audio_out.h"
#include "mo_demux.h"
#include "mo_audio.h"
#include "mo_vorbis.h"
#include "mobi_dec.h"
#include "rgb_source.h"
#include "mc_player.h"
#include "input.h"
#include "ui.h"
#include "vfs.h"

static int has_ext(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && !strcasecmp(s + n - e, ext);
}

bool video_is_name(const char *name)
{
    return has_ext(name, ".mo") || has_ext(name, ".thp") ||
           has_ext(name, ".kwz") || has_ext(name, ".ppm");
}

#define AUDIO_PACKET_FRAMES (64 * 1024)
#define AUDIO_PUSH_FRAMES   2048

static int16_t aud_st[AUDIO_PUSH_FRAMES * 2];

/* Mix to stereo in small chunks and return the number of input frames queued. */
static int push_stereo(const int16_t *in, int ns, int ch)
{
    if (ns <= 0) return 0;
    if (ch == 2) return audio_out_push(in, ns);

    int total = 0;
    while (total < ns) {
        int n = ns - total;
        int space = audio_out_space();
        if (n > space) n = space;
        if (n > AUDIO_PUSH_FRAMES) n = AUDIO_PUSH_FRAMES;
        if (n <= 0) break;

        if (ch == 1) {
            for (int i = 0; i < n; i++)
                aud_st[i*2] = aud_st[i*2+1] = in[total + i];
        } else {
            for (int i = 0; i < n; i++) {
                aud_st[i*2]   = in[(total + i)*ch];
                aud_st[i*2+1] = in[(total + i)*ch + 1];
            }
        }
        int pushed = audio_out_push(aud_st, n);
        total += pushed;
        if (pushed < n) break;
    }
    return total;
}

#define PCM_PIPELINE_FRAMES 5

/* Read/decode one complete video+PCM chunk. The decoder owns six rotating
 * frame slots, so keeping five decoded frames queued never requires another
 * image allocation and the next decode only reuses an already displayed slot. */
static int read_pcm_frame(MoDemux *mux, MobiDecoder *dec, MoAudio *audio_dec,
                          int16_t *audio_buf, int audio_cap, int ch,
                          MoFrame **out)
{
    MoPacket pkt;
    MoFrame *frame = NULL;
    int got_video = 0;
    while (mo_demux_read(mux, &pkt) == 1) {
        if (!pkt.is_audio) {
            if (mobi_decode(dec, pkt.data, pkt.size, &frame) == 0 && frame)
                got_video = 1;
        } else {
            if (pkt.size > 0) {
                int decoded = mo_audio_decode(audio_dec, pkt.data, pkt.size,
                                              audio_buf, audio_cap);
                if (decoded > 0) push_stereo(audio_buf, decoded, ch);
            }
            if (got_video) {
                *out = frame;
                return 1;
            }
        }
    }
    if (got_video) {
        *out = frame;
        return 1;
    }
    return 0;
}

/* Controllers every third frame while playing: GodModeWii's input layer also polls the USB
 * keyboard and the other pads, time a frame can't spare. Presses are kept until read. */
static u32 poll_some(void)
{
    static int n;
    return ++n % 3 ? 0 : input_poll_nowait();
}

static int play_pcm(MoDemux *mux, MobiDecoder *dec, MoAudio *audio_dec,
                     int16_t *audio_buf, int audio_cap, int ch,
                     int period_us, int hz, int vspf)
{
    MoFrame *frames[PCM_PIPELINE_FRAMES];
    int queued = 0;
    if (!audio_out_prepare(mux->sample_rate)) return 0;

    while (queued < PCM_PIPELINE_FRAMES &&
           read_pcm_frame(mux, dec, audio_dec, audio_buf, audio_cap, ch,
                          &frames[queued]))
        queued++;
    if (queued == 0) return 0;

    audio_out_begin();
    vid_clear_both();
    long long start_retrace = vid_retraces();
    long display_index = 0;
    int head = 0, stop = 0, paused = 0;

    while (queued > 0 && !stop) {
        while (paused && !stop) {
            u32 pb = input_poll_nowait();
            long long ideal = (display_index + 1) * (long long)period_us * hz / 1000000;
            if (pb & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) {
                paused = 0;
                start_retrace = vid_retraces() - ideal;
            }
            if (pb & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
            VIDEO_WaitVSync();
        }
        if (stop) break;

        long long ideal = (display_index + 1) * (long long)period_us * hz / 1000000;
        long target = start_retrace + ideal;
        int drop = (int)(vid_retraces() - target) > vspf;
        if (!drop) {
            vid_draw_frame(frames[head]);
            vid_flip();
        }

        /* Refill the slot only after its pixels have been copied to the XFB. */
        MoFrame *next = NULL;
        if (read_pcm_frame(mux, dec, audio_dec, audio_buf, audio_cap, ch, &next))
            frames[head] = next;
        else
            queued--;
        head = (head + 1) % PCM_PIPELINE_FRAMES;
        display_index++;

        long long current = vid_retraces() - start_retrace;
        while (current < ideal) {
            vid_vsync();
            current = vid_retraces() - start_retrace;
        }

        u32 b = poll_some();
        if (b & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
        if (b & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) paused = 1;
    }
    audio_out_stop();
    return 1;
}

static void play_file(const char *path)
{
    MoDemux mux;
    if (mo_demux_open(&mux, path) != 0) return;

    MobiDecoder *dec = mobi_open(mux.width, mux.height);
    if (!dec) { mo_demux_close(&mux); return; }

    int is_vorbis = mux.audio_type == MO_AUDIO_VORBIS;
    int has_audio = mux.audio_type != MO_AUDIO_NONE;
    int ach = mux.channels > 0 ? mux.channels : 2;

    /* Decode every codec incrementally into a bounded per-section buffer. */
    MoAudio audio_dec;
    MoVorbis *vorbis_dec = NULL;
    int16_t *audio_buf = NULL;
    int audio_pending = 0;
    int audio_pos = 0;

    if (has_audio) {
        if (is_vorbis) {
            vorbis_dec = mo_vorbis_open(&mux);
            if (vorbis_dec) ach = mo_vorbis_channels(vorbis_dec);
            else has_audio = 0;
        } else {
            mo_audio_init(&audio_dec, mux.audio_type, ach);
        }
        if (has_audio)
            audio_buf = malloc((size_t)AUDIO_PACKET_FRAMES * ach * sizeof(int16_t));
        if (!audio_buf) has_audio = 0;
    }
    if (has_audio && mux.audio_type != MO_AUDIO_PCM)
        audio_out_start(mux.sample_rate);

    /* Frame rate is fps_den/fps_num (fps_num is fixed at 256), so the frame
     * period in seconds is fps_num/fps_den. e.g. fps_den=7680 -> 30 fps. */
    int period_us = mux.fps_den > 0
        ? (int)((long long)mux.fps_num * 1000000 / mux.fps_den)
        : 33333;
    int hz = vid_refresh_hz();
    int vspf = (int)(((long long)period_us * hz + 500000) / 1000000);
    if (vspf < 1) vspf = 1;

    if (has_audio && mux.audio_type == MO_AUDIO_PCM) {
        if (play_pcm(&mux, dec, &audio_dec, audio_buf, AUDIO_PACKET_FRAMES,
                     ach, period_us, hz, vspf)) {
            free(audio_buf);
            mobi_close(dec);
            mo_demux_close(&mux);
            return;
        }
        has_audio = 0;
    }

    vid_clear_both();                 /* letterbox bars are static; clear once */
    long long start_retrace = vid_retraces();
    MoPacket pkt;
    int stop = 0, paused = 0;

    while (!stop && mo_demux_read(&mux, &pkt) == 1) {
        if (audio_pending > 0) {
            int pushed = push_stereo(audio_buf + audio_pos * ach,
                                     audio_pending, ach);
            audio_pos += pushed;
            audio_pending -= pushed;
            if (audio_pending == 0) audio_pos = 0;
        }

        /* pause loop: hold on the current frame, keep polling input */
        while (paused && !stop) {
            u32 pb = input_poll_nowait();
            long long ideal_retrace = (long long)mux.cur_frame * period_us * hz / 1000000;
            if (pb & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) { paused = 0; start_retrace = vid_retraces() - ideal_retrace; }
            if (pb & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
            VIDEO_WaitVSync();
        }
        if (stop) break;

        if (!pkt.is_audio) {
            long long ideal_retrace = (long long)mux.cur_frame * period_us * hz / 1000000;
            long target = start_retrace + ideal_retrace;
            MoFrame *fr = NULL;
            if (mobi_decode(dec, pkt.data, pkt.size, &fr) == 0 && fr) {
                int drop = (int)(vid_retraces() - target) > vspf;
                if (!drop) {
                    vid_draw_frame(fr);
                    vid_flip();
                }
            }
            /* Hold this frame until its scheduled retrace. Absolute schedule
             * self-corrects: a slow frame simply waits less (or not at all). */
            long long current_retrace = vid_retraces() - start_retrace;
            while (current_retrace < ideal_retrace) {
                vid_vsync();
                current_retrace = vid_retraces() - start_retrace;
            }
        } else if (has_audio && pkt.size > 0) {
            /*
             * Append this section behind any PCM the output ring has not yet
             * accepted. Compacting only moves bounded decoded PCM; compressed
             * input is still consumed directly from the demuxer.
             */
            if (audio_pos > 0 && audio_pending > 0) {
                memmove(audio_buf, audio_buf + audio_pos * ach,
                        (size_t)audio_pending * ach * sizeof(int16_t));
                audio_pos = 0;
            }
            int available = AUDIO_PACKET_FRAMES - audio_pending;
            if (available > 0) {
                int decoded = is_vorbis
                    ? mo_vorbis_decode(vorbis_dec, pkt.data, pkt.size,
                                       audio_buf + audio_pending * ach, available)
                    : mo_audio_decode(&audio_dec, pkt.data, pkt.size,
                                      audio_buf + audio_pending * ach, available);
                if (decoded > 0) audio_pending += decoded;
            }
            if (audio_pending > 0) {
                int pushed = push_stereo(audio_buf, audio_pending, ach);
                audio_pos = pushed;
                audio_pending -= pushed;
                if (audio_pending == 0) audio_pos = 0;
            }
        }

        u32 b = poll_some();
        if (b & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
        if (b & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) paused = 1;
    }

    if (has_audio) audio_out_stop();
    mo_vorbis_close(vorbis_dec);
    free(audio_buf);
    mobi_close(dec);
    mo_demux_close(&mux);
}

/* Shared playback loop for packed-RGB24 sources (KWZ / PPM / THP). Audio is
 * already pre-decoded into src->audio by the opener; we stream it against the
 * hardware retrace clock exactly like play_file() does for the .mo path. */
static void play_rgb(RgbSource *src)
{
    int has_audio = src->audio && src->audio_samples > 0;
    int ach = src->channels > 0 ? src->channels : 1;
    long vtotal = has_audio ? src->audio_samples : 0, vpos = 0;
    int frame_size = src->w * src->h * 3;
    uint8_t *rgb = malloc(frame_size);
    if (!rgb) return;

    if (has_audio) audio_out_start(src->sample_rate);

    double fps = src->fps > 0.01 ? src->fps : 30.0;
    int period_us = (int)(1000000.0 / fps);
    int hz = vid_refresh_hz();
    int vspf = (int)(((long long)period_us * hz + 500000) / 1000000);
    if (vspf < 1) vspf = 1;

    vid_clear_both();
    long long start_retrace = vid_retraces();
    int stop = 0, paused = 0, frame = 0;

    while (!stop) {
        while (paused && !stop) {
            u32 pb = input_poll_nowait();
            long long ideal = (long long)frame * period_us * hz / 1000000;
            if (pb & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) { paused = 0; start_retrace = vid_retraces() - ideal; }
            if (pb & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
            VIDEO_WaitVSync();
        }
        if (stop) break;

        /* THP frames are independent JPEGs.  On Wii, a full-resolution JPEG
         * can take longer than one video period to decode.  Jump over frames
         * whose presentation time has already passed instead of decoding and
         * dropping every late frame forever.  PPM/KWZ remain strictly
         * sequential because their frames depend on earlier layer state. */
        if (src->independent_frames) {
            long long elapsed = vid_retraces() - start_retrace;
            long desired = (long)(elapsed * 1000000LL /
                                  ((long long)period_us * hz));
            if (desired > frame) frame = (int)desired;
            if (frame >= src->frame_count) break;
        }

        long long ideal_retrace = (long long)frame * period_us * hz / 1000000;
        long target = start_retrace + ideal_retrace;

        if (src->get_frame(src, frame, rgb) == 0) {
            int drop = (int)(vid_retraces() - target) > vspf;
            if (!drop || src->independent_frames) {
                vid_draw_rgb24(rgb, src->w, src->h);
                vid_flip();
            }
        }

        if (has_audio && vpos < vtotal) {
            long long current_retrace = vid_retraces() - start_retrace;
            long ideal_pos = (long)(current_retrace * src->sample_rate / hz);
            long target_pos = ideal_pos + src->sample_rate / 4;
            if (target_pos > vtotal) target_pos = vtotal;
            long n = target_pos - vpos;
            if (n > 0) {
                int space = audio_out_space();
                if (n > space) n = space;
                if (n > 0) {
                    push_stereo(src->audio + vpos * ach, (int)n, ach);
                    vpos += n;
                }
            }
        }

        long long current_retrace = vid_retraces() - start_retrace;
        while (current_retrace < ideal_retrace) {
            vid_vsync();
            current_retrace = vid_retraces() - start_retrace;
        }

        u32 b = poll_some();
        if (b & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) stop = 1;
        if (b & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) paused = 1;
        frame++;

        if (frame >= src->frame_count) {
            if (!src->loops) break;         /* PPM/KWZ loop; others stop */
            frame = 0; vpos = 0;
            start_retrace = vid_retraces();  /* restart the clock for the loop */
        }
    }

    if (has_audio) audio_out_stop();
    src->close(src);
    free(rgb);
}


bool video_play(const char *path)
{
    const char *name = vfs_basename(path);
    if (!video_is_name(name)) return false;

    ui_clear();
    ui_goto(ui_rows() / 2, ui_cols() / 2 - 5);
    printf("Loading...");
    VIDEO_WaitVSync();

    RgbSource src;
    int rgb = !has_ext(name, ".mo");
    if (rgb) {
        int r = has_ext(name, ".kwz") ? kwz_open(&src, path)
              : has_ext(name, ".ppm") ? ppm_open(&src, path)
              : thp_open(&src, path);
        if (r != 0) return false;
    } else {
        MoDemux probe;
        if (mo_demux_open(&probe, path) != 0) return false;
        mo_demux_close(&probe);
    }

    if (!vid_attach()) {
        if (rgb) src.close(&src);
        ui_message("VIDEO", C_YELLOW, "Not enough memory for the video framebuffers.");
        return true;
    }
    if (rgb) play_rgb(&src);
    else play_file(path);
    int w, h;
    vid_detach(ui_framebuffer(&w, &h));

    /* Swallow the B / HOME press that stopped playback. */
    while (input_poll_nowait()) VIDEO_WaitVSync();
    return true;
}

#ifndef SEQSYNTH_H
#define SEQSYNTH_H

/* NW4R sequences (RSEQ) from a BRSAR sound archive, played through their instrument bank (RBNK)
 * and its waves (RWAR): a small sequencer and a software synthesizer with the NW4R envelopes.
 * The formats follow VGMTrans' RSAR reader (zlib licence; see seqsynth.c).
 * Plain C, no console dependencies: it renders 48 kHz stereo 16-bit samples. */

#include <stdint.h>
#include <stdbool.h>

#define SEQ_RATE 48000

/* Random access to the BRSAR file: false on a read error. */
typedef bool (*seq_read_fn)(void *ctx, uint64_t off, void *buf, uint32_t len);

typedef struct seq seq_t;

/* Opens the sequence sound called `name`, as the BRSAR browser names it ('/', '|', '\\' and control
 * characters as '_'; "soundNNNN" when unnamed). NULL with *err set on failure. */
seq_t *seq_open(seq_read_fn rd, void *ctx, uint64_t size, const char *name, const char **err);
void seq_close(seq_t *s);

/* Renders up to `frames` stereo frames into out (interleaved). Fewer, or 0, once the sequence
 * has finished. Without repeat a looping sequence plays its loop twice and then fades out; with
 * repeat it loops for ever, and one without a loop starts again from the top when it ends. */
uint32_t seq_render(seq_t *s, int16_t *out, uint32_t frames);
void seq_set_repeat(seq_t *s, bool repeat);

/* For the player screen. */
const char *seq_bank_name(const seq_t *s);
int seq_tracks(const seq_t *s);                     /* tracks still running */
int seq_voices(const seq_t *s);                     /* notes sounding */
uint64_t seq_frames(const seq_t *s);                /* frames rendered so far */
int seq_tempo(const seq_t *s);                      /* beats per minute */

#define SEQ_MAX_TRACKS 16
/* Tracks with bit i set are silent (they keep playing, so they come back in time). */
void seq_set_muted(seq_t *s, uint32_t mask);
/* Track i: 0 not started, 1 running, 2 finished (or only its last notes still sounding). Its
 * program, notes sounding and loudest note since the previous call (0..about 1). */
int seq_track(seq_t *s, int i, int *prg, int *notes, float *level);

#endif

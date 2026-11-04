#ifndef AUDIO_GEN_H
#define AUDIO_GEN_H

#include <stdbool.h>
#include <gccore.h>

/* Common audio formats for the player (audio_gen.c): WAV / AIFF, MP3, FLAC, Ogg Vorbis. */

typedef struct gen gen_t;

typedef struct {
    int channels, rate;
    u64 total;                  /* frames, 0 when not known up front (MP3) */
    const char *format, *codec;
    char detail[24];            /* "16-bit", "192 kbps", ... */
} gen_info;

bool gen_is_name(const char *name);
gen_t *gen_open(const char *path);          /* NULL if it isn't one of these, or is damaged */
const gen_info *gen_get_info(const gen_t *g);
/* interleaved, info.channels per frame; 0 at the end */
u32 gen_read(gen_t *g, s16 *out, u32 frames);
bool gen_rewind(gen_t *g);
void gen_close(gen_t *g);

#endif

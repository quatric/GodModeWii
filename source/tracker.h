#ifndef TRACKER_H
#define TRACKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Tracker module player (tracker.c): ProTracker .mod (4 to 32 channels), ScreamTracker .s3m,
 * FastTracker 2 .xm and Impulse Tracker .it, mixed to 16-bit stereo. */

typedef struct trk trk_t;

/* Does this look like a module? h is the start of the file (n bytes, at least 0x30 for an S3M);
 * a MOD's signature is at 0x438, so it is tested by trk_open() instead and only the name decides. */
bool trk_probe(const uint8_t *h, uint32_t n, const char *name);
/* Is this the 4-byte signature at 0x438 of a ProTracker module (M.K., 8CHN, 16CH ...)? */
bool trk_is_mod_sig(const uint8_t *sig);
/* Parses a whole module (copies what it needs). NULL if it is not one, or is too damaged. */
trk_t *trk_open(const uint8_t *data, uint32_t len, int rate);
const char *trk_format(const trk_t *t);     /* "MOD", "S3M", "XM", "IT" */
const char *trk_title(const trk_t *t);
int trk_channels(const trk_t *t);
uint64_t trk_frames(const trk_t *t);        /* length of one play-through, in frames */
/* interleaved stereo; returns 0 at the end of the song */
uint32_t trk_read(trk_t *t, int16_t *out, uint32_t frames);
void trk_rewind(trk_t *t);
void trk_close(trk_t *t);

#endif

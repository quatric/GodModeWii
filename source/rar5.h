#ifndef RAR5_H
#define RAR5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "unarr-imp.h"

/* A small RAR5 reader (the unarr library only reads RAR 1.5 - 4.x).
 * Single volume, no encryption, no symlinks. Modelled on the public format
 * description in libarchive / the RAR 5.0 technote. */

typedef struct rar5 rar5_t;

typedef bool (*rar5_sink)(void *ud, const uint8_t *data, size_t len);

typedef struct {
    char name[512];     /* UTF-8, "/" separators */
    uint64_t size;
    bool dir;
    bool skip;          /* symlink, hardlink... : no data to write */
} rar5_entry;

/* `stream` must be positioned anywhere; it is rewound. Returns NULL if this
 * isn't a RAR5 file. */
rar5_t *rar5_open(ar_stream *stream);
void rar5_close(rar5_t *r);

/* Moves to the next file. 1 = got one, 0 = end of archive, -1 = error. */
int rar5_next(rar5_t *r, rar5_entry *e);

/* Unpacks (or, with sink == NULL, skips) the current file. Returns false on
 * failure or when the sink stopped it. */
bool rar5_extract(rar5_t *r, rar5_sink sink, void *ud);

const char *rar5_error(const rar5_t *r);

/* Restarts at the first file. */
bool rar5_rewind(rar5_t *r);

#endif

/* TinySoundFont and TinyMidiLoader (Bernhard Schelling, MIT licence: see source/tsf/LICENSE) for
 * the MIDI player in audio.c, and the General MIDI soundbank it plays with when a MIDI file has
 * no soundfont of its own: Gervill's emergency soundbank (OpenJDK, GPL-2 with the Classpath
 * exception), kept zlib-packed in res/gmsf2.bin ("GMSF", u32 size, zlib). */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#ifdef GMW_AUTOTEST
#include <stdio.h>
static void mlog(const char *m)
{
    FILE *f = fopen("sd:/gmwtest.log", "a");
    if (f) {
        fprintf(f, "tml: %s\n", m);
        fclose(f);
    }
}
#define TML_WARN(m) mlog(m)
#define TML_ERROR(m) mlog(m)
#endif
#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "tsf/tsf.h"
#include "tsf/tml.h"
#pragma GCC diagnostic pop
#undef TSF_IMPLEMENTATION
#undef TML_IMPLEMENTATION

#include "midisynth.h"
#include "gmsf2_bin.h"

unsigned char *midi_default_soundfont(unsigned int *size)
{
    const unsigned char *z = gmsf2_bin;
    if (gmsf2_bin_size < 8 || memcmp(z, "GMSF", 4)) return NULL;
    uLongf n = (uLongf)z[4] << 24 | z[5] << 16 | z[6] << 8 | z[7];
    unsigned char *b = malloc(n);
    if (b && uncompress(b, &n, z + 8, gmsf2_bin_size - 8) != Z_OK) {
        free(b);
        return NULL;
    }
    *size = (unsigned int)n;
    return b;
}

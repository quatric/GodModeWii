#ifndef MIDISYNTH_H
#define MIDISYNTH_H

#include "tsf/tsf.h"
#include "tsf/tml.h"

/* The built-in General MIDI soundfont, unpacked into a new buffer (free it); NULL if out of memory. */
unsigned char *midi_default_soundfont(unsigned int *size);

#endif

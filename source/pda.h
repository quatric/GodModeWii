#ifndef PDA_H
#define PDA_H

#include <stdbool.h>
#include <stddef.h>

/* The folder the sketchpad and the voice memos save in: "GodModeWii/<sub>" on the SD card (or the
 * first writable USB drive). Made if missing; false when no drive can be written. */
bool pda_dir(const char *sub, char *out, size_t n);

/* The drawing board: pointer, mouse, pen tablet or uDraw. Saves 24-bit .bmp files. */
void draw_run(void);

/* Voice memos: record with a Wii Speak or a USB microphone, play them back. */
void memo_run(void);

#endif

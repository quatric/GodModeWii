#ifndef EXCITE_H
#define EXCITE_H

#include <stdbool.h>
#include <gccore.h>

/* Monster Games (Excite Truck / Excitebots) textures to malloc'd RGBA; NULL when the data isn't one.
 * .tex and .art / .img have no stored format, so it is recovered from the pixels (best effort). */
u8 *excite_tex(const u8 *d, u32 n, int *w, int *h);
u8 *excite_art(const u8 *d, u32 n, int *w, int *h);
u8 *excite_tm0(const u8 *d, u32 n, int *w, int *h);
/* The Excitebots 6-byte header (u16 width, u16 height, levels, renderer code 0x40 - 0x4F) at d. */
bool excite_head_ok(const u8 *d, u32 n);

#endif

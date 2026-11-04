#ifndef MEDIA_H
#define MEDIA_H

#include <stdbool.h>
#include <gccore.h>
#include "ftype.h"

/* Viewers for first-party GameCube / Wii formats (after quatric's nintoolbox
 * and mobipeg) plus a few common ones. View / listen only: nothing is
 * converted, exported or repacked. */

bool audio_is_name(const char *name);
bool audio_play(const char *path);          /* false if the file isn't a stream it can play */

bool image_is_name(const char *name);
bool image_view(const char *path);          /* false if there is no image in it */

/* GX texture data -> malloc'd RGBA bytes (NULL if short or out of memory); the bytes a texture takes. */
u8 *image_decode_gx(const u8 *data, u32 avail, int fmt, int w, int h, const u8 *pal, int palfmt, int palcount);
u32 image_gx_size(int fmt, int w, int h);

bool model_is_name(const char *name);
bool model_view(const char *path);          /* BRRES (MDL0) / BMD / BDL in 3D */

bool bmg_view(const char *path);            /* BMG message files as text */
bool breff_view(const char *path);          /* BREFF effect sets: the emitter list */
bool msbt_view(const char *path);           /* MSBT message files as text */
bool layout_view(const char *path);         /* BRLYT / BRLAN: textures, fonts, pane tree, text; animated panes */
/* BRLYT / BRLAN drawn with GX: the panes with their textures and materials, playing the
 * animations next to it (layout.c). False when it isn't a layout it can draw. */
bool lyt_view(const char *path);
/* model.c: starts GX (once) and sets it up to draw into the console framebuffer. */
void model_gx_setup(void);
bool kmp_view(const char *path);            /* Mario Kart Wii KMP: sections and counts */

/* Opens the viewer for a file of type t (ftype.h). False when it couldn't show it (the caller
 * then falls back to the archive / actions handling). */
bool media_open_type(const char *path, ftype_t t);
/* Detects the type from the file's magic, then opens the matching viewer. False when no viewer could show it (the caller
 * then falls back to the archive / actions handling). */
bool media_open(const char *path);

/* app.c's text viewer on a buffer (takes ownership of buf). */
void text_show(const char *title, u8 *buf, u32 got, u64 total, bool truncated);

#endif

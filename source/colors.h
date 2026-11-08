#ifndef COLORS_H
#define COLORS_H

#include <stdbool.h>

/* Colour themes, after the default profiles of macOS Terminal (Basic, Grass, Homebrew, Man Page, Novel,
 * Ocean, Pro, Red Sands, Silver Aerogel). libogc's console draws with a fixed 24-colour table, so a
 * theme rewrites that table in memory: background = "black", text = "white", bold text = bright
 * "white", and the six hues. */

int colors_count(void);
const char *colors_name(int i);
int colors_current(void);
/* Applies theme i and redraws the console's background; save: remember it on the card. */
void colors_apply(int i, bool save);
/* The current theme's text and background colours, 0xRRGGBB. */
unsigned colors_fg_rgb(void);
unsigned colors_bg_rgb(void);
/* Applies the saved one (sd:/apps/GodModeWii/theme), once the SD card is mounted. */
void colors_load(void);

#endif

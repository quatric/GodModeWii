#ifndef COLORS_H
#define COLORS_H

#include <stdbool.h>

/* Colour themes, after the default profiles of macOS Terminal (Basic, Grass, Homebrew, Man Page, Novel,
 * Ocean, Pro, Red Sands, Silver Aerogel). libogc's console draws with a fixed 24-colour table, so a
 * theme rewrites that table in memory: background = "black", text = "white", bold text = bright
 * "white", and six hues of its own (the selection, title bars, dialogs, headings). */

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
/* The Pride theme: an easter egg, listed once a file or folder named "pride" has been made (saved on the
 * card as sd:/apps/GodModeWii/pride, so it stays). colors_unlock_pride returns false if that couldn't be saved. */
bool colors_pride_unlocked(void);
bool colors_unlock_pride(void);
int colors_pride(void);

#endif

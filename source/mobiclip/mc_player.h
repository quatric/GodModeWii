#ifndef MC_PLAYER_H
#define MC_PLAYER_H
#include <stdbool.h>
/* Mobiclip .mo, THP, and Flipnote .kwz / .ppm (wii-mobiclip-player). */
bool video_is_name(const char *name);
/* False if the file couldn't be opened as a video. */
bool video_play(const char *path);
#endif

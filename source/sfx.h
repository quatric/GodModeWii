#ifndef SFX_H
#define SFX_H

/* Interface sounds: MoonShell 2.10's sound effects (GPL v2 or later), packed in res/sfx.bin by
 * scripts/make_sfx.py. Played through ASND, which the audio player shares (sfx_asnd_acquire). */

#include <stdbool.h>

/* the order of the sounds in res/sfx.bin */
enum { SFX_CLICK, SFX_OPEN, SFX_NOTIFY, SFX_POWEROFF, SFX_MOVEPAGE, SFX_LONGTAP, SFX_MEMOERASE,
       SFX_PENDOWN, SFX_UNDO, SFX_OPENING, SFX_COUNT };

/* Starts ASND if nothing has yet. The audio player calls this where it used to call ASND_Init. */
void sfx_asnd_acquire(void);
void sfx_play(int id);
/* Plays even when the sounds are turned off: the alarm. */
void sfx_play_force(int id);
/* Waits for what is playing to end (a moment): before powering off. */
void sfx_wait(void);
/* Silences the sound that has just started (the click of the A that chose something slow). */
void sfx_cancel(void);
bool sfx_enabled(void);
void sfx_set_enabled(bool on);
/* sd:/apps/GodModeWii/sfx_off exists: the sounds were turned off last time. */
void sfx_load_setting(void);

#endif

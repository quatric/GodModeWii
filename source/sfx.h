#ifndef SFX_H
#define SFX_H

/* Interface sounds: MoonShell 2.10's sound effects (GPL v2 or later), packed in res/sfx.bin by
 * scripts/make_sfx.py. Played through ASND, which the audio player shares (sfx_asnd_acquire).
 *
 * TiVo mode (the easter egg: + A - 1 2 - on the last credits page) plays TiVo's own
 * interface sounds instead, packed in res/tivo.bin by scripts/make_tivo.py. Each event below has a
 * MoonShell sound and a TiVo sound; some only sound in TiVo mode (the MoonShell set has no tick for
 * the cursor, for instance), so MoonShell mode sounds just as it always has. */

#include <stdbool.h>

enum {
    SFX_CLICK,      /* A: choose                        TiVo: select */
    SFX_OPEN,
    SFX_NOTIFY,     /* the alarm                        TiVo: alert */
    SFX_POWEROFF,   /* power off, restart, leave        TiVo: SC_Return */
    SFX_MOVEPAGE,   /* a page turn (no direction)       TiVo: pagedown */
    SFX_LONGTAP,    /* B: back                          TiVo: deselect */
    SFX_MEMOERASE,  /* sketchpad cleared                TiVo: thumbsdown */
    SFX_PENDOWN,    /* sketchpad / memo pen             TiVo: updown */
    SFX_UNDO,       /* sketchpad undo                   TiVo: slowdown1 */
    SFX_OPENING,
    SFX_STARTUP,    /* the program has started          TiVo: the TiVo Central chime */
    SFX_PAGEUP,     /* D-Pad Left: page up              TiVo: pageup */
    SFX_PAGEDOWN,   /* D-Pad Right: page down           TiVo: pagedown */
    SFX_UPDOWN,     /* D-Pad Up / Down: cursor moved    TiVo only */
    SFX_MENU,       /* the HOME menu opens              TiVo only: select */
    SFX_BONK,       /* that can't be done here, a list's end  TiVo only */
    SFX_ERROR,      /* an error message                 TiVo: bonk */
    SFX_DONE,       /* a success message                TiVo: tagup */
    SFX_YES,        /* A on a yes / no question         TiVo only: thumbsup */
    SFX_NO,         /* B on a yes / no question         TiVo only: thumbsdown */
    SFX_SPEED1,     /* the audio player scrubbing, faster and faster: TiVo only, speedup1 - 4 */
    SFX_SPEED2,
    SFX_SPEED3,
    SFX_SPEED4,
    SFX_SLOWDOWN,   /* back to playing after a scrub    TiVo only: slowdown1 */
    SFX_DIALOG,     /* a dialog pops up (a plain message)  TiVo: alert */
    SFX_ASK,        /* a yes / no question pops up      TiVo only: alert */
    SFX_COUNT
};

/* Starts ASND if nothing has yet. The audio player calls this where it used to call ASND_Init. */
void sfx_asnd_acquire(void);
void sfx_play(int id);
/* Plays even when the sounds are turned off: the alarm. */
void sfx_play_force(int id);
/* Looping white noise (the alarm's option); plays even when the sounds are off. */
void sfx_noise_start(void);
void sfx_noise_stop(void);
/* Waits for what is playing to end (a moment): before powering off. */
void sfx_wait(void);
/* Silences the sound that has just started (the click of the A that chose something slow). */
void sfx_cancel(void);
bool sfx_enabled(void);
void sfx_set_enabled(bool on);
/* sd:/apps/GodModeWii/sfx_off exists: the sounds were turned off last time;
 * sd:/apps/GodModeWii/tivo exists: TiVo mode is on. */
void sfx_load_setting(void);

/* TiVo mode: TiVo's sounds instead of MoonShell's. Saved on the card. */
bool sfx_tivo(void);
/* Switches TiVo mode over and plays its sound: the TiVo Central chime coming on, thumbs down going off.
 * Returns false if the setting could not be saved. */
bool sfx_tivo_toggle(void);

#endif

#ifndef WIILIGHT_H
#define WIILIGHT_H

/* The Wii's disc slot light, as in GlowWii: it is switched on and off about a hundred times a second
 * and the share of the time it's on is its brightness. A thread does it, so nothing here waits.
 * Nothing happens on Dolphin, a Wii U or when the light is turned off in the SYSTEM menu. */

#include <stdbool.h>

/* the patterns in wiilight_patterns.h that are used */
enum { WL_BUSH_WARBLER = 0, WL_DISC_OPEN = 2, WL_DISC_CLOSE = 3, WL_BLINK3 = 4 };

void wiilight_init(void);
void wiilight_shutdown(void);               /* light off, thread stopped */

void wiilight_off(void);
void wiilight_glow(int level);              /* steady, 0-255, until the next call */
void wiilight_pulse(void);                  /* one soft flash */
void wiilight_pattern(int id);              /* one of Nintendo's patterns, played once */
void wiilight_breathe(bool on);             /* slow breathing while something takes a while */

bool wiilight_enabled(void);
void wiilight_set_enabled(bool on);         /* saved on the card (sd:/apps/GodModeWii/light_off) */

#endif

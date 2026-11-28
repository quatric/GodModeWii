#ifndef INPUT_H
#define INPUT_H

#include <gccore.h>
#include <wiiuse/wpad.h>

/* All controllers are folded into WPAD_BUTTON_* bits (see input.c). */
#define IN_DOWN 0
#define IN_HELD 1

void input_init(void);
void input_shutdown(void);

/* Raw poll; waits for vsync like GlowWii's DetectInput(). */
u32 DetectInput(u8 down_or_held);

/* Edge-triggered presses plus auto-repeat on the D-Pad, for scrolling. */
u32 input_pressed(void);

/* Like DetectInput(IN_DOWN) but without waiting for vsync. */
u32 input_poll_nowait(void);

/* A bare poll (no vsync wait, no cursor, hint or alarm handling) for the thread that reads the
 * start-up code. */
u32 input_poll_raw(void);

/* Drops presses made while the program was busy (so they don't act on what comes next). */
void input_flush(void);

/* Non-blocking check used by long operations to allow cancelling. */
bool input_cancel_held(void);

/* The console's Power / Reset / Eject buttons as a controller (see input.c): mode 0 auto (no controller
 * connected), 1 always, 2 off; saved on the SD card. */
void input_console_load(void);
int  input_console_mode(void);
void input_console_set_mode(int mode);
bool input_console_nav(void);           /* Power / Reset are for the menus now, not for leaving */
void input_console_power(void);         /* from the power button callback */

#endif

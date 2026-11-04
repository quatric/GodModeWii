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

/* Drops presses made while the program was busy (so they don't act on what comes next). */
void input_flush(void);

/* Non-blocking check used by long operations to allow cancelling. */
bool input_cancel_held(void);

#endif

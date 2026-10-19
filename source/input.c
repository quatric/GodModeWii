#include <ogcsys.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "input.h"

/* Controllers, keyboard and the optional Wii U / DualShock 3 backends are
 * handled by libwiiinput; everything arrives as WPAD_BUTTON_* bits. */

void input_init(void)
{
    /* Buttons only: no IR pointer and no mouse needed for the file manager. */
    INPUT_InitEx(INPUT_INIT_NO_POINTER | INPUT_INIT_NO_MOUSE);

    INPUT_KeyboardMapSet(0x4C, 0);                  /* Delete: unused here */
    INPUT_KeyboardMapSet(0x06, WPAD_BUTTON_PLUS);   /* C: copy */
    INPUT_KeyboardMapSet(0x19, WPAD_BUTTON_MINUS);  /* V: cut  */
}

void input_shutdown(void)
{
    INPUT_Shutdown();
}

static u32 detect(u8 DownOrHeld, bool wait)
{
    if (wait)
        VIDEO_WaitVSync();

    INPUT_Update();
    return DownOrHeld == IN_DOWN ? INPUT_ButtonsDown() : INPUT_ButtonsHeld();
}

u32 DetectInput(u8 DownOrHeld)
{
    return detect(DownOrHeld, true);
}

u32 input_poll_nowait(void)
{
    return detect(IN_DOWN, false);
}

#define DPAD (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)

u32 input_pressed(void)
{
    static u32 repeat_mask;
    static int hold_frames;

    u32 down = DetectInput(IN_DOWN);
    u32 held = INPUT_ButtonsHeld() & DPAD;

    if (!held || (down & DPAD)) {
        hold_frames = 0;
        repeat_mask = held;
        return down;
    }

    hold_frames++;
    if (hold_frames > 22 && (hold_frames % 3) == 0)
        down |= held & repeat_mask;
    return down;
}

bool input_cancel_held(void)
{
    INPUT_Update();
    return (INPUT_ButtonsHeld() & WPAD_BUTTON_B) != 0;
}

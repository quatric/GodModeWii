#include <ogcsys.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "input.h"
#include "pointer.h"
#include "ui.h"
#include "sfx.h"
#include "wiilight.h"
#include "alarm.h"

/* Controllers, keyboard and the optional Wii U / DualShock 3 backends are
 * handled by libwiiinput; everything arrives as WPAD_BUTTON_* bits. */

void input_init(void)
{
    /* the pointer and mouse drive the viewers' cursor */
    INPUT_InitEx(0);
    ptr_begin();

    INPUT_KeyboardMapSet(0x4C, 0);                  /* Delete: unused here */
    INPUT_KeyboardMapSet(0x06, WPAD_BUTTON_PLUS);   /* C: copy */
    INPUT_KeyboardMapSet(0x19, WPAD_BUTTON_MINUS);  /* V: cut  */
}

void input_shutdown(void)
{
    wiilight_shutdown();
    INPUT_Shutdown();
}

static u32 detect(u8 DownOrHeld, bool wait)
{
    ptr_overlay_update();                           /* an arrow taken away by a redraw is back before the next frame */
    if (wait)
        VIDEO_WaitVSync();

    INPUT_Update();
    ui_hint_update();
    ptr_overlay_update();
    alarm_poll();
    if (DownOrHeld != IN_DOWN)
        return INPUT_ButtonsHeld();
    u32 down = INPUT_ButtonsDown();
    /* a click on a footer hint ("B Stop") is that button */
    u32 hint = ui_hint_under();
    if (hint && ((down & WPAD_BUTTON_A) || (INPUT_MouseButtonsPressed() & INPUT_MOUSE_LEFT)))
        down = (down & ~WPAD_BUTTON_A) | hint;
    return down;
}

u32 DetectInput(u8 DownOrHeld)
{
    return detect(DownOrHeld, true);
}

u32 input_poll_nowait(void)
{
    return detect(IN_DOWN, false);
}

/* For the start-up code reader, which runs in its own small thread: no cursor, hint or alarm work. */
u32 input_poll_raw(void)
{
    INPUT_Update();
    return INPUT_ButtonsDown();
}

#define DPAD (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)

u32 input_pressed(void)
{
    static u32 repeat_mask;
    static int hold_frames;

    u32 down = DetectInput(IN_DOWN);
    u32 held = INPUT_ButtonsHeld() & DPAD;
    if (down & WPAD_BUTTON_A) sfx_play(SFX_CLICK);
    else if (down & WPAD_BUTTON_B) sfx_play(SFX_LONGTAP);
    else if (down & WPAD_BUTTON_LEFT) sfx_play(SFX_PAGEUP);
    else if (down & WPAD_BUTTON_RIGHT) sfx_play(SFX_PAGEDOWN);
    else if (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) sfx_play(SFX_UPDOWN);   /* TiVo mode only */

    if (!held || (down & DPAD)) {
        hold_frames = 0;
        repeat_mask = held;
        return down;
    }

    hold_frames++;
    if (hold_frames > 22 && (hold_frames % 3) == 0) {
        down |= held & repeat_mask;
        if (down & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN)) sfx_play(SFX_UPDOWN);   /* a tick a step, like TiVo */
    }
    return down;
}

bool input_cancel_held(void)
{
    INPUT_Update();
    return (INPUT_ButtonsHeld() & WPAD_BUTTON_B) != 0;
}

void input_flush(void)
{
    for (int i = 0; i < 3; i++)
        detect(IN_DOWN, true);
}

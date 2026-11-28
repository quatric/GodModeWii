#include <ogcsys.h>
#include <stdio.h>
#include <ogc/lwp_watchdog.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "input.h"
#include "virt.h"
#include "pointer.h"
#include "ui.h"
#include "sfx.h"
#include "wiilight.h"
#include "alarm.h"

/* ---- the console's own buttons, for using GodModeWii without a controller ----
 * Power: down.  Reset: A (a tap), 2 (held about a second), HOME (held 2.5 s).  Power while Reset is held: B.
 * A disc ejected: HOME.  Auto: only while no controller, keyboard or mouse is there. */
#define CONBTN_SETTING "sd:/apps/GodModeWii/console_buttons"
static int conbtn_mode;                 /* 0 auto, 1 always, 2 off */
static volatile int power_presses;

void input_console_load(void)
{
    FILE *f = fopen(CONBTN_SETTING, "r");
    conbtn_mode = 0;
    if (f) { int m = fgetc(f) - '0'; if (m >= 0 && m <= 2) conbtn_mode = m; fclose(f); }
}

int input_console_mode(void) { return conbtn_mode; }

void input_console_set_mode(int m)
{
    conbtn_mode = m;
    FILE *f = fopen(CONBTN_SETTING, "w");
    if (f) { fputc('0' + m, f); fclose(f); }
}

bool input_console_nav(void)
{
    if (conbtn_mode) return conbtn_mode == 1;
    return INPUT_DeviceCount() == 0 && !INPUT_KeyboardConnected() && !INPUT_MouseConnected();
}

void input_console_power(void) { power_presses++; }

static u32 console_buttons(void)
{
    static u64 since;
    static bool was, used, home;
    u32 out = 0;
    bool r = SYS_ResetButtonDown() != 0;
    u64 now = ticks_to_millisecs(gettime());
    if (r && !was) { since = now; used = home = false; }
    if (r && !home && !used && now - since >= 2500) { out |= WPAD_BUTTON_HOME; home = true; }
    if (!r && was && !used && !home) out |= now - since < 700 ? WPAD_BUTTON_A : WPAD_BUTTON_2;
    was = r;
    if (power_presses > 0) {
        power_presses--;
        out |= r ? WPAD_BUTTON_B : WPAD_BUTTON_DOWN;
        if (r) used = true;             /* this Reset press was for B */
    }
    if (virt_disc_ejected()) out |= WPAD_BUTTON_HOME;
    return out;
}

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
    /* WPAD_Shutdown frees the Wii Remote state while the Bluetooth stack can
     * still deliver events for it, which crashes in libogc's __wpad_eventCB.
     * Disconnect the remotes first and let their events drain. */
    for (int ch = 0; ch < WPAD_MAX_WIIMOTES; ch++) WPAD_Disconnect(ch);
    usleep(300000);
    INPUT_Shutdown();
}

static u32 detect(u8 DownOrHeld, bool wait)
{
    ptr_overlay_update();                           /* an arrow taken away by a redraw is back before the next frame */
    if (wait)
        VIDEO_WaitVSync();
    ui_flip();

    INPUT_Update();
    virt_disc_idle();
    ui_hint_update();
    ptr_overlay_update();
    alarm_poll();
    if (DownOrHeld != IN_DOWN)
        return INPUT_ButtonsHeld();
    u32 down = INPUT_ButtonsDown();
    if (conbtn_mode != 2) down |= console_buttons();
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

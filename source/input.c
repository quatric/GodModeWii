#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ogcsys.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/usbkeyboard.h>

#include "input.h"

/* Controller handling adapted from GlowWii (source/main.c):
 * every device is mapped onto WPAD_BUTTON_* so the UI only sees one scheme. */

static u32 map_classic(u32 buttons)
{
    u32 out = 0;
    if (buttons & WPAD_CLASSIC_BUTTON_ZR)    out |= WPAD_BUTTON_PLUS;
    if (buttons & WPAD_CLASSIC_BUTTON_ZL)    out |= WPAD_BUTTON_MINUS;
    if (buttons & WPAD_CLASSIC_BUTTON_PLUS)  out |= WPAD_BUTTON_PLUS;
    if (buttons & WPAD_CLASSIC_BUTTON_MINUS) out |= WPAD_BUTTON_MINUS;
    if (buttons & WPAD_CLASSIC_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (buttons & WPAD_CLASSIC_BUTTON_B)     out |= WPAD_BUTTON_B;
    if (buttons & WPAD_CLASSIC_BUTTON_X)     out |= WPAD_BUTTON_2;
    if (buttons & WPAD_CLASSIC_BUTTON_Y)     out |= WPAD_BUTTON_1;
    if (buttons & WPAD_CLASSIC_BUTTON_HOME)  out |= WPAD_BUTTON_HOME;
    if (buttons & WPAD_CLASSIC_BUTTON_UP)    out |= WPAD_BUTTON_UP;
    if (buttons & WPAD_CLASSIC_BUTTON_DOWN)  out |= WPAD_BUTTON_DOWN;
    if (buttons & WPAD_CLASSIC_BUTTON_LEFT)  out |= WPAD_BUTTON_LEFT;
    if (buttons & WPAD_CLASSIC_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}

/* USB keyboard support */
static lwp_t kbd_thread_hndl = LWP_THREAD_NULL;
static volatile bool kbd_thread_should_run = false;
static u32 kbd_buttons;

// from Priiloader (/tools/Dacoslove/source/Input.cpp)
static void KBEventHandler(USBKeyboard_event event)
{
    if (event.type != USBKEYBOARD_PRESSED && event.type != USBKEYBOARD_RELEASED)
        return;

    u32 button = 0;

    switch (event.keyCode)
    {
    case 0x52: button = WPAD_BUTTON_UP; break;
    case 0x51: button = WPAD_BUTTON_DOWN; break;
    case 0x50: button = WPAD_BUTTON_LEFT; break;
    case 0x4F: button = WPAD_BUTTON_RIGHT; break;
    case 0x28: /* Enter */
    case 0x58: button = WPAD_BUTTON_A; break;
    case 0x2A: button = WPAD_BUTTON_B; break;   /* Backspace */
    case 0x1B: button = WPAD_BUTTON_1; break;   /* X: mark */
    case 0x1C: button = WPAD_BUTTON_2; break;   /* Y: menu */
    case 0x06: button = WPAD_BUTTON_PLUS; break;  /* C: copy */
    case 0x19: button = WPAD_BUTTON_MINUS; break; /* V: cut  */
    case 0x29: /* ESC */
    case 0x4A: button = WPAD_BUTTON_HOME; break;
    default: break;
    }

    if (event.type == USBKEYBOARD_PRESSED)
        kbd_buttons |= button;
    else
        kbd_buttons &= ~button;
}

static void *kbd_thread(void *userp)
{
    while (kbd_thread_should_run)
    {
        if (!USBKeyboard_IsConnected())
            USBKeyboard_Open(KBEventHandler);

        USBKeyboard_Scan();
        usleep(4000);
    }
    return NULL;
}

void input_init(void)
{
    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS);

    if (USBKeyboard_Initialize() == 0) {
        kbd_thread_should_run = true;
        LWP_CreateThread(&kbd_thread_hndl, kbd_thread, NULL, NULL, 0, 64);
    }
}

void input_shutdown(void)
{
    kbd_thread_should_run = false;
    if (kbd_thread_hndl != LWP_THREAD_NULL) {
        LWP_JoinThread(kbd_thread_hndl, NULL);
        kbd_thread_hndl = LWP_THREAD_NULL;
    }
    USBKeyboard_Close();
    USBKeyboard_Deinitialize();
}

static u32 detect(u8 DownOrHeld, bool wait)
{
    const int down = DownOrHeld == IN_DOWN;
    u32 pressed = 0, buttons = 0, gcpressed = 0;
    if (wait)
        VIDEO_WaitVSync();

    if (WPAD_ScanPads() > WPAD_ERR_NONE) {
        for (int chan = 0; chan < 4; chan++)
            buttons |= down ? WPAD_ButtonsDown(chan) : WPAD_ButtonsHeld(chan);
        pressed |= buttons | map_classic(buttons);
    }

    if (PAD_ScanPads() > PAD_ERR_NONE) {
        for (int chan = 0; chan < 4; chan++)
            gcpressed |= down ? PAD_ButtonsDown(chan) : PAD_ButtonsHeld(chan);
        if (gcpressed & PAD_TRIGGER_R)   pressed |= WPAD_BUTTON_PLUS;
        if (gcpressed & PAD_TRIGGER_L)   pressed |= WPAD_BUTTON_MINUS;
        if (gcpressed & PAD_BUTTON_A)    pressed |= WPAD_BUTTON_A;
        if (gcpressed & PAD_BUTTON_B)    pressed |= WPAD_BUTTON_B;
        if (gcpressed & PAD_BUTTON_X)    pressed |= WPAD_BUTTON_2;
        if (gcpressed & PAD_BUTTON_Y)    pressed |= WPAD_BUTTON_1;
        if (gcpressed & PAD_BUTTON_MENU) pressed |= WPAD_BUTTON_HOME;
        if (gcpressed & PAD_BUTTON_UP)    pressed |= WPAD_BUTTON_UP;
        if (gcpressed & PAD_BUTTON_DOWN)  pressed |= WPAD_BUTTON_DOWN;
        if (gcpressed & PAD_BUTTON_LEFT)  pressed |= WPAD_BUTTON_LEFT;
        if (gcpressed & PAD_BUTTON_RIGHT) pressed |= WPAD_BUTTON_RIGHT;
    }

    /* kbd_buttons is consumed so a key press fires once (edge-triggered). */
    pressed |= kbd_buttons;
    kbd_buttons = 0;

    return pressed;
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
    u32 held = 0;

    /* DetectInput waits for vsync; the second call returns immediately-ish. */
    if (WPAD_ScanPads() > WPAD_ERR_NONE)
        for (int chan = 0; chan < 4; chan++) {
            u32 b = WPAD_ButtonsHeld(chan);
            held |= b | map_classic(b);
        }
    if (PAD_ScanPads() > PAD_ERR_NONE)
        for (int chan = 0; chan < 4; chan++) {
            u32 g = PAD_ButtonsHeld(chan);
            if (g & PAD_BUTTON_UP)    held |= WPAD_BUTTON_UP;
            if (g & PAD_BUTTON_DOWN)  held |= WPAD_BUTTON_DOWN;
            if (g & PAD_BUTTON_LEFT)  held |= WPAD_BUTTON_LEFT;
            if (g & PAD_BUTTON_RIGHT) held |= WPAD_BUTTON_RIGHT;
        }

    held &= DPAD;
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
    u32 held = 0;
    if (WPAD_ScanPads() > WPAD_ERR_NONE)
        for (int chan = 0; chan < 4; chan++) {
            u32 b = WPAD_ButtonsHeld(chan);
            held |= b | map_classic(b);
        }
    if (PAD_ScanPads() > PAD_ERR_NONE)
        for (int chan = 0; chan < 4; chan++)
            if (PAD_ButtonsHeld(chan) & PAD_BUTTON_B)
                held |= WPAD_BUTTON_B;
    return (held & WPAD_BUTTON_B) != 0;
}

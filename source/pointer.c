#include <stdlib.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "pointer.h"
#include "ui.h"

#define AW 12
#define AH 16
/* X outline (black), # fill (white). */
static const char *const ARROW[AH] = {
    "X           ", "XX          ", "X#X         ", "X##X        ", "X###X       ", "X####X      ",
    "X#####X     ", "X######X    ", "X#######X   ", "X########X  ", "X#####XXXXX ", "X##X##X     ",
    "X#X X##X    ", "XX  X##X    ", "X    X##X   ", "     XXXX   ",
};

#define SW 8                                    /* saved words per row: 16 pixels from an even x */
static u32 saved[AH][SW];
/* where the saved block is (sx < 0: nothing saved) */
static int sx = -1, sy, sw_n, sh_n;

void ptr_begin(void)
{
    int w, h;
    ui_framebuffer(&w, &h);
    INPUT_SetScreenSize(w, h);
    INPUT_SetCursorMode(INPUT_CURSOR_IR | INPUT_CURSOR_MOUSE);
    sx = -1;
}

bool ptr_get(int *x, int *y)
{
    if (!INPUT_CursorActive()) return false;
    INPUT_CursorPosition(x, y);
    return true;
}

void ptr_hide(void)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb || sx < 0) return;
    for (int r = 0; r < sh_n; r++) {
        u32 *row = (u32 *)(fb + (sy + r) * fw * 2) + sx / 2;
        for (int k = 0; k < sw_n; k++) row[k] = saved[r][k];
    }
    sx = -1;
}

void ptr_show(void)
{
    int fw, fh, x, y;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb) return;
    bool on = ptr_get(&x, &y);
    if (on && sx >= 0 && x == sx + (x & 1) && y == sy) return;      /* hasn't moved */

    ptr_hide();
    if (!on || x < 0 || y < 0 || x >= fw || y >= fh) return;

    sx = x & ~1;
    sy = y;
    sw_n = (fw - sx) / 2 < SW ? (fw - sx) / 2 : SW;
    sh_n = fh - sy < AH ? fh - sy : AH;

    for (int r = 0; r < sh_n; r++) {
        /* The framebuffer is uncached and takes whole words:
         * each pixel pair is read, changed, written. */
        u32 *row = (u32 *)(fb + (sy + r) * fw * 2) + sx / 2;
        for (int k = 0; k < sw_n; k++) saved[r][k] = row[k];
        for (int k = 0; k < sw_n; k++) {
            u32 w = row[k];
            bool hit = false;
            for (int half = 0; half < 2; half++) {
                int c = k * 2 + half - (x - sx);
                char ch = c >= 0 && c < AW ? ARROW[r][c] : ' ';
                if (ch == ' ') continue;
                u32 yv = ch == 'X' ? 16 : 235, sh = half ? 8 : 24;
                w = (w & ~(0xFFu << sh)) | yv << sh;
                hit = true;
            }

            if (hit) w = (w & 0xFF00FF00u) | 0x00800080u;      /* grey: no colour in the pair */
            row[k] = w;
        }
    }
}

int ptr_drag_update(ptr_drag *d, u32 held, int *dx, int *dy)
{
    int x, y;
    bool on = ptr_get(&x, &y), a = (held & WPAD_BUTTON_A) || (INPUT_MouseButtonsHeld() & 1);
    *dx = *dy = 0;
    if (a && !d->down) {                        /* pressed */
        d->down = true;
        d->dragged = false;
        d->ptr_press = on;
        d->x0 = d->lx = x;
        d->y0 = d->ly = y;
        return PD_NONE;
    }

    if (!a) {
        bool was = d->down && d->ptr_press && !d->dragged;
        d->down = d->ptr_press = false;
        return was ? PD_TAP : PD_NONE;
    }

    if (!d->ptr_press || !on) return PD_NONE;   /* pointed off the screen mid-drag: wait for it */
    /* Pressing A wobbles the remote by more than IR jitter; under this it is still a tap. */
    if (!d->dragged && abs(x - d->x0) + abs(y - d->y0) < 16) return PD_NONE;
    if (!d->dragged) {
        d->lx = d->x0;
        d->ly = d->y0;
    }

    d->dragged = true;
    *dx = x - d->lx;
    *dy = y - d->ly;
    d->lx = x;
    d->ly = y;
    return *dx || *dy ? PD_MOVE : PD_NONE;
}

int ptr_wheel(void)
{
    int dx, dy, w;
    INPUT_MouseDelta(&dx, &dy, &w);
    return w;
}

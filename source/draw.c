/* Sketchpad: draw with the Wii Remote pointer, a USB mouse, a pen tablet or a uDraw. The palette,
 * brush sizes and tools (draw, fill, erase, undo, clear) follow PictoCard's doodle pad. The picture
 * is kept as 0x00RRGGBB and painted into the console framebuffer (YUY2); saved as a 24-bit .bmp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>
#include <input_tablet.h>

#include "pda.h"
#include "input.h"
#include "pointer.h"
#include "sfx.h"
#include "wiilight.h"
#include "ui.h"
#include "vfs.h"

#define CW 576                      /* the canvas */
#define CH 304
#define MAX_UNDO 6
#define FILL_TOLERANCE 2000         /* squared RGB distance */

enum { TOOL_DRAW, TOOL_FILL, TOOL_ERASE, TOOL_COUNT };
enum { CMD_SIZE0, CMD_SIZE1, CMD_SIZE2, CMD_SIZE3, CMD_DRAW, CMD_FILL, CMD_ERASE, CMD_UNDO,
       CMD_CLEAR, CMD_SAVE, CMD_QUIT, CMD_COUNT };

static const u32 COLORS[16] = {
    0x000000, 0x555555, 0xaaaaaa, 0xffffff, 0x800000, 0xff0000, 0xff9900, 0xffff00,
    0x800080, 0x004d00, 0x00ff00, 0x000080, 0x00ffff, 0xff69b4, 0xff00ff, 0x8b4513,
};
static const int SIZES[4] = { 2, 5, 10, 20 };

static struct { const char *label; int col, w, cmd; } items[CMD_COUNT] = {
    { "2", 0, 0, CMD_SIZE0 },   { "5", 0, 0, CMD_SIZE1 },   { "10", 0, 0, CMD_SIZE2 },
    { "20", 0, 0, CMD_SIZE3 },  { "Draw", 0, 0, CMD_DRAW }, { "Fill", 0, 0, CMD_FILL },
    { "Erase", 0, 0, CMD_ERASE }, { "Undo", 0, 0, CMD_UNDO }, { "Clear", 0, 0, CMD_CLEAR },
    { "Save", 0, 0, CMD_SAVE }, { "Quit", 0, 0, CMD_QUIT },
};
#define TOOLBAR_ROW 4
#define PALETTE_Y 52                /* swatches: 32 x 24 every 36 pixels */

static u32 *cv;                     /* the picture */
static u32 *undo_buf[MAX_UNDO];
static int undo_n;
static int color, size = 1, tool;
static bool dirty;                  /* changed since saved (or opened) */
static int cx0, cy0;                /* where the canvas sits on the screen */
static int dx0, dy0, dx1, dy1;      /* to repaint: dirty rectangle in canvas pixels */
static char status_line[80];
static char last_saved[64];

/* ---------------------------------------------------------------- painting */

static void rgb_yuv(int r, int g, int b, int *y, int *u, int *v)
{
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

static void mark(int x0, int y0, int x1, int y1)
{
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

static void mark_all(void) { dx0 = 0; dy0 = 0; dx1 = CW; dy1 = CH; }
static void mark_none(void) { dx0 = CW; dy0 = CH; dx1 = 0; dy1 = 0; }

/* Canvas rectangle [x0, x1) x [y0, y1) into the framebuffer, a pixel pair at a time. */
static void paint(int x0, int y0, int x1, int y1)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CW) x1 = CW;
    if (y1 > CH) y1 = CH;
    x0 &= ~1;
    x1 = (x1 + 1) & ~1;
    if (x0 >= x1 || y0 >= y1) return;
    ptr_hide();
    for (int y = y0; y < y1; y++) {
        u32 *row = (u32 *)(fb + (cy0 + y) * fw * 2) + (cx0 + x0) / 2;
        const u32 *src = cv + y * CW + x0;
        for (int x = x0; x < x1; x += 2, src += 2, row++) {
            int r0 = src[0] >> 16 & 255, g0 = src[0] >> 8 & 255, b0 = src[0] & 255;
            int r1 = src[1] >> 16 & 255, g1 = src[1] >> 8 & 255, b1 = src[1] & 255;
            int y0v, u0, v0, y1v, u1, v1;
            rgb_yuv(r0, g0, b0, &y0v, &u0, &v0);
            rgb_yuv(r1, g1, b1, &y1v, &u1, &v1);
            *row = (u32)y0v << 24 | (u32)((u0 + u1) >> 1) << 16 | (u32)y1v << 8 | (u32)((v0 + v1) >> 1);
        }
    }
}

static void flush(void)
{
    if (dx0 < dx1 && dy0 < dy1) paint(dx0, dy0, dx1, dy1);
    mark_none();
}

/* A solid rectangle on the screen (x and w even), outside the canvas. */
static void fill_rect(int x, int y, int w, int h, u32 rgb)
{
    int fw, fh;
    u8 *fb = ui_framebuffer(&fw, &fh);
    if (!fb) return;
    int yy, u, v;
    rgb_yuv(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, &yy, &u, &v);
    u32 word = (u32)yy << 24 | (u32)u << 16 | (u32)yy << 8 | (u32)v;
    ptr_hide();
    for (int r = 0; r < h && y + r < fh; r++) {
        u32 *row = (u32 *)(fb + (y + r) * fw * 2) + x / 2;
        for (int i = 0; i < w / 2; i++) row[i] = word;
    }
}

static void swatch(int i)
{
    int x = cx0 + i * 36;
    fill_rect(x, PALETTE_Y - 2, 36, 28, i == color && tool != TOOL_ERASE ? 0xffffff : 0x000000);
    fill_rect(x + 2, PALETTE_Y, 32, 24, COLORS[i]);
}

/* ---------------------------------------------------------------- the picture */

static void blank(void)
{
    for (int i = 0; i < CW * CH; i++) cv[i] = 0xFFFFFF;
    mark_all();
}

static void push_undo(void)
{
    u32 *copy = malloc(sizeof(u32) * CW * CH);
    if (!copy) return;
    memcpy(copy, cv, sizeof(u32) * CW * CH);
    if (undo_n == MAX_UNDO) {
        free(undo_buf[0]);
        memmove(&undo_buf[0], &undo_buf[1], sizeof(u32 *) * (MAX_UNDO - 1));
        undo_n--;
    }
    undo_buf[undo_n++] = copy;
}

static void undo(void)
{
    if (!undo_n) return;
    u32 *prev = undo_buf[--undo_n];
    memcpy(cv, prev, sizeof(u32) * CW * CH);
    free(prev);
    mark_all();
    sfx_play(SFX_UNDO);
}

static u32 ink(void) { return tool == TOOL_ERASE ? 0xFFFFFF : COLORS[color]; }

static void disc(int cx, int cy, int diameter, u32 c)
{
    int r = diameter / 2;
    int r2 = r * r + (diameter > 2 ? r / 2 : 0);
    if (diameter <= 2) r = 1;
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= CH) continue;
        for (int x = -r; x <= r; x++) {
            int px = cx + x;
            if (px < 0 || px >= CW) continue;
            if (diameter <= 2 || x * x + y * y <= r2) cv[py * CW + px] = c;
        }
    }
    mark(cx - r, cy - r, cx + r + 1, cy + r + 1);
}

static void line(int x0, int y0, int x1, int y1, int diameter, u32 c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 2;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) disc(x0 + dx * i / steps, y0 + dy * i / steps, diameter, c);
}

static void flood_fill(int sx, int sy)
{
    if (sx < 0 || sy < 0 || sx >= CW || sy >= CH) return;
    u32 fill = COLORS[color], target = cv[sy * CW + sx];
    if (target == fill) return;
    int tr = target >> 16 & 255, tg = target >> 8 & 255, tb = target & 255;
    u8 *seen = calloc(CW * CH, 1);
    int *stack = malloc(sizeof(int) * CW * CH);
    if (!seen || !stack) {
        free(seen);
        free(stack);
        return;
    }
    int top = 0;
    stack[top++] = sy * CW + sx;
    while (top) {
        int pos = stack[--top];
        if (seen[pos]) continue;
        seen[pos] = 1;
        u32 p = cv[pos];
        int dr = (int)(p >> 16 & 255) - tr, dg = (int)(p >> 8 & 255) - tg, db = (int)(p & 255) - tb;
        if (dr * dr + dg * dg + db * db > FILL_TOLERANCE) continue;
        cv[pos] = fill;
        int x = pos % CW;
        if (x > 0) stack[top++] = pos - 1;
        if (x < CW - 1) stack[top++] = pos + 1;
        if (pos >= CW) stack[top++] = pos - CW;
        if (pos < CW * (CH - 1)) stack[top++] = pos + CW;
    }
    free(seen);
    free(stack);
    mark_all();
}

/* 24-bit bottom-up .bmp, the next free drawing_NNN.bmp. */
static bool save(char *name, size_t n)
{
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX + 32];
    if (!pda_dir("drawings", dir, sizeof(dir))) return false;
    for (int i = 1; i < 1000; i++) {
        snprintf(name, n, "drawing_%03d.bmp", i);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (access(path, F_OK) != 0) break;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    u32 bytes = CW * CH * 3;
    u8 h[54] = { 'B', 'M' };
    u32 total = 54 + bytes, off = 54, dib = 40, planes_bpp = 1 | 24 << 16;
    /* little-endian fields, written byte by byte: this CPU is big-endian */
    for (int k = 0; k < 4; k++) h[2 + k] = total >> (8 * k);
    for (int k = 0; k < 4; k++) h[10 + k] = off >> (8 * k);
    for (int k = 0; k < 4; k++) h[14 + k] = dib >> (8 * k);
    for (int k = 0; k < 4; k++) h[18 + k] = (u32)CW >> (8 * k);
    for (int k = 0; k < 4; k++) h[22 + k] = (u32)CH >> (8 * k);
    for (int k = 0; k < 4; k++) h[26 + k] = planes_bpp >> (8 * k);
    for (int k = 0; k < 4; k++) h[34 + k] = bytes >> (8 * k);
    for (int k = 0; k < 4; k++) h[38 + k] = 2835 >> (8 * k);      /* 72 dpi */
    for (int k = 0; k < 4; k++) h[42 + k] = 2835 >> (8 * k);
    bool ok = fwrite(h, 1, 54, f) == 54;
    u8 row[CW * 3];
    for (int y = CH - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < CW; x++) {
            u32 p = cv[y * CW + x];
            row[x * 3] = p & 255;
            row[x * 3 + 1] = p >> 8 & 255;
            row[x * 3 + 2] = p >> 16 & 255;
        }
        ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) unlink(path);
    return ok;
}

/* ---------------------------------------------------------------- screen */

static void say(const char *s)
{
    snprintf(status_line, sizeof(status_line), "%s", s);
    ui_color(C_GREEN, true);
    ui_field(ui_rows() - 1, 1, ui_cols() - 2, "%s", status_line);
    ui_reset();
}

static void layout_items(void)
{
    int col = 6;
    for (int i = 0; i < CMD_COUNT; i++) {
        items[i].w = (int)strlen(items[i].label) + 2;
        items[i].col = col;
        col += items[i].w + (i == CMD_SIZE3 ? 3 : 1);
    }
}

static void toolbar(int hover)
{
    ui_color(C_WHITE, false);
    ui_field(TOOLBAR_ROW, 0, 6, "Size");
    for (int i = 0; i < CMD_COUNT; i++) {
        bool on = (i <= CMD_SIZE3 && i - CMD_SIZE0 == size) ||
                  (i >= CMD_DRAW && i <= CMD_ERASE && i - CMD_DRAW == tool);
        if (on) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else if (i == hover) {
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
        } else {
            ui_color(C_WHITE, true);
        }
        ui_field(TOOLBAR_ROW, items[i].col, items[i].w, " %s", items[i].label);
        ui_reset();
    }
}

static void chrome(void)
{
    ui_clear();
    ui_color(C_CYAN, true);
    ui_field(0, 1, ui_cols() - 2, "Sketchpad");
    ui_color(C_WHITE, false);
    ui_field(1, 1, ui_cols() - 2, "Point and hold A to draw. Pick a colour, a size or a tool above.");
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, ui_cols() - 2, "A Draw  B Quit  1 Undo  2 Colour  - Size  + Save");
    ui_reset();
    for (int i = 0; i < 16; i++) swatch(i);
    toolbar(-1);
    mark_all();
    flush();
    if (status_line[0]) say(status_line);
}

static int toolbar_hit(void)
{
    int r, c;
    if (!ptr_cell(&r, &c) || r != TOOLBAR_ROW) return -1;
    for (int i = 0; i < CMD_COUNT; i++)
        if (c >= items[i].col && c < items[i].col + items[i].w) return i;
    return -1;
}

static int palette_hit(int x, int y)
{
    int i = (x - cx0) / 36;
    if (x < cx0 || i > 15 || y < PALETTE_Y - 2 || y >= PALETTE_Y + 26) return -1;
    return i;
}

static bool quit_ok(void)
{
    if (!dirty) return true;
    bool ok = ui_confirm("SKETCHPAD", C_YELLOW, "Leave without saving this drawing?");
    chrome();
    return ok;
}

static void set_color(int i)
{
    int old = color;
    color = i;
    if (tool == TOOL_ERASE) tool = TOOL_DRAW;
    swatch(old);
    swatch(i);
}

static bool command(int cmd)
{
    char name[64], msg[96];
    switch (cmd) {
    case CMD_SIZE0: case CMD_SIZE1: case CMD_SIZE2: case CMD_SIZE3:
        size = cmd - CMD_SIZE0;
        sfx_play(SFX_CLICK);
        break;
    case CMD_DRAW: case CMD_FILL: case CMD_ERASE:
        tool = cmd - CMD_DRAW;
        swatch(color);
        sfx_play(SFX_CLICK);
        break;
    case CMD_UNDO:
        undo();
        break;
    case CMD_CLEAR:
        push_undo();
        blank();
        dirty = true;
        sfx_play(SFX_MEMOERASE);
        break;
    case CMD_SAVE:
        if (save(name, sizeof(name))) {
            dirty = false;
            snprintf(last_saved, sizeof(last_saved), "%s", name);
            snprintf(msg, sizeof(msg), "Saved GodModeWii/drawings/%s", name);
            say(msg);
            sfx_play(SFX_NOTIFY);
            wiilight_pattern(WL_BUSH_WARBLER);
        } else {
            ui_message("SKETCHPAD", C_RED, "Could not save: is a drive with room for it inserted?");
            chrome();
        }
        break;
    case CMD_QUIT:
        return quit_ok();
    }
    toolbar(-1);
    return false;
}

void draw_run(void)
{
    int fw, fh;
    ui_framebuffer(&fw, &fh);
    cx0 = (fw - CW) / 2 & ~1;
    cy0 = 112;
    cv = malloc(sizeof(u32) * CW * CH);
    if (!cv) {
        ui_message("SKETCHPAD", C_RED, "Not enough memory.");
        return;
    }
    undo_n = 0;
    color = 0;
    tool = TOOL_DRAW;
    dirty = false;
    status_line[0] = '\0';
    mark_none();
    layout_items();
    blank();

    INPUT_SetCursorMode(INPUT_CURSOR_IR | INPUT_CURSOR_MOUSE | INPUT_CURSOR_STICK);
    TB_Init();
    chrome();
    sfx_play(SFX_OPEN);

    bool was_a = false, stroke = false, tb_down = false;
    int lx = 0, ly = 0, tlx = 0, tly = 0, hover = -1;

    for (;;) {
        u32 p = DetectInput(IN_DOWN);
        u32 h = INPUT_ButtonsHeld();
        int x, y;
        bool on = ptr_get(&x, &y);
        bool a = (h & WPAD_BUTTON_A) || (INPUT_MouseButtonsHeld() & INPUT_MOUSE_LEFT);
        bool pressed = a && !was_a;
        was_a = a;
        int px = x - cx0, py = y - cy0;
        bool on_canvas = on && px >= 0 && py >= 0 && px < CW && py < CH;

        int hv = on ? toolbar_hit() : -1;
        if (hv != hover) {
            hover = hv;
            toolbar(hover);
        }

        if (p & WPAD_BUTTON_HOME) p |= WPAD_BUTTON_B;
        if (p & WPAD_BUTTON_B) {
            if (quit_ok()) break;
            continue;
        }
        if (p & WPAD_BUTTON_1) undo();
        if (p & WPAD_BUTTON_2) { set_color((color + 1) % 16); sfx_play(SFX_CLICK); }
        if (p & WPAD_BUTTON_MINUS) { size = (size + 1) % 4; toolbar(hover); sfx_play(SFX_CLICK); }
        if (p & WPAD_BUTTON_PLUS) {
            if (command(CMD_SAVE)) break;
        }

        if (pressed && on) {
            int pi = palette_hit(x, y);
            if (on_canvas) {
                push_undo();
                dirty = true;
                if (tool == TOOL_FILL) {
                    flood_fill(px, py);
                    sfx_play(SFX_PENDOWN);
                } else {
                    stroke = true;
                    lx = px;
                    ly = py;
                    disc(px, py, SIZES[size], ink());
                    sfx_play(SFX_PENDOWN);
                }
            } else if (pi >= 0) {
                set_color(pi);
                sfx_play(SFX_CLICK);
            } else if (hover >= 0) {
                if (command(items[hover].cmd)) break;
                hover = -1;
            }
        } else if (a && stroke && on) {
            int nx = px < 0 ? 0 : px >= CW ? CW - 1 : px;
            int ny = py < 0 ? 0 : py >= CH ? CH - 1 : py;
            line(lx, ly, nx, ny, SIZES[size], ink());
            lx = nx;
            ly = ny;
        }
        if (!a) stroke = false;

        /* a pen tablet or uDraw: every report since the last frame, pressure sets the width */
        tb_sample s;
        while (TB_Pop(&s)) {
            bool tip = s.in_range && (s.buttons & TB_BUTTON_TIP);
            if (!tip) {
                tb_down = false;
                continue;
            }
            int tx, ty;
            TB_ToScreen(&s, CW, CH, &tx, &ty);
            int d = (int)(SIZES[size] * (0.4f + 1.2f * s.pressure));
            if (d < 2) d = 2;
            u32 c = s.eraser ? 0xFFFFFF : ink();
            if (!tb_down) {
                push_undo();
                dirty = true;
                tb_down = true;
                disc(tx, ty, d, c);
            } else {
                line(tlx, tly, tx, ty, d, c);
            }
            tlx = tx;
            tly = ty;
        }

        flush();
    }

    TB_Shutdown();
    ptr_begin();                                    /* the cursor modes of the other screens */
    for (int i = 0; i < undo_n; i++) free(undo_buf[i]);
    undo_n = 0;
    free(cv);
    cv = NULL;
    ui_clear();
}

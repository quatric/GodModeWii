#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>

#include "ui.h"
#include "input.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static int g_cols = 76, g_rows = 28;

void ui_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
    console_init(xfb, 16, 16, rmode->fbWidth - 32, rmode->xfbHeight - 32,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    int c, r;
    CON_GetMetrics(&c, &r);
    if (c > 0) g_cols = c;
    if (r > 0) g_rows = r;
}

int ui_cols(void) { return g_cols; }
int ui_rows(void) { return g_rows; }


/* ANSI cursor addressing is 1-based; callers use 0-based cells. */
void ui_goto(int row, int col) { printf("\x1b[%d;%dH", row + 1, col + 1); }

/* The "0" in "\x1b[30;0m" clears every attribute including the background,
 * so the current background is tracked and re-applied after each colour. */
static int cur_bg = -1;

void ui_clear(void)
{
    cur_bg = -1;
    printf("\x1b[0m\x1b[2J");
}

void ui_bg(int bg)
{
    cur_bg = bg;
    printf("\x1b[%dm", 40 + bg);
}

void ui_color(int fg, bool bold)
{
    printf("\x1b[%d;%dm", 30 + fg, bold ? 1 : 0);
    if (cur_bg >= 0)
        printf("\x1b[%dm", 40 + cur_bg);
}

void ui_reset(void)
{
    cur_bg = -1;
    printf("\x1b[0m\x1b[37;0m\x1b[40m");
}

void ui_field(int row, int col, int width, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (width >= (int)sizeof(buf))
        width = sizeof(buf) - 1;
    int len = strlen(buf);
    for (; len < width; len++)
        buf[len] = ' ';
    buf[width] = '\0';
    ui_goto(row, col);
    fputs(buf, stdout);
}

void ui_fill(int row, int col, int width, char c)
{
    char buf[128];
    if (width > 127) width = 127;
    memset(buf, c, width);
    buf[width] = '\0';
    ui_goto(row, col);
    fputs(buf, stdout);
}

void ui_bar(int row, int col, int width, int pct, int color)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int inner = width - 2;
    int fill = inner * pct / 100;
    char buf[128];
    if (inner > 120) inner = 120;
    buf[0] = '[';
    for (int i = 0; i < inner; i++)
        buf[1 + i] = i < fill ? '#' : '-';
    buf[1 + inner] = ']';
    buf[2 + inner] = '\0';
    ui_goto(row, col);
    ui_color(C_WHITE, false);
    putchar('[');
    ui_color(color, true);
    buf[1 + fill] = '\0';
    fputs(buf + 1, stdout);
    ui_color(C_WHITE, false);
    for (int i = fill; i < inner; i++)
        putchar('-');
    ui_color(C_WHITE, false);
    putchar(']');
}

void ui_fmt_size(char *out, size_t n, u64 bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%u B", (unsigned)bytes);
    else
        snprintf(out, n, "%.*f %s", v >= 100 ? 0 : (v >= 10 ? 1 : 2), v, units[u]);
}

/* ---------------------------------------------------------------- boxes */

#define BOX_W_MAX 70

static int wrap_lines(const char *text, int width, char lines[][BOX_W_MAX + 2], int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        /* explicit newline ends the line */
        int len = 0, last_space = -1;
        while (p[len] && p[len] != '\n' && len < width) {
            if (p[len] == ' ')
                last_space = len;
            len++;
        }
        int take = len;
        if (p[len] && p[len] != '\n' && last_space > 0)
            take = last_space;
        memcpy(lines[n], p, take);
        lines[n][take] = '\0';
        n++;
        p += take;
        if (*p == '\n' || *p == ' ')
            p++;
    }
    return n;
}

/* Draws a framed box sized for `nlines` of text; returns first content row. */
static int draw_box(const char *title, int color, int nlines, int extra, int w, int *out_left)
{
    int h = nlines + extra + 4;
    int top = (g_rows - h) / 2;
    int left = (g_cols - w) / 2;
    if (top < 0) top = 0;
    if (left < 0) left = 0;

    ui_reset();
    ui_color(color, true);
    ui_goto(top, left);
    putchar('+');
    for (int i = 0; i < w - 2; i++) putchar('-');
    putchar('+');
    for (int r = 1; r < h - 1; r++) {
        ui_goto(top + r, left);
        putchar('|');
        for (int i = 0; i < w - 2; i++) putchar(' ');
        putchar('|');
    }
    ui_goto(top + h - 1, left);
    putchar('+');
    for (int i = 0; i < w - 2; i++) putchar('-');
    putchar('+');

    if (title && *title) {
        ui_goto(top, left + 2);
        ui_color(C_WHITE, true);
        printf(" %s ", title);
    }
    ui_reset();
    *out_left = left;
    return top + 2;
}

static int box_width(void)
{
    int w = g_cols - 8;
    return w > BOX_W_MAX ? BOX_W_MAX : w;
}

static void draw_text_box(const char *title, int color, const char *text,
                          const char *footer, int extra, int *content_row, int *left)
{
    int w = box_width();
    char lines[16][BOX_W_MAX + 2];
    int n = wrap_lines(text, w - 4, lines, 16);
    int row = draw_box(title, color, n, extra + (footer ? 2 : 0), w, left);
    for (int i = 0; i < n; i++) {
        ui_goto(row + i, *left + 2);
        ui_color(C_WHITE, false);
        fputs(lines[i], stdout);
    }
    if (footer) {
        ui_goto(row + n + extra + 1, *left + 2);
        ui_color(C_CYAN, true);
        fputs(footer, stdout);
    }
    ui_reset();
    if (content_row)
        *content_row = row + n;
}

void ui_message(const char *title, int color, const char *text)
{
    int left;
    draw_text_box(title, color, text, "Press A to continue", 0, NULL, &left);
    for (;;) {
        u32 p = DetectInput(IN_DOWN);
        if (p & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            return;
    }
}

bool ui_confirm(const char *title, int color, const char *text)
{
    int left;
    draw_text_box(title, color, text, "A: Yes    B: No", 0, NULL, &left);
    for (;;) {
        u32 p = DetectInput(IN_DOWN);
        if (p & WPAD_BUTTON_A) return true;
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) return false;
    }
}

bool ui_hold_confirm(const char *title, const char *text, int seconds)
{
    int left, crow;
    char foot[64];
    snprintf(foot, sizeof(foot), "HOLD A for %d seconds to continue - B cancels", seconds);
    draw_text_box(title, C_RED, text, foot, 1, &crow, &left);
    int need = seconds * 60, held = 0;
    int w = box_width();
    for (;;) {
        u32 p = DetectInput(IN_HELD);
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            return false;
        if (p & WPAD_BUTTON_A) {
            if (++held >= need)
                return true;
        } else if (held > 0) {
            held = 0;
        }
        ui_bar(crow, left + 2, w - 4, held * 100 / need, C_RED);
    }
}

int ui_menu(const char *title, const char **items, const bool *enabled, int n, int start)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        if ((int)strlen(items[i]) > w) w = strlen(items[i]);
    w += 8;
    if (w < 28) w = 28;
    if (w > g_cols - 4) w = g_cols - 4;
    int sel = start;
    if (sel < 0 || sel >= n) sel = 0;
    int max_rows = g_rows - 8;
    int view = n < max_rows ? n : max_rows;
    int scroll = 0;
    int left, row;
    bool first = true;

    for (;;) {
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + view) scroll = sel - view + 1;
        if (first) {
            row = draw_box(title, C_CYAN, view, 2, w, &left);
            first = false;
        }
        for (int i = 0; i < view; i++) {
            int idx = scroll + i;
            ui_goto(row + i, left + 1);
            if (idx == sel) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
            else if (enabled && !enabled[idx]) ui_color(C_WHITE, false);
            else ui_color(C_WHITE, true);
            char line[96];
            snprintf(line, sizeof(line), " %c %s", idx == sel ? '>' : ' ', items[idx]);
            int len = strlen(line);
            for (int k = len; k < w - 2; k++) line[k] = ' ';
            line[w - 2] = '\0';
            fputs(line, stdout);
            ui_reset();
        }
        ui_goto(row + view + 1, left + 2);
        ui_color(C_CYAN, true);
        fputs("A: Select   B: Back", stdout);
        ui_reset();

        u32 p = input_pressed();
        if (p & WPAD_BUTTON_UP)   sel = (sel + n - 1) % n;
        if (p & WPAD_BUTTON_DOWN) sel = (sel + 1) % n;
        if (p & WPAD_BUTTON_A)    return sel;
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) return -1;
    }
}

/* Character roller: the Wii has no keyboard, so names are spun like a
 * slot machine. Up/Down changes the char, Left/Right moves, 1 deletes. */
static const char CHARSET[] =
    " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    ".-_()[]+,!#&'@~";

bool ui_text_input(const char *title, char *buf, int maxlen)
{
    int w = box_width();
    int len = strlen(buf);
    int pos = len ? len - 1 : 0;
    int left;
    char hint[2][BOX_W_MAX + 2];
    (void)hint;
    int row = draw_box(title, C_CYAN, 3, 3, w, &left);
    ui_goto(row + 4, left + 2);
    ui_color(C_CYAN, true);
    fputs("U/D: char  L/R: move  +/-: jump  1: delete  2: space", stdout);
    ui_goto(row + 5, left + 2);
    fputs("A: OK    B: Cancel", stdout);
    ui_reset();

    for (;;) {
        int vis = w - 6;
        int off = pos >= vis ? pos - vis + 1 : 0;
        ui_goto(row + 1, left + 2);
        for (int i = 0; i < vis; i++) {
            int idx = off + i;
            char c = idx < len ? buf[idx] : ' ';
            if (idx == pos) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
            else { ui_color(C_YELLOW, true); }
            putchar(c);
            ui_reset();
        }
        ui_goto(row + 2, left + 2);
        ui_color(C_WHITE, false);
        printf("%d/%d chars  ", len, maxlen);
        ui_reset();

        u32 p = input_pressed();
        int cs = sizeof(CHARSET) - 1;
        int step = 0;
        if (p & WPAD_BUTTON_UP) step = 1;
        if (p & WPAD_BUTTON_DOWN) step = -1;
        if (p & WPAD_BUTTON_PLUS) step = 10;
        if (p & WPAD_BUTTON_MINUS) step = -10;
        if (step) {
            if (pos >= len) {
                if (len >= maxlen) continue;
                buf[len++] = 'a';
                buf[len] = '\0';
            }
            const char *f = strchr(CHARSET, buf[pos]);
            int idx = f ? (int)(f - CHARSET) : 0;
            idx = ((idx + step) % cs + cs) % cs;
            buf[pos] = CHARSET[idx];
        }
        if (p & WPAD_BUTTON_RIGHT) {
            if (pos < len - 1) pos++;
            else if (len < maxlen && pos == len - 1) { buf[len++] = 'a'; buf[len] = '\0'; pos++; }
        }
        if (p & WPAD_BUTTON_LEFT) { if (pos > 0) pos--; }
        if (p & WPAD_BUTTON_2) {
            if (len < maxlen) {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos] = ' ';
                len++;
            }
        }
        if (p & WPAD_BUTTON_1) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos);
                len--;
                if (pos >= len && pos > 0) pos--;
            }
        }
        if (p & WPAD_BUTTON_A) {
            while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
            return len > 0;
        }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            return false;
    }
}

void ui_splash(const char *status)
{
    ui_clear();
    int r = g_rows / 2 - 6;
    static const char *art[] = {
        "  ____           _ __  __           _      __        ___ _ ",
        " / ___| ___   __| |  \\/  | ___   __| | ___ \\ \\      / (_|_)",
        "| |  _ / _ \\ / _` | |\\/| |/ _ \\ / _` |/ _ \\ \\ \\ /\\ / /| | |",
        "| |_| | (_) | (_| | |  | | (_) | (_| |  __/  \\ V  V / | | |",
        " \\____|\\___/ \\__,_|_|  |_|\\___/ \\__,_|\\___|   \\_/\\_/  |_|_|",
    };
    for (int i = 0; i < 5; i++) {
        ui_goto(r + i, (g_cols - 61) / 2);
        ui_color(i < 3 ? C_CYAN : C_BLUE, true);
        fputs(art[i], stdout);
    }
    ui_reset();
    ui_field(r + 7, 0, g_cols - 1, "%*s%s", (g_cols - (int)strlen(status)) / 2, "", status);
}

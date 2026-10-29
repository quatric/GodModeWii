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

/* The console font is ASCII only: accented Latin letters lose their accents, curly quotes and
 * dashes go plain, and anything else is one '?'. Never longer than src, so dst may be src. */
static const char *fold(u32 cp, char *one)
{
    static const char latin1[] = "AAAAAA?CEEEEIIII" "DNOOOOOxOUUUUY??" "aaaaaa?ceeeeiiii" "dnooooo/ouuuuy?y";
    static const char ext_a[] = "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "Ii??JjKkkLlLlLlL"
                                "lLlNnNnNnnNnOoOo" "Oo??RrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    switch (cp) {
    case 0xA0: return " ";
    case 0xC6: return "AE";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE6: return "ae";
    case 0xFE: return "th";
    case 0x132: return "IJ";
    case 0x133: return "ij";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x2013: case 0x2014: return "-";
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2026: return "...";
    }
    one[0] = cp >= 0xC0 && cp <= 0xFF ? latin1[cp - 0xC0] : cp >= 0x100 && cp <= 0x17F ? ext_a[cp - 0x100] : '?';
    one[1] = '\0';
    return one;
}

void ui_ascii(char *dst, size_t n, const char *src)
{
    const u8 *s = (const u8 *)src;
    size_t k = 0;
    while (*s && k + 1 < n) {
        char one[2] = { (char)*s, '\0' };
        const char *out = one;
        u32 cp = *s++;
        if (cp >= 0x80) {
            int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
            bool bad = cp < 0xC0;
            cp &= 0x3F >> more;
            for (; more && (*s & 0xC0) == 0x80; more--)
                cp = cp << 6 | (*s++ & 0x3F);
            out = bad || more ? "?" : fold(cp, one);
        }
        for (; *out && k + 1 < n; out++)
            dst[k++] = *out;
    }
    if (n)
        dst[k] = '\0';
}

void ui_field(int row, int col, int width, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_ascii(buf, sizeof(buf), buf);

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

/* Dialogs are drawn as solid colour windows (no ASCII frames): a coloured
 * title bar, a blue body and a dark button strip at the bottom. */
#define WIN_BG C_BLUE

/* A problem summary turns its whole window red so it can't be missed. */
static int win_body = WIN_BG;

void ui_alert_window(bool on)
{
    win_body = on ? C_RED : WIN_BG;
}

static int title_fg(int bar)
{
    return (bar == C_RED || bar == C_BLUE || bar == C_MAGENTA) ? C_WHITE : C_BLACK;
}

static void win_blank(int row, int left, int w, int bg)
{
    ui_bg(bg);
    ui_goto(row, left);
    for (int i = 0; i < w; i++)
        putchar(' ');
}

/* Body text inside the window, padded to the window width. */
static void win_text(int row, int left, int w, int fg, bool bold, const char *s)
{
    ui_bg(win_body);
    ui_color(fg, bold);
    ui_field(row, left + 2, w - 4, "%s", s);
}

static void win_strip(int row, int left, int w, const char *text)
{
    win_blank(row, left, w, C_CYAN);
    ui_color(C_BLACK, false);
    ui_field(row, left + 2, w - 4, "%s", text);
    ui_reset();
}

/* Draws title bar + body + button strip; returns the first content row. */
static int draw_window(const char *title, int color, int content_rows, int w,
                       const char *strip, int *out_left, int *out_strip_row)
{
    int h = content_rows + 4;                 /* title, pad, content, pad, strip */
    int top = (g_rows - h) / 2;
    int left = (g_cols - w) / 2;
    if (top < 0) top = 0;
    if (left < 0) left = 0;

    win_blank(top, left, w, color);
    ui_color(title_fg(color), true);
    ui_field(top, left + 2, w - 4, "%s", title ? title : "");
    for (int r = 1; r < h - 1; r++)
        win_blank(top + r, left, w, win_body);
    win_strip(top + h - 1, left, w, strip);
    ui_reset();
    *out_left = left;
    if (out_strip_row)
        *out_strip_row = top + h - 1;
    return top + 2;
}

static int box_width(void)
{
    int w = g_cols - 8;
    return w > BOX_W_MAX ? BOX_W_MAX : w;
}

static void draw_text_box(const char *title, int color, const char *text,
                          const char *strip, int extra, int *content_row, int *left)
{
    int w = box_width();
    char lines[16][BOX_W_MAX + 2];
    int n = wrap_lines(text, w - 4, lines, 16);
    int row = draw_window(title, color, n + extra, w, strip, left, NULL);
    for (int i = 0; i < n; i++)
        win_text(row + i, *left, w, C_WHITE, true, lines[i]);
    ui_reset();
    if (content_row)
        *content_row = row + n;
}

void ui_message(const char *title, int color, const char *text)
{
    int left;
    draw_text_box(title, color, text, "A  OK", 0, NULL, &left);
    for (;;) {
        u32 p = DetectInput(IN_DOWN);
        if (p & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            return;
    }
}

void ui_message_pages(const char *title, int color, const char *const *pages, int n)
{
    int i = 0;
    for (;;) {
        char strip[64], head[96];
        int left;
        snprintf(head, sizeof(head), "%s  (%d/%d)", title, i + 1, n);
        snprintf(strip, sizeof(strip), "L / R  Prev / Next    A  OK");
        draw_text_box(head, color, pages[i], strip, 0, NULL, &left);
        for (;;) {
            u32 p = DetectInput(IN_DOWN);
            if (p & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME))
                return;
            if (p & WPAD_BUTTON_PLUS) { i = (i + 1) % n; break; }
            if (p & WPAD_BUTTON_MINUS) { i = (i + n - 1) % n; break; }
        }
    }
}

bool ui_confirm(const char *title, int color, const char *text)
{
    int left;
    draw_text_box(title, color, text, "A  Yes        B  No", 0, NULL, &left);
    for (;;) {
        u32 p = DetectInput(IN_DOWN);
        if (p & WPAD_BUTTON_A) return true;
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) return false;
    }
}

int ui_menu(const char *title, const char **items, const bool *enabled, int n, int start)
{
    return ui_menu_ex(title, items, enabled, n, start, "A  Select        B  Back");
}

int ui_menu_ex(const char *title, const char **items, const bool *enabled, int n, int start, const char *strip)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        if ((int)strlen(items[i]) > w) w = strlen(items[i]);
    w += 10;
    if (w < 34) w = 34;
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
            row = draw_window(title, C_CYAN, view, w, strip, &left, NULL);
            first = false;
        }
        for (int i = 0; i < view; i++) {
            int idx = scroll + i;
            bool on = idx == sel;
            ui_bg(on ? C_CYAN : WIN_BG);
            if (on)                          ui_color(C_BLACK, false);
            else if (enabled && !enabled[idx]) ui_color(C_WHITE, false);
            else                             ui_color(C_WHITE, true);
            ui_field(row + i, left + 1, w - 2, "  %s", items[idx]);
        }
        ui_reset();

        u32 p = input_pressed();
        if (p & WPAD_BUTTON_UP)   sel = (sel + n - 1) % n;
        if (p & WPAD_BUTTON_DOWN) sel = (sel + 1) % n;
        if (p & WPAD_BUTTON_A)    return sel;
        if (p & WPAD_BUTTON_B)    return -1;
        if (p & WPAD_BUTTON_HOME) return UI_MENU_HOME;
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
    int left, strip_row;
    int row = draw_window(title, C_CYAN, 4, w, "A  OK     B  Cancel", &left, &strip_row);
    win_text(row + 2, left, w, C_CYAN, true, "U/D char  L/R move  +/- jump  1 del  2 space");

    for (;;) {
        int vis = w - 6;
        int off = pos >= vis ? pos - vis + 1 : 0;
        ui_goto(row, left + 2);
        for (int i = 0; i < vis; i++) {
            int idx = off + i;
            char c = idx < len ? buf[idx] : ' ';
            if (idx == pos) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
            else            { ui_bg(C_BLACK); ui_color(C_YELLOW, true); }
            putchar(c);
        }
        ui_bg(WIN_BG);
        ui_color(C_WHITE, false);
        ui_field(row + 1, left + 2, w - 4, "%d / %d characters", len, maxlen);
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

/* ---------------------------------------------------------------- splash */

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
        ui_color(C_CYAN, true);          /* one solid colour for the whole logo */
        fputs(art[i], stdout);
    }
    ui_reset();
    ui_field(r + 7, 0, g_cols - 1, "%*s%s", (g_cols - (int)strlen(status)) / 2, "", status);
}

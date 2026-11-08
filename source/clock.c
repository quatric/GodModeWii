#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wiiuse/wpad.h>

#include "clock.h"
#include "alarm.h"
#include "colors.h"
#include "input.h"
#include "pointer.h"
#include "ui.h"

/* 5 x 7 digits */
static const unsigned char GLYPH[11][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 },            /* the colon */
};

static void rgb_yuv(unsigned rgb, int *y, int *u, int *v)
{
    int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

/* A solid block on the screen: x and w even. */
static void block(int x, int y, int w, int h, unsigned rgb)
{
    int fw, fh;
    unsigned char *fb = ui_framebuffer(&fw, &fh);
    if (!fb) return;
    int yy, u, v;
    rgb_yuv(rgb, &yy, &u, &v);
    unsigned word = (unsigned)yy << 24 | (unsigned)u << 16 | (unsigned)yy << 8 | (unsigned)v;
    for (int r = 0; r < h && y + r < fh; r++) {
        unsigned *row = (unsigned *)(fb + (y + r) * fw * 2) + x / 2;
        for (int i = 0; i < w / 2; i++) row[i] = word;
    }
}

/* One glyph, each dot scale x scale pixels (scale even). */
static void glyph(int x, int y, int g, int scale, unsigned fg, unsigned bg)
{
    block(x, y, 5 * scale, 7 * scale, bg);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (GLYPH[g][r] & (0x10 >> c)) block(x + c * scale, y + r * scale, scale, scale, fg);
}

static int days_in(int year, int mon)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
    return d[mon];
}

static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTH[12] = { "January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December" };

#define CAL_ROW 10

static void draw_calendar(int year, int mon, const struct tm *now)
{
    int cols = ui_cols();
    char title[40];
    snprintf(title, sizeof(title), "<  %s %d  >", MONTH[mon], year);
    int tl = (int)strlen(title), left = (cols - 28) / 2;
    ui_color(C_CYAN, true);
    ui_field(CAL_ROW, left, 28, "%*s%s", (28 - tl) / 2, "", title);
    for (int d = 0; d < 7; d++) {
        ui_color(d == 0 ? C_RED : d == 6 ? C_CYAN : C_WHITE, true);
        ui_field(CAL_ROW + 1, left + d * 4, 4, " %s", WDAY[d]);
    }
    ui_reset();

    struct tm first = { .tm_year = year - 1900, .tm_mon = mon, .tm_mday = 1, .tm_hour = 12 };
    mktime(&first);
    int start = first.tm_wday, n = days_in(year, mon), day = 1;
    for (int wk = 0; wk < 6; wk++) {
        for (int d = 0; d < 7; d++) {
            int col = left + d * 4;
            if ((wk == 0 && d < start) || day > n) {
                ui_field(CAL_ROW + 2 + wk, col, 4, "");
                continue;
            }
            bool today = now && year == now->tm_year + 1900 && mon == now->tm_mon && day == now->tm_mday;
            if (today) {
                ui_bg(C_CYAN);
                ui_color(C_BLACK, false);
            } else {
                ui_color(d == 0 ? C_RED : d == 6 ? C_CYAN : C_WHITE, d == 0 || d == 6);
            }
            ui_field(CAL_ROW + 2 + wk, col, 4, " %2d ", day);
            ui_reset();
            day++;
        }
    }
}

static void draw_time(const struct tm *t, bool full)
{
    int fw, fh;
    ui_framebuffer(&fw, &fh);
    unsigned fg = colors_fg_rgb(), bg = colors_bg_rgb();
    int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
    const int S = 8, G = 8, SS = 4;
    int big = 5 * 5 * S + 4 * G;                    /* five 40-pixel glyphs (the colon too) and four gaps */
    int x0 = ((fw - (big + 2 * S + 3 * 5 * SS + 2 * 4)) / 2) & ~1, y = 64;
    static int shown_min = -1;
    int min_key = t->tm_hour * 60 + t->tm_min;
    ptr_hide();
    if (full || min_key != shown_min) {
        shown_min = min_key;
        int x = x0;
        block(x, y, 5 * S, 7 * S, bg);              /* the leading digit is blank before 10 */
        if (h12 >= 10) glyph(x, y, h12 / 10, S, fg, bg);
        x += 5 * S + G;
        glyph(x, y, h12 % 10, S, fg, bg);
        x += 5 * S + G;
        glyph(x, y, 10, S, fg, bg);                 /* the colon */
        x += 5 * S + G;
        glyph(x, y, t->tm_min / 10, S, fg, bg);
        x += 5 * S + G;
        glyph(x, y, t->tm_min % 10, S, fg, bg);
        ui_color(C_WHITE, true);
        ui_field((y - 16) / 16 + 4, (x0 + big + 2 * S) / 8, 3, "%s", t->tm_hour < 12 ? "AM" : "PM");
        ui_reset();
    }
    /* the seconds, small, beside it: ":SS" */
    int sx = (x0 + big + 2 * S) & ~1, sy = y;
    block(sx, sy, 3 * 5 * SS + 8, 7 * SS, bg);
    glyph(sx, sy, 10, SS, fg, bg);
    glyph(sx + 5 * SS + 4, sy, t->tm_sec / 10, SS, fg, bg);
    glyph(sx + 2 * (5 * SS + 4), sy, t->tm_sec % 10, SS, fg, bg);
}

void clock_run(void)
{
    int shown_sec = -1, view_year = 0, view_mon = 0;
    bool full = true;
    ui_clear();
    for (;;) {
        time_t tt = time(NULL);
        struct tm now;
        bool ok = tt >= 1000000000 && localtime_r(&tt, &now);
        if (!ok) memset(&now, 0, sizeof(now)), now.tm_year = 100, now.tm_mday = 1;

        if (full) {
            ui_clear();
            ui_color(C_CYAN, true);
            ui_field(0, 1, ui_cols() - 2, "Clock");
            ui_reset();
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 2, 1, ui_cols() - 2, "Left/Right Month  A Today  B Back");
            ui_reset();
            if (!view_year) {
                view_year = now.tm_year + 1900;
                view_mon = now.tm_mon;
            }
            char date[48];
            snprintf(date, sizeof(date), "%s.  %d/%d/%d", WDAY[now.tm_wday], now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
            ui_color(C_WHITE, true);
            ui_field(8, (ui_cols() - (int)strlen(date)) / 2, (int)strlen(date), "%s", date);
            ui_reset();
            draw_calendar(view_year, view_mon, ok ? &now : NULL);
            shown_sec = -1;
        }
        if (now.tm_sec != shown_sec || full) {
            shown_sec = now.tm_sec;
            draw_time(&now, full);
        }
        full = false;

        u32 p = input_pressed() | ptr_ui_buttons();
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (p & WPAD_BUTTON_LEFT) {
            if (--view_mon < 0) { view_mon = 11; view_year--; }
            draw_calendar(view_year, view_mon, ok ? &now : NULL);
        }
        if (p & WPAD_BUTTON_RIGHT) {
            if (++view_mon > 11) { view_mon = 0; view_year++; }
            draw_calendar(view_year, view_mon, ok ? &now : NULL);
        }
        if (p & WPAD_BUTTON_A) {
            view_year = now.tm_year + 1900;
            view_mon = now.tm_mon;
            draw_calendar(view_year, view_mon, ok ? &now : NULL);
        }
    }
    ui_clear();
}

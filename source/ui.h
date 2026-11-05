#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <gccore.h>

enum { C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_WHITE };

void ui_init(void);
int  ui_cols(void);
int  ui_rows(void);
/* The framebuffer line where text row `row` starts. */
int  ui_row_y(int row);

void ui_clear(void);
/* Clears the whole screen, the border around the text area too: after a viewer drew over it. */
void ui_wipe(void);
void ui_goto(int row, int col);
void ui_color(int fg, bool bold);
void ui_bg(int bg);
void ui_reset(void);

/* Prints exactly `width` columns at row/col (truncated or padded). */
void ui_ascii(char *dst, size_t n, const char *src);
void ui_field(int row, int col, int width, const char *fmt, ...);
void ui_fill(int row, int col, int width, char c);
void ui_bar(int row, int col, int width, int pct, int color);
void ui_fmt_size(char *out, size_t n, u64 bytes);

/* Dialogs. All of them block and leave the screen for the caller to redraw. */
void ui_message(const char *title, int color, const char *text);
/* Several pages of text; L / R (or - / +) flip between them, A closes. */
void ui_message_pages(const char *title, int color, const char *const *pages, int n);
/* While on, dialog windows get a red body instead of blue (problem summaries). */
void ui_alert_window(bool on);
bool ui_confirm(const char *title, int color, const char *text);
#define UI_MENU_HOME (-2)      /* HOME pressed inside a menu (B / cancel is -1) */
int  ui_menu(const char *title, const char **items, const bool *enabled, int n, int start);
int  ui_menu_ex(const char *title, const char **items, const bool *enabled, int n, int start,
                const char *strip);
bool ui_text_input(const char *title, char *buf, int maxlen);
/* Like ui_text_input, but any printable ASCII and an empty result is allowed (editing file
 * lines). */
bool ui_text_edit(const char *title, char *buf, int maxlen);

void ui_splash(const char *status);

/* A loading box with an old-style text progress bar, for loads that take a while. Between begin and
 * end, ticks (from file reads) draw it once the load has run for a moment and animate it; progress
 * shows a percentage instead of the moving block. Nested begins keep the outer label. The screen
 * under the box is left for the caller to redraw. */
void ui_busy_begin(const char *what);
void ui_busy_end(void);
void ui_busy_tick(void);
void ui_busy_progress(u64 done, u64 total);

/* The console's framebuffer (YUY2, 2 bytes per pixel), for the image viewer. */
void *ui_framebuffer(int *w, int *h);

#ifdef GMW_AUTOTEST
/* Test builds only: saves the framebuffer as sd:/shot_N.yuv (YUY2) and says the viewer should
 * close. */
bool ui_test_dump(void);
#define UI_TEST_SHOT() if (ui_test_dump()) break
#else
#define UI_TEST_SHOT() do { } while (0)
#endif

#endif

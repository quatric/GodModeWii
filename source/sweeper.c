#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "sweeper.h"
#include "ui.h"
#include "input.h"
#include "pointer.h"
#include "sfx.h"

#define CONSOLE "Wii"
#define FLAG_KEY "1"                /* the buttons as this console's controllers label them */
#define OPEN_KEY "2"

/* ---------------------------------------------------------------- the game */

#define N 16
#define MINES 40
#define CW 3                                        /* columns a square */

enum { HIDDEN, SHOWN, FLAGGED };

static u8 mine[N][N];
static u8 state[N][N], near_[N][N];
static int cx, cy, x0, y0, shown, flags;
static bool placed, over;
static u64 started, ended;

static int count_near(int x, int y)
{
    int c = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx || dy) && nx >= 0 && ny >= 0 && nx < N && ny < N && mine[ny][nx]) c++;
        }
    return c;
}

static void new_board(void)
{
    memset(mine, 0, sizeof(mine));
    memset(state, HIDDEN, sizeof(state));
    cx = cy = N / 2;
    shown = flags = 0;
    placed = over = false;
    started = ended = 0;
}

/* The bad blocks go in after the first dig, away from it, so the first dig is always safe. */
static void place_mines(int fx, int fy)
{
    srand((unsigned)gettime());
    int want = MINES, guard = 0;
    while (want > 0 && guard++ < 100000) {
        int x = rand() % N, y = rand() % N;
        if (mine[y][x] || (abs(x - fx) <= 1 && abs(y - fy) <= 1)) continue;
        mine[y][x] = 1;
        want--;
    }
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) near_[y][x] = (u8)count_near(x, y);
    placed = true;
    started = gettime();
}

static int total_mines(void) { return MINES; }

static void draw_cell(int x, int y)
{
    static const int num_color[9] = { C_WHITE, C_CYAN, C_GREEN, C_RED, C_BLUE, C_YELLOW, C_MAGENTA, C_WHITE, C_WHITE };
    bool cur = x == cx && y == cy && !over;
    char t[CW + 1] = "   ";
    int fg = C_WHITE, bg = C_BLUE;
    bool bold = true;
    if (state[y][x] == FLAGGED) {
        t[1] = 'F';
        fg = C_RED;
        if (over && !mine[y][x]) { t[1] = 'x'; fg = C_YELLOW; }   /* flagged wrongly */
    } else if (state[y][x] == SHOWN || (over && mine[y][x])) {
        bg = C_BLACK;
        if (mine[y][x]) {
            t[1] = '*';
            fg = C_RED;
        } else if (near_[y][x]) {
            t[1] = (char)('0' + near_[y][x]);
            fg = num_color[near_[y][x]];
        }
    } else {
        t[1] = '.';
        fg = C_WHITE;
        bold = false;
    }
    if (cur) { bg = C_CYAN; fg = fg == C_CYAN ? C_BLUE : C_BLACK; bold = false; }
    ui_bg(bg);
    ui_color(fg, bold);
    ui_goto(y0 + y, x0 + x * CW);
    printf("%s", t);
}

static void draw_status(void)
{
    u64 now = over ? ended : gettime();
    unsigned secs = placed ? (unsigned)(ticks_to_millisecs(now - started) / 1000) : 0;
    ui_reset();
    ui_field(y0 - 2, x0, N * CW, "Bad blocks: %-3d  Flags: %-3d  Time: %u s", total_mines(), flags, secs);
}

static void draw_all(void)
{
    int cols = ui_cols(), rows = ui_rows();
    ui_clear();
    ui_bg(C_RED);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols, " BAD BLOCK SWEEPER");
    ui_reset();
    x0 = (cols - N * CW) / 2;
    y0 = 4;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) draw_cell(x, y);
    ui_reset();
    draw_status();
    ui_color(C_WHITE, false);
    ui_field(y0 + N + 1, x0, N * CW, "* a bad block (not a real one: see the note)");
    ui_bg(C_CYAN);
    ui_color(C_BLACK, false);
    ui_field(rows - 2, 0, cols, " A Dig   " FLAG_KEY " Flag   " OPEN_KEY " Dig around   + New game   B Quit");
    ui_reset();
}

static void reveal(int x, int y)
{
    /* flood fill without recursion: a small stack of squares to open */
    static u8 stack[N * N][2];
    int sp = 0;
    stack[sp][0] = (u8)x, stack[sp][1] = (u8)y, sp++;
    while (sp) {
        sp--;
        int px = stack[sp][0], py = stack[sp][1];
        if (state[py][px] != HIDDEN) continue;
        state[py][px] = SHOWN;
        shown++;
        draw_cell(px, py);
        if (near_[py][px] || mine[py][px]) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = px + dx, ny = py + dy;
                if (nx >= 0 && ny >= 0 && nx < N && ny < N && state[ny][nx] == HIDDEN && sp < N * N)
                    stack[sp][0] = (u8)nx, stack[sp][1] = (u8)ny, sp++;
            }
    }
}

static void lose(int x, int y)
{
    static const char *const quips[] = {
        "Your data stepped right on it. Don't worry: like every bad block here, it was imaginary. Your real NAND "
        "didn't feel a thing.",
        "Bad block! Somewhere, a save file shudders... then remembers this is a game and none of these blocks are real.",
        "Kaboom. That block was about as bad as a block can be, which is not at all, because it isn't real.",
    };
    over = true;
    ended = gettime();
    for (int yy = 0; yy < N; yy++)
        for (int xx = 0; xx < N; xx++) draw_cell(xx, yy);
    draw_status();
    ui_message("BAD BLOCK!", C_RED, quips[(x * 7 + y * 3 + (int)(ended & 0xff)) % 3]);
}

static void check_win(void)
{
    if (over || shown + total_mines() != N * N) return;
    over = true;
    ended = gettime();
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            if (mine[y][x] && state[y][x] != FLAGGED) { state[y][x] = FLAGGED; flags++; }
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) draw_cell(x, y);
    draw_status();
    char msg[200];
    snprintf(msg, sizeof(msg), "You steered clear of every bad block in %u seconds. The " CONSOLE " thanks you!",
             (unsigned)(ticks_to_millisecs(ended - started) / 1000));
    ui_message("NAND SAVED!", C_GREEN, msg);
}

static void dig(int x, int y)
{
    if (over || state[y][x] == FLAGGED) return;
    if (!placed) place_mines(x, y);
    if (state[y][x] == SHOWN) {                     /* a number with its flags all down: open the rest */
        int f = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < N && ny < N && state[ny][nx] == FLAGGED) f++;
            }
        if (!near_[y][x] || f != near_[y][x]) return;
        for (int dy = -1; dy <= 1 && !over; dy++)
            for (int dx = -1; dx <= 1 && !over; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= N || ny >= N || state[ny][nx] != HIDDEN) continue;
                if (mine[ny][nx]) lose(nx, ny);
                else reveal(nx, ny);
            }
        check_win();
        return;
    }
    if (mine[y][x]) { lose(x, y); return; }
    reveal(x, y);
    check_win();
}

#ifdef GMW_AUTOTEST
/* "@sweeper": a board after a first dig, then with everything shown (saved like a viewer's screen). */
void sweeper_autotest(void)
{
    extern bool ui_test_dump(void);
    new_board();
    dig(2, 2);
    draw_all();
    ui_test_dump();
    over = true;
    ended = gettime();
    draw_all();
    ui_test_dump();
}
#endif

void sweeper_run(void)
{
    new_board();
    ui_message("BAD BLOCK SWEEPER", C_RED,
               "Oh no! Your " CONSOLE " has too many bad blocks in its NAND! Please help us steer clear of them.\n\n"
               "(A note from the NAND: relax, none of these bad blocks are real. They're scattered at random each "
               "game, nothing on your " CONSOLE " is read or touched, and your actual blocks are doing just fine. "
               "Probably.)");

    draw_all();
    unsigned last_secs = ~0u;
    int last_r = -1, last_c = -1;
    for (;;) {
        u32 p = input_pressed() | ptr_ui_buttons();
        int pr, pc, ok = ptr_cell(&pr, &pc);
        int hx = ok ? (pc - x0) / CW : -1, hy = ok ? pr - y0 : -1;
        bool on_board = ok && pc >= x0 && pr >= y0 && hx < N && hy < N;
        int ox = cx, oy = cy;
        if (ok && (pr != last_r || pc != last_c)) {
            last_r = pr;
            last_c = pc;
            if (on_board) { cx = hx; cy = hy; }
        }
        if (p & WPAD_BUTTON_UP)    cy = (cy + N - 1) % N;
        if (p & WPAD_BUTTON_DOWN)  cy = (cy + 1) % N;
        if (p & WPAD_BUTTON_LEFT)  cx = (cx + N - 1) % N;
        if (p & WPAD_BUTTON_RIGHT) cx = (cx + 1) % N;
        if (ox != cx || oy != cy) { draw_cell(ox, oy); draw_cell(cx, cy); }

        bool redraw = false;
        if (p & WPAD_BUTTON_A) {
            bool was_over = over;
            dig(cx, cy);
            redraw = over || was_over;              /* a dialog went over the board */
        }
        if ((p & WPAD_BUTTON_2) && !over && state[cy][cx] == SHOWN) {
            dig(cx, cy);
            redraw = over;
        }
        if ((p & WPAD_BUTTON_1) && !over && state[cy][cx] != SHOWN) {
            state[cy][cx] = state[cy][cx] == FLAGGED ? HIDDEN : FLAGGED;
            flags += state[cy][cx] == FLAGGED ? 1 : -1;
            sfx_play(SFX_PENDOWN);
            draw_cell(cx, cy);
            draw_status();
        }
        if (p & WPAD_BUTTON_PLUS) { new_board(); redraw = true; }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (redraw) draw_all();
        unsigned secs = placed && !over ? (unsigned)(ticks_to_millisecs(gettime() - started) / 1000) : 0;
        if (secs != last_secs) { last_secs = secs; draw_status(); }
        ui_reset();
    }
    ui_clear();
}

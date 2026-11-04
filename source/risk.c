#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

#include "risk.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"

static bool g_unlocked;
static bool g_forced;       /* persistent "always unlocked" switch */

bool risk_unlocked(void) { return g_unlocked || g_forced; }
bool risk_forced(void) { return g_forced; }
void risk_lock(void) { g_unlocked = false; }

static const u32 KONAMI[10] = {
    WPAD_BUTTON_UP, WPAD_BUTTON_UP, WPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN,
    WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT,
    WPAD_BUTTON_B, WPAD_BUTTON_A,
};
static const char *const KONAMI_GLYPH[10] = { "UP",    "UP",   "DOWN",  "DOWN", "LEFT",
                                              "RIGHT", "LEFT", "RIGHT", "B",    "A" };


/* One step of the code. Tolerant by design: buttons outside the code are ignored, several buttons in
 * one frame count as the expected one if it is among them, and after a wrong press the longest part of
 * the entered buttons that still starts the code is kept (UP UP UP DOWN DOWN ... still works). */
#define KONAMI_MASK (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_A | WPAD_BUTTON_B)

static int konami_step(int progress, u32 p)
{
    p &= KONAMI_MASK;
    if (!p)
        return progress;
    if (p & KONAMI[progress])
        return progress + 1;
    /* wrong button: find the longest suffix of (entered buttons + p) that is a prefix of the code */
    u32 seq[11];
    for (int i = 0; i < progress; i++)
        seq[i] = KONAMI[i];
    seq[progress] = p;
    for (int len = progress; len > 0; len--) {
        bool ok = true;
        for (int i = 0; i < len && ok; i++)
            ok = (seq[progress + 1 - len + i] & KONAMI[i]) != 0;
        if (ok)
            return len;
    }
    return 0;
}

/* Full-screen warning, then the Konami code. `lines` are the warning
 * text, `title` the red banner. HOME cancels. */
static void draw_gate(const char *title, const char *const *lines, int n, int progress, bool full)
{
    int cols = ui_cols();
    int r = n + 5;      /* the code's row, under the warning and the prompt */
    /* After the first draw only the code row changes; clearing the screen each press made the banner blink. */
    if (full) {
        ui_clear();
        ui_bg(C_RED);
        ui_color(C_WHITE, true);
        ui_field(0, 0, cols - 1, " !!!  %s  !!!", title);
        ui_reset();

        for (int i = 0; i < n; i++) {
            ui_color(i == 0 ? C_RED : C_WHITE, i == 0);
            ui_field(2 + i, 1, cols - 2, "%s", lines[i]);
        }
        ui_color(C_YELLOW, true);
        ui_field(r - 2, 1, cols - 2, "If you understand and accept this, enter the code:");
        ui_color(C_CYAN, true);
        ui_field(r + 2, 1, cols - 2, "D-Pad, then B and A.  A wrong button sets you back.");
        ui_field(r + 3, 1, cols - 2, "HOME: cancel");
        ui_reset();
    }

    int width = -1;
    for (int i = 0; i < 10; i++)
        width += (int)strlen(KONAMI_GLYPH[i]) + 3;
    ui_goto(r, (cols - width) / 2);
    for (int i = 0; i < 10; i++) {
        ui_color(i < progress ? C_GREEN : C_WHITE, i < progress);
        printf("[%s] ", KONAMI_GLYPH[i]);
    }
    ui_reset();
}

static bool konami_gate(const char *title, const char *const *lines, int n)
{
    int progress = 0;
    draw_gate(title, lines, n, progress, true);
    const u32 mask = WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT |
                     WPAD_BUTTON_RIGHT | WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME;
    while (progress < 10) {
        u32 p = DetectInput(IN_DOWN) & mask;
        if (!p)
            continue;
        if (p & WPAD_BUTTON_HOME)
            return false;
        progress = konami_step(progress, p);
        draw_gate(title, lines, n, progress, false);
    }
    return true;
}

/* Asked the first time something tries to change the NAND. */
bool risk_ensure_unlocked(void)
{
    if (g_unlocked || g_forced)
        return true;
    static const char *const lines[] = {
        "You are about to change the Wii NAND.",
        "",
        "* A wrong move here can PERMANENTLY BRICK your console.",
        "* Deleting IOS, System Menu, boot2 or ticket data is fatal.",
        "* Make a full NAND backup (BootMii) BEFORE you continue.",
        "* On a Wii U this is the vWii NAND - just as fragile.",
        "* No warranty. You are solely responsible for what you do.",
        "* Not sure what a file does? Leave it alone.",
    };
    if (!konami_gate("RISKY OPTION - READ THIS", lines, 8))
        return false;
    g_unlocked = true;
    return true;
}

bool risk_authorize(const char *path, const char *what)
{
    (void)what;
    if (vfs_dev_of(path) != DEV_NAND)
        return true;
    /* One Konami code per session is enough; it asks the first time. */
    return risk_ensure_unlocked();
}

/* ---------------------------------------------------------------- forced mode */

/* The switch lives in a small file on the SD card (or the first Gecko card). */
static const char *const FORCE_DIRS[] = { "sd:/apps/GodModeWii", "gca:/apps/GodModeWii",
                                          "gcb:/apps/GodModeWii" };
#define FORCE_FILE "force.cfg"

static bool force_path(int i, char *out, size_t n)
{
    return snprintf(out, n, "%s/%s", FORCE_DIRS[i], FORCE_FILE) < (int)n;
}

void risk_load_forced(void)
{
    g_forced = false;
    for (int i = 0; i < 3 && !g_forced; i++) {
        char p[64];
        struct stat st;
        if (force_path(i, p, sizeof(p)) && stat(p, &st) == 0)
            g_forced = true;
    }
}

static bool force_store(bool on)
{
    bool wrote = false;
    for (int i = 0; i < 3; i++) {
        char p[64];
        if (!force_path(i, p, sizeof(p)))
            continue;
        if (!on) {
            unlink(p);
            continue;
        }
        if (wrote)
            continue;
        mkdir("sd:/apps", 0777);
        mkdir(FORCE_DIRS[i], 0777);
        FILE *f = fopen(p, "wb");
        if (f) {
            bool ok = fputs("1\n", f) >= 0;
            wrote = (fclose(f) == 0) && ok;
        }
    }
    if (on)
        return wrote;
    /* off: succeeded only if no file is left */
    for (int i = 0; i < 3; i++) {
        char p[64];
        struct stat st;
        if (force_path(i, p, sizeof(p)) && stat(p, &st) == 0)
            return false;
    }
    return true;
}

/* The code is read from the pads while the logo is up. A thread does that so
 * drive detection can carry on in the meantime. */
static volatile bool poll_run, poll_hit;
static volatile int poll_progress;
static volatile u64 poll_last_ms;      /* when the last code button went in */
static lwp_t poll_thread = LWP_THREAD_NULL;
static u8 poll_stack[8192] __attribute__((aligned(32)));

static void *poll_main(void *arg)
{
    int progress = 0;
    while (poll_run) {
        u32 p = input_poll_nowait() & KONAMI_MASK;
        if (p) {
            progress = konami_step(progress, p);
            poll_progress = progress;
            poll_last_ms = ticks_to_millisecs(gettime());
            if (progress == 10) {
                poll_hit = true;
                progress = 0;
                poll_progress = 0;
            }
        }
        usleep(8000);
    }
    return NULL;
}

void risk_startup_poll_begin(void)
{
    poll_run = true;
    poll_hit = false;
    poll_progress = 0;
    poll_last_ms = 0;
    /* Above the main thread (80): drive detection must not starve the code reader. */
    LWP_CreateThread(&poll_thread, poll_main, NULL, poll_stack, sizeof(poll_stack), 100);
}

/* Keeps listening until at least `min_ms` after begin, then reports whether the code went in.
 * A code that is part way in is given time to finish: up to 2 s after each button, 15 s in all. */
bool risk_startup_poll_end(u32 min_ms, u64 began_ms)
{
    for (;;) {
        u64 now = ticks_to_millisecs(gettime());
        u64 elapsed = now - began_ms;
        if (poll_hit || elapsed >= 15000)
            break;
        if (elapsed >= min_ms && !(poll_progress > 0 && now - poll_last_ms < 2000))
            break;
        usleep(20000);
    }
    poll_run = false;
    if (poll_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(poll_thread, NULL);
        poll_thread = LWP_THREAD_NULL;
    }
    return poll_hit;
}

/* Flips the persistent switch; returns false if it could not be saved. */
bool risk_toggle_forced(void)
{
    bool want = !g_forced;
    if (!force_store(want))
        return false;
    g_forced = want;
    return true;
}

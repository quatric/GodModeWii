#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "risk.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"

static bool g_unlocked;

bool risk_unlocked(void) { return g_unlocked; }
void risk_lock(void) { g_unlocked = false; }

static const u32 KONAMI[10] = {
    WPAD_BUTTON_UP, WPAD_BUTTON_UP, WPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN,
    WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT,
    WPAD_BUTTON_B, WPAD_BUTTON_A,
};
static const char *const KONAMI_GLYPH[10] = { "UP", "UP", "DN", "DN", "LT", "RT", "LT", "RT", " B", " A" };

/* Full-screen warning followed by the Konami code. `lines` are the warning
 * text, `title` the red banner. HOME cancels. */
static void draw_gate(const char *title, const char *const *lines, int n, int progress)
{
    int cols = ui_cols();
    ui_clear();
    ui_bg(C_RED);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " !!!  %s  !!!", title);
    ui_reset();

    int r = 2;
    for (int i = 0; i < n; i++) {
        ui_color(i == 0 ? C_RED : C_WHITE, i == 0);
        ui_field(r++, 1, cols - 2, "%s", lines[i]);
    }
    r++;
    ui_color(C_YELLOW, true);
    ui_field(r++, 1, cols - 2, "If you understand and accept this, enter the code:");
    r++;

    int left = (cols - 10 * 5) / 2;
    ui_goto(r, left);
    for (int i = 0; i < 10; i++) {
        ui_color(i < progress ? C_GREEN : C_WHITE, i < progress);
        printf("[%s] ", KONAMI_GLYPH[i]);
    }
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(r + 2, 1, cols - 2, "D-Pad, then B and A.  A wrong button starts over.");
    ui_field(r + 3, 1, cols - 2, "HOME: cancel");
    ui_reset();
}

static bool konami_gate(const char *title, const char *const *lines, int n)
{
    int progress = 0;
    draw_gate(title, lines, n, progress);
    const u32 mask = WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT |
                     WPAD_BUTTON_RIGHT | WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME;
    while (progress < 10) {
        u32 p = DetectInput(IN_DOWN) & mask;
        if (!p)
            continue;
        if (p & WPAD_BUTTON_HOME)
            return false;
        if (p == KONAMI[progress])
            progress++;
        else
            progress = (p == KONAMI[0]) ? 1 : 0;
        draw_gate(title, lines, n, progress);
    }
    return true;
}

/* Asked the first time something tries to change the NAND. */
bool risk_ensure_unlocked(void)
{
    if (g_unlocked)
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

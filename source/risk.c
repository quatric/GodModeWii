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
static const char *KONAMI_GLYPH[10] = { "UP", "UP", "DN", "DN", "LT", "RT", "LT", "RT", " B", " A" };

static const char *SAFE_NAND[] = {
    "nand:/tmp",
    "nand:/import",
    "nand:/title/00010000",   /* disc game save data  */
    "nand:/title/00010001",   /* WiiWare / VC channels */
    "nand:/title/00010004",   /* disc game channels   */
    "nand:/title/00010005",   /* DLC                  */
};

/* Always "critical", whatever the safe list says. This is not a block: it
 * only decides that the 3-second hold-A confirmation is required (after the
 * Konami unlock). /shared1 holds shared system content, so a mistake there
 * can brick the console. */
static const char *CRITICAL_NAND[] = {
    "nand:/sys",
    "nand:/ticket",
    "nand:/shared1",
    "nand:/shared2",
    "nand:/title/00000001",   /* IOS, boot2, System Menu */
    "nand:/title/00000002",
};

bool risk_is_critical(const char *path)
{
    if (vfs_dev_of(path) != DEV_NAND)
        return false;
    for (size_t i = 0; i < sizeof(CRITICAL_NAND) / sizeof(CRITICAL_NAND[0]); i++)
        if (vfs_is_within(path, CRITICAL_NAND[i]))
            return true;
    for (size_t i = 0; i < sizeof(SAFE_NAND) / sizeof(SAFE_NAND[0]); i++)
        if (vfs_is_within(path, SAFE_NAND[i]))
            return false;
    return true;
}

static void draw_unlock_screen(int progress)
{
    int cols = ui_cols();
    ui_clear();
    ui_goto(0, 0);
    ui_bg(C_RED);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " !!!  RISKY OPTIONS  -  READ THIS  !!!");
    ui_reset();

    int r = 2;
    ui_color(C_RED, true);
    ui_field(r++, 1, cols - 2, "Unlocking lets GodModeWii WRITE, DELETE and RENAME files on");
    ui_field(r++, 1, cols - 2, "the Wii NAND.");
    ui_color(C_WHITE, false);
    r++;
    ui_field(r++, 1, cols - 2, "* A wrong move here can PERMANENTLY BRICK your console.");
    ui_field(r++, 1, cols - 2, "* Deleting IOS, System Menu, boot2 or ticket data is fatal.");
    ui_field(r++, 1, cols - 2, "* Make a full NAND backup (BootMii) BEFORE you continue.");
    ui_field(r++, 1, cols - 2, "* No warranty. You are solely responsible for what you do.");
    ui_field(r++, 1, cols - 2, "* Not sure what a file does? Leave it alone.");
    r++;
    ui_color(C_YELLOW, true);
    ui_field(r++, 1, cols - 2, "If you understand and accept this, enter the code:");
    r++;

    int left = (cols - 10 * 4) / 2;
    ui_goto(r, left);
    for (int i = 0; i < 10; i++) {
        if (i < progress) ui_color(C_GREEN, true);
        else ui_color(C_WHITE, false);
        printf("[%s] ", KONAMI_GLYPH[i]);
    }
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(r + 2, 1, cols - 2, "D-Pad, then B and A.  Wrong button starts over.");
    ui_field(r + 3, 1, cols - 2, "HOME: cancel");
    ui_reset();
}

bool risk_unlock_flow(void)
{
    if (g_unlocked)
        return true;

    int progress = 0;
    draw_unlock_screen(progress);
    const u32 mask = WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT |
                     WPAD_BUTTON_RIGHT | WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME;
    while (progress < 10) {
        u32 p = DetectInput(IN_DOWN) & mask;
        if (!p)
            continue;
        if (p & WPAD_BUTTON_HOME)
            return false;
        if (p == KONAMI[progress]) {
            progress++;
        } else {
            progress = (p == KONAMI[0]) ? 1 : 0;
        }
        draw_unlock_screen(progress);
    }

    if (!ui_hold_confirm("FINAL WARNING",
            "Code accepted. Risky options stay unlocked until you exit or "
            "lock them again.\nYou can still BRICK your Wii.", 3))
        return false;
    g_unlocked = true;
    return true;
}

bool risk_authorize(const char *path, const char *what)
{
    if (vfs_dev_of(path) != DEV_NAND)
        return true;

    if (!g_unlocked) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Cannot %s on the NAND while risky options are locked.\n\n"
                 "Open the HOME menu and choose \"Unlock risky options\" "
                 "(Konami code). Copying FROM the NAND is always allowed.", what);
        ui_message("LOCKED", C_YELLOW, msg);
        return false;
    }

    if (risk_is_critical(path)) {
        char msg[300];
        snprintf(msg, sizeof(msg),
                 "You are about to %s:\n%s\n\nThis is a SYSTEM area. "
                 "A mistake here can brick your Wii for good.", what, path);
        return ui_hold_confirm("BRICK RISK", msg, 3);
    }
    return true;
}

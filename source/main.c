/*
 * GodModeWii - a GodMode9-style, text-only file manager for the Wii and the
 * Wii U's vWii mode. See README.md for credits and the brick-risk warning.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/es.h>
#include <ogc/ipc.h>
#include <ogc/wiilaunch.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <wiiupdate.h>

#include "app.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "risk.h"
#include "ftp.h"
#include "runtimeiospatch.h"

/* Stay on the IOS the loader started us with (IOS58 under the Homebrew
 * Channel). Reloading IOS would drop the HW_AHBPROT access the NAND
 * permission patch needs. Same trick as FSToolBox-Mod. */
s32 __IOS_LoadStartupIOS(void)
{
    return 0;
}

static void on_reset(u32 irq, void *ctx) { g_quit_request = true; }
static void on_power(void)
{
    g_quit_request = true;
    g_power_request = true;
}

static bool detect_dolphin(void)
{
    s32 fd = IOS_Open("/dev/dolphin", IPC_OPEN_NONE);
    if (fd >= 0) {
        IOS_Close(fd);
        return true;
    }
    return false;
}

/* From Priiloader: the vWii NandLoader titles only exist on a Wii U. */
static bool detect_vwii(void)
{
    u32 x = 0;
    s32 ret = ES_GetTitleContentsCount(0x0000000100000200ll, &x);
    return ret >= 0 && x > 0;
}

/* Ask GitHub whether a newer release is out. B skips it, and any failure
 * (no network, no reply) just carries on. Returns true when the user chose to
 * install the update. */
static bool check_for_update(void)
{
    if (WIIUPDATE_Start("quatric", "GodModeWii", GMW_VERSION) < 0)
        return false;
    ui_splash("Checking for updates...   B  Skip");
    u64 start = gettime();
    while (WIIUPDATE_State() == WIIUPDATE_CHECKING) {
        if ((DetectInput(IN_DOWN) & WPAD_BUTTON_B) || ticks_to_secs(diff_ticks(start, gettime())) >= 20) {
            WIIUPDATE_Cancel();
            return false;
        }
    }
    if (!WIIUPDATE_TakeNotice())
        return false;
    char text[300];
    if (WIIUPDATE_CanInstall()) {
        snprintf(text, sizeof text, "%s\n\nUpdate now? GodModeWii will download it, install it over this copy and "
                 "return to the loader.", WIIUPDATE_Message());
        return ui_confirm("UPDATE AVAILABLE", C_GREEN, text);
    }
    snprintf(text, sizeof text, "%s\n\nDownload it from\n%s", WIIUPDATE_Message(), WIIUPDATE_Url());
    ui_message("UPDATE AVAILABLE", C_GREEN, text);
    return false;
}

/* Download and install the update found by check_for_update, once the drives
 * are mounted. B cancels the download. Returns true when it is installed. */
static bool install_update(void)
{
    if (WIIUPDATE_Install() < 0)
        return false;
    wiiupdate_install_state st;
    int shown = -1;
    while ((st = WIIUPDATE_InstallState()) == WIIUPDATE_INSTALL_DOWNLOADING || st == WIIUPDATE_INSTALL_EXTRACTING) {
        int pct = WIIUPDATE_Progress();
        if (pct != shown) {
            char status[96];
            snprintf(status, sizeof status, "%s %s...  %d%%%s",
                     st == WIIUPDATE_INSTALL_DOWNLOADING ? "Downloading" : "Installing", WIIUPDATE_Latest(), pct,
                     st == WIIUPDATE_INSTALL_DOWNLOADING ? "   B  Cancel" : "");
            ui_splash(status);
            ui_bar(ui_rows() / 2 + 3, (ui_cols() - 50) / 2, 50, pct, C_GREEN);
            shown = pct;
        }
        if (DetectInput(IN_DOWN) & WPAD_BUTTON_B)
            WIIUPDATE_Cancel();
    }
    WIIUPDATE_Wait(5000);
    ui_splash("");
    char text[200];
    if (st == WIIUPDATE_INSTALL_DONE) {
        snprintf(text, sizeof text, "GodModeWii %s is installed. Press A to return to the loader.", WIIUPDATE_Latest());
        ui_message("UPDATED", C_GREEN, text);
        return true;
    }
    snprintf(text, sizeof text, "The update could not be installed (%s). This copy is unchanged.",
             WIIUPDATE_InstallError());
    ui_message("UPDATE FAILED", C_RED, text);
    return false;
}

int main(int argc, char **argv)
{
    ui_init();
    ui_splash("Starting...");

    g_is_dolphin = detect_dolphin();
    g_is_vwii = detect_vwii();

    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);

    /* NAND permissions: the same ISFS patch FSToolBox-Mod applies, and only
     * that one (the hash / identify / setuid patches are not needed here). */
    if (AHBPROT_DISABLED && !g_is_dolphin)     /* the scan is pointless (and very slow) in an emulator */
        g_nand_full = IosPatch_ISFS(false) > 0;

    input_init();
    WIIUPDATE_SetAppPath(argc > 0 ? argv[0] : NULL);
    bool update = check_for_update();
    ui_splash("Looking for drives...");
    u64 began = ticks_to_millisecs(gettime());
    risk_startup_poll_begin();
    vfs_init();
    bool toggled = risk_startup_poll_end(2500, began);
    risk_load_forced();
    if (toggled) {
        bool saved = risk_toggle_forced();
        if (!saved)
            ui_message("RISKY MODE", C_RED, "Could not save the setting. Is the SD card writable?");
        else if (risk_forced())
            ui_message("RISKY MODE FORCED ON", C_RED,
                       "Dangerous permissions are now ALWAYS enabled, and stay that way every time this program starts. "
                       "Nothing will ask for a warning or code first.");
        else
            ui_message("RISKY MODE OFF", C_GREEN, "Dangerous permissions are no longer forced. Risky changes ask for the warning and code again.");
    } else if (risk_forced()) {
        ui_message("RISKY MODE FORCED ON", C_RED,
                   "Dangerous permissions are ALWAYS enabled. Nothing will ask for a warning or code first.");
    }

    int mode = update && install_update() ? EXIT_LOADER : app_run();

    ui_splash("Unmounting...");
    risk_lock();
    WIIUPDATE_Wait(10000);
    ftp_net_shutdown();
    vfs_shutdown();
    input_shutdown();

    switch (mode) {
    case EXIT_POWEROFF:
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
        break;
    case EXIT_RESTART:
        if (g_is_vwii)                  /* the vWii NandLoader hands control back to the Wii U menu */
            WII_LaunchTitle(0x0000000100000200ll);
        SYS_ResetSystem(SYS_RESTART, 0, 0);
        break;
    case EXIT_MENU:
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        break;
    default:
        exit(0);
    }
    return 0;
}

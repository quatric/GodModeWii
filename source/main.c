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
#include <wiiuse/wpad.h>

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
    ui_splash("Looking for drives...");
    vfs_init();

    int mode = app_run();

    ui_splash("Unmounting...");
    risk_lock();
    ftp_net_shutdown();
    vfs_shutdown();
    input_shutdown();

    switch (mode) {
    case EXIT_POWEROFF:
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
        break;
    case EXIT_MENU:
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        break;
    default:
        exit(0);
    }
    return 0;
}

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
#include <network.h>

#include "app.h"
#include "ui.h"
#include "sfx.h"
#include "wiilight.h"
#include "colors.h"
#include "alarm.h"
#include "input.h"
#include "vfs.h"
#include "risk.h"
#include "ftp.h"
#include "runtimeiospatch.h"
#include "launch.h"
#include "arcfs.h"

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

/* exit() returns through the stub HBC leaves behind. A forwarder channel leaves none, so HBC is
 * then started by title ID, newest first, as USB Loader GX does. */
static void exit_to_loader(void)
{
    static const u64 hbc[] = { 0x000100014F484243ll, 0x000100014C554C5All, 0x00010001AF1BF516ll,
                               0x000100014A4F4449ll, 0x0001000148415858ll };
    if (!memcmp((const void *)0x80001804, "STUBHAXX", 8))
        exit(0);
    WII_Initialize();
    for (size_t i = 0; i < sizeof(hbc) / sizeof(hbc[0]); i++)
        WII_LaunchTitle(hbc[i]);
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
}

/* Ask GitHub whether a newer release is out. B skips it, and any failure
 * (no network, no reply) just carries on. Returns true when the user chose to
 * install the update. */
static bool check_for_update(void)
{
    if (WIIUPDATE_Start("quatric", "GodModeWii", GMW_VERSION) < 0)
        return false;
    ui_splash("Initializing network...   B  Skip");
    u64 start = gettime(), polled = 0;
    bool online = false;
    while (WIIUPDATE_State() == WIIUPDATE_CHECKING) {
        /* Each poll is an IOS call, so the network start-up isn't asked about more than every
         * 100 ms. */
        if (!online && ticks_to_millisecs(diff_ticks(polled, gettime())) >= 100) {
            polled = gettime();
            if (net_gethostip()) {
                online = true;
                ui_splash("Checking for updates...   B  Skip");
            }
        }
        if ((DetectInput(IN_DOWN) & WPAD_BUTTON_B) ||
            ticks_to_secs(diff_ticks(start, gettime())) >= 20) {
            WIIUPDATE_Cancel();
            return false;
        }
    }

    if (!WIIUPDATE_TakeNotice())
        return false;

    char text[300];
    if (WIIUPDATE_CanInstall()) {
        snprintf(text, sizeof text,
                 "%s\n\nUpdate now? GodModeWii will download it, install it over this copy and "
                 "return to the loader.",
                 WIIUPDATE_Message());
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

    while ((st = WIIUPDATE_InstallState()) == WIIUPDATE_INSTALL_DOWNLOADING ||
           st == WIIUPDATE_INSTALL_EXTRACTING) {
        int pct = WIIUPDATE_Progress();
        if (pct != shown) {
            char status[96];
            snprintf(status, sizeof status, "%s %s...  %d%%%s",
                     st == WIIUPDATE_INSTALL_DOWNLOADING ? "Downloading" : "Installing",
                     WIIUPDATE_Latest(), pct,
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
        snprintf(text, sizeof text, "GodModeWii %s is installed. Press A to return to the loader.",
                 WIIUPDATE_Latest());
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
    /* the scan is pointless (and very slow) in an emulator */
    if (AHBPROT_DISABLED && !g_is_dolphin)
        g_nand_full = IosPatch_ISFS(false) > 0;

    input_init();
    wiilight_init();
    WIIUPDATE_SetAppPath(argc > 0 ? argv[0] : NULL);
#ifdef GMW_AUTOTEST
    bool update = false;
    (void)check_for_update;
#else
    bool update = check_for_update();
#endif

    ui_splash("Looking for drives...");
    u64 began = ticks_to_millisecs(gettime());
    risk_startup_poll_begin();
    vfs_init();
    bool toggled = risk_startup_poll_end(4000, began);
    risk_load_forced();

    /* what is kept on the card, now that it's mounted: sounds, light, theme, alarm */
    sfx_load_setting();
    wiilight_load_setting();
    colors_load();
    alarm_load();
    sfx_play(SFX_STARTUP);                          /* the soft tick of B; in TiVo mode, the TiVo Central chime */
    wiilight_pattern(WL_DISC_CLOSE);

    if (toggled) {
        bool saved = risk_toggle_forced();
        if (!saved)
            ui_message("RISKY MODE", C_RED, "Could not save the setting. Is the SD card writable?");
        else if (risk_forced())
            ui_message("RISKY MODE FORCED ON", C_RED,
                       "Dangerous permissions are now ALWAYS enabled, and stay that way every time this program starts. "
                       "Nothing will ask for a warning or code first.");
        else
            ui_message("RISKY MODE OFF", C_GREEN,
                       "Dangerous permissions are no longer forced. Risky changes ask for the warning and code again.");
    }
    /* already forced on from an earlier start: no dialog again (it was shown when it was turned on) */

#ifdef GMW_AUTOTEST
    /* Test builds only: open each path listed in sd:/gmwtest.txt (the viewers dump the screen),
     * then stop. */
    {
        extern bool media_open(const char *path);

        FILE *tf = fopen("sd:/gmwtest.txt", "r");
        char line[512];

        while (tf && fgets(line, sizeof line, tf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            /* the next layout opens paused on this frame */
            if (!strncmp(line, "@frame ", 7)) {
                extern int g_lyt_test_frame;
                g_lyt_test_frame = atoi(line + 7);
                continue;
            }
            /* "@wad2bin <wad> <keys> [null]": a wad2bin conversion with fixed keys */
            if (!strncmp(line, "@wad2bin ", 9)) {
                extern void wad2bin_autotest(const char *wad, const char *keyfile, bool null_key);
                char wad[256], keyf[256], nk[8] = "";
                if (sscanf(line + 9, "%255s %255s %7s", wad, keyf, nk) >= 2)
                    wad2bin_autotest(wad, keyf, !strcmp(nk, "null"));
                continue;
            }
            /* "@waddump <title folder or content.bin> <out.wad>": a WAD backup */
            if (!strncmp(line, "@waddump ", 9)) {
                extern void waddump_autotest(const char *src, const char *out);
                char src[400], dst[256];
                if (sscanf(line + 9, "%399s %255s", src, dst) == 2)
                    waddump_autotest(src, dst);
                continue;
            }
            /* "@copy" / "@move <source> <target folder> [skip|overwrite]": a paste, logged */
            if (!strncmp(line, "@copy ", 6) || !strncmp(line, "@move ", 6)) {
                extern void ops_autotest(const char *src, const char *dst_dir, bool cut,
                                         const char *policy);
                char src[256], dst[256], pol[16] = "overwrite";
                if (sscanf(line + 6, "%255s %255s %15s", src, dst, pol) >= 2)
                    ops_autotest(src, dst, line[1] == 'm', pol);
                continue;
            }
            /* "@databin <title id> <out>": a save exported as data.bin with a test console's keys */
            if (!strncmp(line, "@databin ", 9)) {
                extern void databin_autotest(const char *tid, const char *out);
                char t[32], dst[256];
                if (sscanf(line + 9, "%31s %255s", t, dst) == 2)
                    databin_autotest(t, dst);
                continue;
            }
            /* "@dbimport <data.bin>": a save copied back to the NAND */
            if (!strncmp(line, "@dbimport ", 10)) {
                extern void databin_import_autotest(const char *path);
                char src[400];
                if (sscanf(line + 10, "%399s", src) == 1)
                    databin_import_autotest(src);
                continue;
            }
            /* "@theme install|convert <path>": the Wii Menu theme installer, answering yes */
            if (!strncmp(line, "@theme ", 7)) {
                extern void theme_autotest(const char *cmd, const char *path);
                char cmd[16], tp[400];
                if (sscanf(line + 7, "%15s %399s", cmd, tp) == 2)
                    theme_autotest(cmd, tp);
                continue;
            }
            /* "@colors <n>": theme n, then a sample screen saved like a viewer's */
            if (!strncmp(line, "@colors ", 8)) {
                extern void colors_autotest(int i);
                extern bool ui_test_dump(void);
                colors_autotest(atoi(line + 8));
                ui_clear();
                ui_bg(C_BLUE);
                ui_color(C_WHITE, true);
                ui_field(0, 0, ui_cols() - 1, " GodModeWii  theme test");
                ui_reset();
                ui_goto(2, 2); ui_color(C_CYAN, true); printf("Heading");
                ui_goto(3, 2); ui_color(C_WHITE, false); printf("normal text");
                ui_goto(4, 2); ui_bg(C_CYAN); ui_color(C_BLACK, false); printf(" selected row ");
                ui_reset();
                ui_goto(5, 2); ui_color(C_GREEN, true); printf("done");
                ui_goto(6, 2); ui_color(C_YELLOW, true); printf("warning");
                ui_goto(7, 2); ui_color(C_RED, true); printf("error");
                ui_goto(8, 0); ui_bg(C_GREEN); ui_color(C_BLACK, false); printf(" green bar ");
                ui_reset();
                ui_test_dump();
                continue;
            }
            /* "@pride": the Pride theme on the drive list, a folder and a dialog */
            if (!strcmp(line, "@pride")) {
                extern void app_pride_autotest(void);
                app_pride_autotest();
                continue;
            }
            /* "@sweeper": Bad Block Sweeper's board, from the bad blocks of a nand.bin on the card */
            if (!strcmp(line, "@sweeper")) {
                extern void sweeper_autotest(void);
                sweeper_autotest();
                continue;
            }
            /* "@tivo": TiVo mode on (its dialog saved like a viewer's), a few of its sounds, then off again */
            if (!strcmp(line, "@tivo") || !strcmp(line, "@tivo once")) {
                FILE *lg = fopen("sd:/gmwtest.log", "a");   /* as loaded at start (it's saved on the card) */
                if (lg) { fprintf(lg, "tivo at start: %d\n", sfx_tivo()); fclose(lg); }
                for (int k = 0; k < (line[5] ? 1 : 2); k++) {
                    bool saved = sfx_tivo_toggle();
                    ui_tivo_dialog(sfx_tivo(), saved, sfx_enabled());
                    sfx_wait();
                    if (sfx_tivo()) {
                        static const int ids[] = { SFX_CLICK, SFX_LONGTAP, SFX_PAGEUP, SFX_PAGEDOWN, SFX_UPDOWN };
                        for (int j = 0; j < 5; j++) { sfx_play(ids[j]); sfx_wait(); }
                    }
                }
                continue;
            }
            /* one wiiload upload (sent from the PC), logged */
            if (!strcmp(line, "@wiiload")) {
                extern bool wiiload_run(char *launch, size_t launchsz);
                char prog[512] = "";
                wiiload_run(prog, sizeof prog);
                continue;
            }

            bool ok = media_open(line);
            /* note how far into the path it got: each archive level, then the file */
            FILE *lg = fopen("sd:/gmwtest.log", "a");
            if (lg) {
                char part[512];
                for (char *bar = strchr(line, '|'); bar; bar = strchr(bar + 1, '|')) {
                    snprintf(part, sizeof part, "%.*s|/", (int)(bar - line), line);
                    vfs_entry *list;
                    int n = -1;
                    bool listed = vfs_list_visible(part, &list, &n);
                    fprintf(lg, "  list %s: %s (%d)%s%s\n", part, listed ? "ok" : "failed", n,
                            listed ? "" : " ", listed ? "" : arcfs_error());
                    if (listed) free(list);
                }
                vfs_file vf;
                bool rd = vfs_open_read(&vf, line);
                if (rd) vfs_close(&vf);
                fprintf(lg, "%s: viewer %s, file %s\n", line, ok ? "opened" : "failed",
                        rd ? "readable" : "unreadable");
                fclose(lg);
            }
        }

        if (tf) fclose(tf);

        FILE *done = fopen("sd:/gmwtest.done", "w");
        if (done) fclose(done);
        vfs_shutdown();
        for (;;) VIDEO_WaitVSync();
    }
#endif
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
        /* the vWii NandLoader hands control back to the Wii U menu */
        if (g_is_vwii)
            WII_LaunchTitle(0x0000000100000200ll);
        SYS_ResetSystem(SYS_RESTART, 0, 0);
        break;
    case EXIT_MENU:
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        break;
    case EXIT_LAUNCH:
        launch_go();                    /* only comes back if nothing was loaded */
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        break;
    default:
        exit_to_loader();
    }
    return 0;
}

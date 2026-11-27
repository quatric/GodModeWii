/*
 * Console info: the model and settings from setting.txt, boot2's version (ES and the SEEPROM's two
 * counters), and the tools that go with them. See sysinfo.h.
 */
#include <stdio.h>
#include <string.h>
#include <ogc/es.h>

#include "sysinfo.h"
#include "setting.h"
#include "seeprom.h"
#include "nanddump.h"
#include "runtimeiospatch.h"
#include "hbcreborn.h"
#include "ui.h"
#include "vfs.h"

static void show_info(void)
{
    char msg[1200], v[48];
    int p = 0, model = setting_model();
    char m[32] = "?";
    setting_get("MODEL", m, sizeof(m));
    p += snprintf(msg + p, sizeof(msg) - p, "Model:     %s (%s)\n", setting_model_name(model), m);
    if (setting_get("AREA", v, sizeof(v))) {
        char g[16] = "?", vid[16] = "?";
        setting_get("GAME", g, sizeof(g));
        setting_get("VIDEO", vid, sizeof(vid));
        p += snprintf(msg + p, sizeof(msg) - p, "Region:    %s  (games %s, video %s)\n", v, g, vid);
    }
    char code[8], serno[16];
    if (setting_get("CODE", code, sizeof(code)) && setting_get("SERNO", serno, sizeof(serno)))
        p += snprintf(msg + p, sizeof(msg) - p, "Serial:    %s%s\n", code, serno);
    u32 id = 0;
    if (ES_GetDeviceID(&id) >= 0)
        p += snprintf(msg + p, sizeof(msg) - p, "Console ID: %08X\n", (unsigned)id);

    u32 b2 = 0;
    if (!g_is_vwii && ES_GetBoot2Version(&b2) >= 0)
        p += snprintf(msg + p, sizeof(msg) - p, "boot2:     v%u (ES)\n", (unsigned)b2);
    /* the SEEPROM keeps two copies of boot2's counter: version, then an update tag */
    u8 seep[SEEPROM_SIZE];
    if (!g_is_dolphin && !g_is_vwii && AHBPROT_DISABLED && seeprom_read(seep)) {
        p += snprintf(msg + p, sizeof(msg) - p, "SEEPROM:   boot2 counters");
        for (int i = 0; i < 2; i++) {
            const u8 *c = seep + 0x48 + i * 10;
            u32 tag = (u32)c[4] << 24 | (u32)c[5] << 16 | (u32)c[6] << 8 | c[7];
            p += snprintf(msg + p, sizeof(msg) - p, "%s v%u (tag %u)", i ? "," : "", c[0], (unsigned)tag);
        }
        p += snprintf(msg + p, sizeof(msg) - p, "\n");
    }
    if (model == MODEL_MINI)
        p += snprintf(msg + p, sizeof(msg) - p, "\nA Wii mini: no SD card slot, Wi-Fi or GameCube ports.\n");
    else if (model == MODEL_FAMILY)
        p += snprintf(msg + p, sizeof(msg) - p, "\nA Wii Family Edition: no GameCube ports.\n");
    snprintf(msg + p, sizeof(msg) - p, "\nboot1 and the boot blocks: \"Check boot1 / boot2\".");
    ui_message("CONSOLE INFO", C_CYAN, msg);
}

void sysinfo_run(void)
{
    int sel = 0;
    for (;;) {
        const char *items[] = {
            "Show console info (model, region, boot2)",
            "Check boot1 / boot2 (reads the NAND chip)",
            "Edit setting.txt (region, model, serial)",
            "Install / update HBC Reborn (latest, from GitHub)",
        };
        bool on[] = { true, !g_is_dolphin && !g_is_vwii, true, true };
        ui_clear();
        int r = ui_menu_ex("CONSOLE", items, on, 4, sel, "A Select   B Back");
        if (r < 0) return;
        sel = r;
        if (r == 0) {
            show_info();
        } else if (r == 1) {
            char out[700];
            int ok = nanddump_boot_report(out, sizeof(out));
            ui_message(ok == 0 ? "BOOT1 / BOOT2" : "BOOT1 / BOOT2: COULDN'T READ", ok == 0 ? C_CYAN : C_RED, out);
        } else if (r == 2) {
            setting_editor();
        } else {
            hbcreborn_install();
        }
    }
}

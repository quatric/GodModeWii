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
#include "iospatch.h"
#include "hbcreborn.h"
#include "ui.h"
#include "risk.h"
#include "discdump.h"
#include <stdlib.h>
#include <malloc.h>
#include <ogc/isfs.h>
#include <ogc/lwp_watchdog.h>
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

/* ---- NAND write test (Risky Mode): a file in /tmp written, read back, compared and deleted ---- */

#define NT_PATH "/tmp/gmw_nandtest.bin"
#define NT_BLOCK 0x4000

static u32 nt_word(u32 i) { u32 x = i * 2654435761u + 0x9E3779B9u; x ^= x >> 15; return x * 0x85EBCA6Bu; }

static int nand_write_test(u32 mb, char *msg, size_t mn, FILE *lg)
{
    u32 *buf = memalign(32, NT_BLOCK);
    if (!buf) { snprintf(msg, mn, "Out of memory."); return -1; }
    u32 blocks = mb * (1u << 20) / NT_BLOCK, bad = 0;
    s32 r = 0;
    ISFS_Delete(NT_PATH);
    u64 t0 = ticks_to_millisecs(gettime()), tw = 0;
    int fd = -1;
    if ((r = ISFS_CreateFile(NT_PATH, 0, 3, 3, 0)) < 0 || (fd = ISFS_Open(NT_PATH, ISFS_OPEN_WRITE)) < 0) {
        snprintf(msg, mn, "Couldn't create %s (%d).", NT_PATH, fd < 0 && r >= 0 ? fd : r);
        free(buf);
        return -1;
    }
    for (u32 b = 0; b < blocks && r >= 0; b++) {
        for (u32 i = 0; i < NT_BLOCK / 4; i++) buf[i] = nt_word(b * (NT_BLOCK / 4) + i);
        r = ISFS_Write(fd, buf, NT_BLOCK);
        if (r == NT_BLOCK) r = 0;
        else if (r >= 0) r = -1;
        if (!lg && (b & 15) == 15) ui_busy_progress((u64)b + 1, (u64)blocks * 2);
    }
    ISFS_Close(fd);
    tw = ticks_to_millisecs(gettime()) - t0;
    if (r < 0) {
        ISFS_Delete(NT_PATH);
        snprintf(msg, mn, "Writing failed (%d) - the NAND may be full, or failing.", (int)r);
        free(buf);
        return -1;
    }
    t0 = ticks_to_millisecs(gettime());
    fd = ISFS_Open(NT_PATH, ISFS_OPEN_READ);
    for (u32 b = 0; b < blocks && fd >= 0; b++) {
        if (ISFS_Read(fd, buf, NT_BLOCK) != NT_BLOCK) { bad += NT_BLOCK / 4; continue; }
        for (u32 i = 0; i < NT_BLOCK / 4; i++) if (buf[i] != nt_word(b * (NT_BLOCK / 4) + i)) bad++;
        if (!lg && (b & 15) == 15) ui_busy_progress((u64)blocks + b + 1, (u64)blocks * 2);
    }
    u64 tr = ticks_to_millisecs(gettime()) - t0;
    if (fd >= 0) ISFS_Close(fd);
    s32 del = ISFS_Delete(NT_PATH);
    free(buf);
    snprintf(msg, mn, "Wrote and read back %u MB in /tmp.\n\nWrite: %u KB/s\nRead:  %u KB/s\nWords that came back wrong: %u\n%s\n\n%s",
             (unsigned)mb, tw ? (unsigned)((u64)mb * 1024 * 1000 / tw) : 0, tr ? (unsigned)((u64)mb * 1024 * 1000 / tr) : 0,
             (unsigned)bad, del < 0 ? "The test file couldn't be deleted (it goes at the next boot: /tmp is cleared)." : "The test file was deleted.",
             fd < 0 ? "The file couldn't be opened again to read it." :
             bad ? "Data came back different from what was written: the NAND is failing. Back it up now." : "Everything came back as written.");
    if (lg) fprintf(lg, "nandtest: mb %u bad %u fd %d del %d\n", (unsigned)mb, (unsigned)bad, fd, (int)del);
    return bad || fd < 0 ? 1 : 0;
}

static void nand_test_menu(void)
{
    const char *T = "NAND WRITE TEST";
    if (!risk_ensure_unlocked()) return;
    if (!ui_confirm(T, C_YELLOW, "Write a test file to the NAND's /tmp, read it back, compare it and delete it. It shows "
                    "whether the NAND still keeps what is written and how fast it is.\n\nEvery write wears the NAND a little: "
                    "run it when you suspect the NAND, not for fun. Nothing else on the NAND is touched.")) return;
    const char *sizes[] = { "4 MB (quick)", "16 MB", "64 MB" };
    static const u32 mbs[] = { 4, 16, 64 };
    int k = ui_menu("TEST SIZE", sizes, NULL, 3, 0);
    if (k < 0) return;
    char msg[500];
    ui_busy_begin("Testing the NAND...");
    int r = nand_write_test(mbs[k], msg, sizeof(msg), NULL);
    ui_busy_end();
    ui_message(T, r == 0 ? C_GREEN : r > 0 ? C_RED : C_YELLOW, msg);
}

#ifdef GMW_AUTOTEST
void sysinfo_nandtest_autotest(void)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    char msg[500];
    nand_write_test(2, msg, sizeof(msg), lg);
    fclose(lg);
}
#endif

void sysinfo_run(void)
{
    int sel = 0;
    for (;;) {
        const char *items[] = {
            "Show console info (model, region, boot2)",
            "Check boot1 / boot2 (reads the NAND chip)",
            "Edit setting.txt (region, model, serial)",
            "Install / update HBC Reborn (latest, from GitHub)",
            "Disc drive stress test (Risky Mode)",
            "NAND write test (Risky Mode)",
        };
        bool on[] = { true, !g_is_dolphin && !g_is_vwii, true, true, !g_is_vwii, true };
        ui_clear();
        int r = ui_menu_ex("CONSOLE", items, on, 6, sel, "A Select   B Back");
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
        } else if (r == 3) {
            hbcreborn_install();
        } else if (r == 4) {
            discdump_stress_test();
        } else {
            nand_test_menu();
        }
    }
}

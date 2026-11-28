/*
 * GodModeWii - box and disc art from GameTDB. See covers.h.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "wiiupdate.h"

#include "covers.h"
#include "vfs.h"
#include "ui.h"
#include "ftp.h"
#include "media.h"

/* GameTDB's art folders by the region letter of the ID (its 4th character). */
static const char *region(char r)
{
    switch (r) {
    case 'E': case 'N': return "US";
    case 'J': return "JA";
    case 'K': case 'Q': case 'T': return "KO";
    case 'D': return "DE";
    case 'F': return "FR";
    case 'I': return "IT";
    case 'S': return "ES";
    case 'H': return "NL";
    default: return "EN";
    }
}

static int cache_dev(void)
{
    for (int d = DEV_SD; d <= DEV_USB7; d++)
        if (d != DEV_GCA && d != DEV_GCB && g_devs[d].mounted && !vfs_is_readonly(g_devs[d].root)) return d;
    return -1;
}

bool covers_show(const char *id6, bool disc)
{
    const char *T = disc ? "DISC ART" : "COVER";
    char id[7] = "";
    for (int i = 0; i < 6 && id6[i]; i++) id[i] = id6[i];
    if (strlen(id) != 6) { ui_message(T, C_YELLOW, "GameTDB's art needs the game's full six-character ID."); return false; }
    int dev = cache_dev();
    if (dev < 0) { ui_message(T, C_RED, "There is no writable SD card or USB drive to keep the picture on."); return false; }

    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%sGodModeWii/covers", g_devs[dev].root);
    mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%sGodModeWii/covers/%s", g_devs[dev].root, disc ? "disc" : "box");
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s.png", dir, id);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return image_view(path);

    ui_splash("Connecting...");
    if (!ftp_net_start()) return false;
    const char *regs[3] = { region(id[3]), "EN", "US" };
    for (int i = 0; i < 3; i++) {
        if (i && !strcmp(regs[i], regs[0])) continue;
        char url[160], msg[96];
        snprintf(url, sizeof(url), "https://art.gametdb.com/wii/%s/%s/%s.png", disc ? "disc" : "cover", regs[i], id);
        snprintf(msg, sizeof(msg), "Downloading %s art for %s (%s)...", disc ? "disc" : "box", id, regs[i]);
        ui_splash(msg);
        if (WIIUPDATE_Get(url, "image/png", path, NULL, NULL, 4 << 20, NULL, NULL) == 0) return image_view(path);
    }
    remove(path);
    char msg[200];
    snprintf(msg, sizeof(msg), "GameTDB has no %s art for %s (or the network is down).", disc ? "disc" : "box", id);
    ui_message(T, C_YELLOW, msg);
    return false;
}

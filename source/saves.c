/*
 * Export every save on the NAND. See saves.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "saves.h"
#include "vfs.h"
#include "ui.h"
#include "ops.h"

/* Title types that keep save data: disc games, channels, WiiWare / game channels. */
static const char *const KINDS[] = { "00010000", "00010001", "00010004" };

static bool mkdirs(const char *dir)
{
    bool is_dir;
    if (vfs_stat(dir, &is_dir, NULL)) return is_dir;
    char parent[VFS_PATH_MAX];
    if (!vfs_parent(dir, parent, sizeof(parent))) return false;
    if (!vfs_is_root(parent) && !mkdirs(parent)) return false;
    return vfs_mkdir(dir) || (vfs_stat(dir, &is_dir, NULL) && is_dir);
}

/* "524d4745" -> "RMGE" when it is printable ASCII. */
static bool game_id(const char *lo, char out[5])
{
    if (strlen(lo) != 8) return false;
    for (int i = 0; i < 4; i++) {
        char h[3] = { lo[i * 2], lo[i * 2 + 1], 0 };
        int c = (int)strtol(h, NULL, 16);
        if (!isalnum(c)) return false;
        out[i] = (char)c;
    }
    out[4] = '\0';
    return true;
}

/* Lets the user pick the SD card or a USB drive. -1 if canceled / none. */
static int pick_target(void)
{
    const char *items[1 + USB_SLOTS];
    int devs[1 + USB_SLOTS], n = 0;
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (d == DEV_GCA || d == DEV_GCB) continue;
        if (!g_devs[d].mounted || vfs_is_readonly(g_devs[d].root)) continue;
        items[n] = g_devs[d].name;
        devs[n++] = d;
    }
    if (!n) {
        ui_message("EXPORT SAVES", C_RED,
                   "There is no writable SD card or USB drive to export to.");
        return -1;
    }
    if (n == 1)
        return devs[0];
    int r = ui_menu("EXPORT SAVES TO", items, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

void saves_export_all(void)
{
    if (!g_devs[DEV_NAND].mounted) {
        ui_message("EXPORT SAVES", C_RED, "The NAND isn't available.");
        return;
    }
    ui_splash("Looking for saves...");

    int cap = 64, n = 0;
    char **srcs = malloc(cap * sizeof(char *));
    char **ids = malloc(cap * sizeof(char *));
    u32 total_files = 0;
    u64 total_bytes = 0;

    for (int k = 0; k < 3 && srcs && ids; k++) {
        char dir[64];
        snprintf(dir, sizeof(dir), "nand:/title/%s", KINDS[k]);
        vfs_entry *list;
        int cnt;
        if (!vfs_list(dir, &list, &cnt))
            continue;
        for (int i = 0; i < cnt; i++) {
            if (!list[i].is_dir) continue;
            char data[VFS_PATH_MAX];
            snprintf(data, sizeof(data), "%s/%s/data", dir, list[i].name);
            u64 b = 0;
            u32 f = 0, d = 0;
            bool is_dir;
            if (!vfs_stat(data, &is_dir, NULL) || !is_dir || !ops_tree_info(data, &b, &f, &d) || !f)
                continue;

            if (n == cap) {
                cap *= 2;
                char **ns = realloc(srcs, cap * sizeof(char *)),
                     **ni = ns ? realloc(ids, cap * sizeof(char *)) : NULL;
                if (ns) srcs = ns;
                if (ni) ids = ni;
                if (!ns || !ni) break;
            }

            char gid[5], label[64];
            if (game_id(list[i].name, gid))
                snprintf(label, sizeof(label), "%s-%s %s", KINDS[k], list[i].name, gid);
            else
                snprintf(label, sizeof(label), "%s-%s", KINDS[k], list[i].name);

            srcs[n] = strdup(data);
            ids[n] = strdup(label);
            if (!srcs[n] || !ids[n]) {
                free(srcs[n]);
                free(ids[n]);
                break;
            }

            n++;
            total_files += f;
            total_bytes += b;
        }
        free(list);
    }

    if (!n) {
        ui_message(
            "EXPORT SAVES", C_YELLOW,
            g_nand_full
                ? "No save data was found on the NAND."
                : "No save data could be read. The NAND permission patch isn't active: launch GodModeWii from the Homebrew Channel.");
        goto out;
    }

    char sz[24], q[300];
    ui_fmt_size(sz, sizeof(sz), total_bytes);
    snprintf(
        q, sizeof(q),
        "Export the saves of %d title(s) (%u files, %s)?\n\nEach title's data folder is copied to "
        "GodModeWii/save-export/<date>/ on the drive you choose next. The NAND is only read.",
        n, (unsigned)total_files, sz);
    if (!ui_confirm("EXPORT ALL SAVES", C_CYAN, q))
        goto out;
    int dev = pick_target();
    if (dev < 0)
        goto out;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char base[VFS_PATH_MAX];
    snprintf(base, sizeof(base), "%sGodModeWii/save-export/%04d%02d%02d-%02d%02d%02d",
             g_devs[dev].root, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
             tm->tm_min, tm->tm_sec);
    char **dsts = calloc(n, sizeof(char *));
    bool ok = dsts != NULL;
    for (int i = 0; ok && i < n; i++) {
        dsts[i] = malloc(VFS_PATH_MAX);
        if (!dsts[i]) {
            ok = false;
            break;
        }
        snprintf(dsts[i], VFS_PATH_MAX, "%s/%s", base, ids[i]);
        ok = mkdirs(dsts[i]);
    }
    if (!ok) {
        ui_message("EXPORT SAVES", C_RED,
                   "Could not create the export folder. Is the drive writable?");
    } else if (ops_copy_each(srcs, dsts, n, "EXPORTING SAVES")) {
        char done[VFS_PATH_MAX + 64];
        snprintf(done, sizeof(done), "Exported the saves of %d title(s) to\n%s", n, base);
        ui_message("SAVES EXPORTED", C_GREEN, done);
    }
    if (dsts)
        for (int i = 0; i < n; i++) free(dsts[i]);
    free(dsts);

out:
    for (int i = 0; i < n; i++) {
        free(srcs[i]);
        free(ids[i]);
    }
    free(srcs);
    free(ids);
}

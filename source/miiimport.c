/*
 * Mii import: files and folders of Miis into the Wii's Mii database. See miiimport.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <malloc.h>

#include "miiimport.h"
#include "miikit/mii.h"
#include "ui.h"
#include "vfs.h"
#include "ops.h"
#include "risk.h"

#define RFL_PATH   "nand:/shared2/menu/FaceLib/RFL_DB.dat"
#define MAX_DEPTH  4
#define MAX_FILES  20000

enum { DEST_PARADE, DEST_PLAZA };

static const char *const EXTS[] = { ".miigx", ".mii", ".rcd", ".rsd", NULL };

bool miiimport_is_name(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; EXTS[i]; i++) {
        size_t e = strlen(EXTS[i]);
        if (n > e && !strcasecmp(name + n - e, EXTS[i]))
            return true;
    }
    return false;
}

static bool mkdirs(const char *dir)
{
    bool is_dir;
    if (vfs_stat(dir, &is_dir, NULL)) return is_dir;
    char parent[VFS_PATH_MAX];
    if (!vfs_parent(dir, parent, sizeof(parent))) return false;
    if (!vfs_is_root(parent) && !mkdirs(parent)) return false;
    return vfs_mkdir(dir) || (vfs_stat(dir, &is_dir, NULL) && is_dir);
}

static int backup_device(void)
{
    if (g_devs[DEV_SD].mounted && !vfs_is_readonly("sd:/"))
        return DEV_SD;
    for (int d = DEV_USB0; d <= DEV_USB7; d++)
        if (g_devs[d].mounted && !vfs_is_readonly(g_devs[d].root))
            return d;
    return -1;
}

/* Copies the Mii database to <drive>:/GodModeWii/mii-backups/<date-time>/. 0 if it was copied,
 * -1 if there is no drive to copy to or the copy failed. `where` gets the folder. */
static int backup_db(char *where, size_t wsz)
{
    int dev = backup_device();
    if (dev < 0)
        return -1;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(where, wsz, "%sGodModeWii/mii-backups/%04d%02d%02d-%02d%02d%02d", g_devs[dev].root,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (!mkdirs(where))
        return -1;
    char *src = (char *)RFL_PATH;
    if (!ops_paste(&src, 1, false, where))
        return -1;
    char copy[VFS_PATH_MAX];
    return vfs_join(copy, sizeof(copy), where, "RFL_DB.dat") && vfs_exists(copy) ? 0 : -1;
}

/* A Mii from a file, checked the way the Wii checks it. 0, or a negative MII_E_* (why: the reason). */
static int read_mii(const char *path, mii *m, const char **why)
{
    u8 buf[96] __attribute__((aligned(32)));
    vfs_file f;
    *why = NULL;
    if (!vfs_open_read(&f, path))
        return MII_E_IO;
    s32 got = vfs_read(&f, buf, sizeof(buf));
    vfs_close(&f);
    if (got < 0)
        return MII_E_IO;
    /* a Mii file is 74 bytes, or 76 with its CRC; anything longer (padding) is read as 74 */
    if (got < MII_SIZE)
        return MII_E_ARG;
    size_t n = f.size == MII_STORE_SIZE ? MII_STORE_SIZE : MII_SIZE;
    int r = mii_import(buf, n, m);
    if (r < 0)
        return r;
    if (mii_id_empty(m->id) && (r = mii_make_id(m)) < 0)
        return r;
    return mii_check(m, why);
}

typedef struct {
    int files, added, duplicate, invalid, bad, full;
    char first_bad[VFS_PATH_MAX];
    const char *first_why;
} tally_t;

static int add_one(mii_db *db, const char *path, int dest, tally_t *t)
{
    mii m;
    const char *why = NULL;
    t->files++;
    int r = read_mii(path, &m, &why);
    if (r == MII_OK) {
        if (dest == DEST_PARADE) {
            m.local_only = 0;       /* the Parade holds Miis that others can see */
            r = mii_hidden_put(db, &m);
        } else {
            r = mii_db_add(db, &m);
        }
    }
    if (r >= 0) {
        t->added++;
        return 0;
    }
    if (r == MII_E_EXISTS) {
        t->duplicate++;
    } else if (r == MII_E_FULL) {
        t->full++;
        return -1;
    } else {
        if (r == MII_E_INVALID || r == MII_E_CRC || r == MII_E_ARG) t->invalid++;
        else t->bad++;
        if (!t->first_bad[0]) {
            snprintf(t->first_bad, sizeof(t->first_bad), "%s", path);
            t->first_why = why ? why : mii_strerror(r);
        }
    }
    return 0;
}

static int count_files(const char *dir, int depth)
{
    vfs_entry *e;
    int cnt, total = 0;
    if (!vfs_list(dir, &e, &cnt))
        return 0;
    for (int i = 0; i < cnt && total < MAX_FILES; i++) {
        char p[VFS_PATH_MAX];
        if (!vfs_join(p, sizeof(p), dir, e[i].name))
            continue;
        if (e[i].is_dir) {
            if (depth < MAX_DEPTH)
                total += count_files(p, depth + 1);
        } else if (miiimport_is_name(e[i].name)) {
            total++;
        }
    }
    free(e);
    return total;
}

static void walk(mii_db *db, const char *dir, int depth, int dest, int total, tally_t *t)
{
    vfs_entry *e;
    int cnt;
    if (!vfs_list(dir, &e, &cnt))
        return;
    for (int i = 0; i < cnt && t->files < MAX_FILES && !t->full; i++) {
        char p[VFS_PATH_MAX];
        if (!vfs_join(p, sizeof(p), dir, e[i].name))
            continue;
        if (e[i].is_dir) {
            if (depth < MAX_DEPTH)
                walk(db, p, depth + 1, dest, total, t);
        } else if (miiimport_is_name(e[i].name)) {
            if (!(t->files % 8)) {
                char s[64];
                snprintf(s, sizeof(s), "Reading Miis... %d of %d", t->files + 1, total);
                ui_splash(s);
            }
            if (add_one(db, p, dest, t) < 0)
                break;
        }
    }
    free(e);
}

static int pick_dest(const char *title, bool parade_first)
{
    const char *items[2];
    items[0] = parade_first ? "Mii Parade (Miis your Wii has met)" : "Mii Channel plaza (100 slots)";
    items[1] = parade_first ? "Mii Channel plaza (100 slots)" : "Mii Parade (Miis your Wii has met)";
    int r = ui_menu(title, items, NULL, 2, 0);
    if (r < 0)
        return -1;
    return (r == 0) == parade_first ? DEST_PARADE : DEST_PLAZA;
}

/* The shared part: confirm, unlock, back up, load the database, add the Miis, save. */
static void run_import(const char *what, int total, int dest, const char *single, const char *dir)
{
    char q[300];
    snprintf(q, sizeof(q),
             "Add %s to the %s?\n\nThe Wii's Mii database is changed. It is copied to GodModeWii/mii-backups/ on "
             "the SD card or a USB drive first. Close the Mii Channel and games first: they would write their own "
             "copy over it.",
             what, dest == DEST_PARADE ? "Mii Parade" : "Mii Channel plaza");
    if (!ui_confirm("IMPORT MIIS", C_YELLOW, q))
        return;
    if (!risk_ensure_unlocked())
        return;

    ui_splash("Backing up the Mii database...");
    char where[VFS_PATH_MAX] = "";
    if (backup_db(where, sizeof(where)) < 0) {
        ui_message("IMPORT MIIS", C_RED,
                   "Couldn't back up the Mii database (no writable SD card or USB drive, or the database can't be "
                   "read), so nothing was changed.");
        return;
    }

    mii_db db = { 0 };
    ui_splash("Reading the Mii database...");
    int r = mii_db_load(&db);
    if (r < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Couldn't read the Mii database: %s.%s", mii_strerror(r),
                 r == MII_E_DENIED ? " Start GodModeWii from the Homebrew Channel." : "");
        ui_message("IMPORT MIIS", C_RED, msg);
        return;
    }

    tally_t t;
    memset(&t, 0, sizeof(t));
    if (single)
        add_one(&db, single, dest, &t);
    else
        walk(&db, dir, 0, dest, total, &t);

    char msg[VFS_PATH_MAX * 2 + 400];
    int p = 0;
    bool saved = true;
    if (t.added) {
        ui_splash("Saving the Mii database...");
        r = mii_db_save(&db);
        if (r == MII_E_IO || r == MII_E_VERIFY)
            r = mii_db_save(&db);
        if (r < 0) {
            saved = false;
            p += snprintf(msg + p, sizeof(msg) - p,
                          "The Mii database couldn't be saved: %s. Nothing was added.\n\nThe backup is in %s", mii_strerror(r),
                          where);
        }
    }
    if (saved) {
        p += snprintf(msg + p, sizeof(msg) - p, "Added %d of %d Mii(s) to the %s.", t.added, t.files,
                      dest == DEST_PARADE ? "Mii Parade" : "Mii Channel plaza");
        if (t.duplicate)
            p += snprintf(msg + p, sizeof(msg) - p, "\n%d already in the plaza.", t.duplicate);
        if (t.invalid + t.bad)
            p += snprintf(msg + p, sizeof(msg) - p, "\n%d the Wii would refuse or couldn't be read.", t.invalid + t.bad);
        if (t.full)
            p += snprintf(msg + p, sizeof(msg) - p, "\nThe plaza is full (100 Miis).");
        if (t.first_bad[0])
            p += snprintf(msg + p, sizeof(msg) - p, "\n\nFirst problem: %s (%s)", vfs_basename(t.first_bad),
                          t.first_why ? t.first_why : "?");
        snprintf(msg + p, sizeof(msg) - p, "\n\nBackup: %s", where);
    }
    mii_db_free(&db);
    ui_message(saved && t.added ? "MIIS IMPORTED" : "IMPORT MIIS", saved && t.added ? C_GREEN : C_YELLOW, msg);
}

void miiimport_file(const char *path)
{
    int dest = pick_dest("ADD THIS MII TO", false);
    if (dest < 0)
        return;
    char what[96];
    snprintf(what, sizeof(what), "\"%s\"", vfs_basename(path));
    run_import(what, 1, dest, path, NULL);
}

void miiimport_folder(const char *dir)
{
    ui_splash("Looking for Mii files...");
    int total = count_files(dir, 0);
    if (!total) {
        ui_message("IMPORT MIIS", C_YELLOW,
                   "There are no Mii files (.miigx, .mii, .rcd, .rsd) in this folder or the ones in it.");
        return;
    }
    int dest = pick_dest("ADD THESE MIIS TO", true);
    if (dest < 0)
        return;
    char what[64];
    snprintf(what, sizeof(what), "%d Mii file(s) from this folder", total);
    run_import(what, total, dest, NULL, dir);
}

/*
 * Favourite homebrew: programs (a .dol / .elf, or an app folder with boot.dol / boot.elf) kept as shortcuts in the
 * HOME menu. One path a line in sd:/apps/GodModeWii/favorites.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "favorites.h"
#include "vfs.h"

#define SETTING "sd:/apps/GodModeWii/favorites.txt"

static char favs[FAV_MAX][VFS_PATH_MAX];
static char names[FAV_MAX][64];
static int nfav = -1;                   /* -1: not read yet */

static bool ends(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !strcasecmp(s + a - b, ext);
}

/* The app's name from the meta.xml beside it, else its folder's (or file's) name. */
static void name_of(const char *prog, char *out, size_t n)
{
    char dir[VFS_PATH_MAX], meta[VFS_PATH_MAX];
    snprintf(out, n, "%s", vfs_basename(prog));
    if (!vfs_parent(prog, dir, sizeof(dir)) || !vfs_join(meta, sizeof(meta), dir, "meta.xml")) return;
    const char *b = vfs_basename(prog);
    if (!strcasecmp(b, "boot.dol") || !strcasecmp(b, "boot.elf")) snprintf(out, n, "%s", vfs_basename(dir));
    FILE *f = fopen(meta, "rb");
    if (!f) return;
    char buf[2048];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    char *a = strstr(buf, "<name>"), *e = a ? strstr(a, "</name>") : NULL;
    if (a && e && e - a > 6) snprintf(out, n, "%.*s", (int)(e - a - 6), a + 6);
}

static void load(void)
{
    nfav = 0;
    FILE *f = fopen(SETTING, "r");
    if (!f) return;
    char line[VFS_PATH_MAX];
    while (nfav < FAV_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        snprintf(favs[nfav], VFS_PATH_MAX, "%s", line);
        name_of(line, names[nfav], sizeof(names[nfav]));
        nfav++;
    }
    fclose(f);
}

static bool save(void)
{
    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/GodModeWii", 0777);
    if (!nfav) return remove(SETTING) == 0 || !vfs_exists(SETTING);
    FILE *f = fopen(SETTING, "w");
    if (!f) return false;
    for (int i = 0; i < nfav; i++) fprintf(f, "%s\n", favs[i]);
    return fclose(f) == 0;
}

bool fav_program(const char *path, char *prog, size_t n)
{
    bool is_dir;
    if (!path[0] || vfs_is_archive(path) || !vfs_stat(path, &is_dir, NULL)) return false;
    if (!is_dir) {
        if (!ends(path, ".dol") && !ends(path, ".elf")) return false;
        snprintf(prog, n, "%s", path);
        return true;
    }
    static const char *const boots[] = { "boot.dol", "boot.elf" };
    for (int i = 0; i < 2; i++)
        if (vfs_join(prog, n, path, boots[i]) && vfs_stat(prog, &is_dir, NULL) && !is_dir) return true;
    return false;
}

void fav_reload(void)
{
    nfav = -1;
}

int fav_count(void)
{
    if (nfav < 0) load();
    return nfav;
}

const char *fav_path(int i) { return i >= 0 && i < fav_count() ? favs[i] : ""; }
const char *fav_name(int i) { return i >= 0 && i < fav_count() ? names[i] : ""; }

int fav_find(const char *prog)
{
    for (int i = 0; i < fav_count(); i++)
        if (!strcasecmp(favs[i], prog)) return i;
    return -1;
}

bool fav_add(const char *prog)
{
    if (fav_find(prog) >= 0) return true;
    if (fav_count() >= FAV_MAX) return false;
    snprintf(favs[nfav], VFS_PATH_MAX, "%s", prog);
    name_of(prog, names[nfav], sizeof(names[nfav]));
    nfav++;
    return save();
}

bool fav_remove(int i)
{
    if (i < 0 || i >= fav_count()) return false;
    for (int k = i; k < nfav - 1; k++) {
        memcpy(favs[k], favs[k + 1], VFS_PATH_MAX);
        memcpy(names[k], names[k + 1], sizeof(names[k]));
    }
    nfav--;
    return save();
}

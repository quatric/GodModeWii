/*
 * setting.txt, the Wii Menu's console settings. See setting.h.
 *
 * The file is always 0x100 bytes: the text XORed with a key that starts at 0x73B5DBFA and is rotated left by
 * one bit after each byte, then zeros (as stored: the first zero byte ends the text). That key repeats every 32 bytes, so it is applied as eight
 * big-endian words. It is written back in place (same file, same owner and permissions).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ogc/isfs.h>
#include <ogc/es.h>

#include "setting.h"
#include "ui.h"
#include "vfs.h"
#include "risk.h"

#define SETTING_PATH "/title/00000001/00000002/data/setting.txt"
#define MAX_FIELDS   16

static char text[SETTING_SIZE + 1];
static bool loaded, ok;

static void crypt(u8 *buf)
{
    static const u32 key[8] = { 0xfaf4e9d3, 0xa74e9c39, 0x73e7ce9d, 0x3b76edda,
                                0xb56bd7ae, 0x5dbb76ed, 0xdbb76fdf, 0xbf7ffefd };
    for (int i = 0; i < SETTING_SIZE / 4; i++) {
        u32 w = ((u32)buf[i * 4] << 24 | (u32)buf[i * 4 + 1] << 16 | (u32)buf[i * 4 + 2] << 8 | buf[i * 4 + 3]) ^
                key[i % 8];
        buf[i * 4] = w >> 24; buf[i * 4 + 1] = w >> 16; buf[i * 4 + 2] = w >> 8; buf[i * 4 + 3] = w;
    }
}

/* The raw (encrypted) file. */
static bool read_raw(u8 *raw)
{
    s32 fd = ISFS_Open(SETTING_PATH, ISFS_OPEN_READ);
    if (fd < 0) return false;
    s32 r = ISFS_Read(fd, raw, SETTING_SIZE);
    ISFS_Close(fd);
    return r == SETTING_SIZE;
}

void setting_reload(void)
{
    static u8 raw[SETTING_SIZE] __attribute__((aligned(32)));
    loaded = true;
    ok = read_raw(raw);
    memset(text, 0, sizeof(text));
    if (!ok) return;
    size_t n = strnlen((const char *)raw, SETTING_SIZE);     /* the padding is zeros as stored */
    crypt(raw);
    memcpy(text, raw, n);
}

bool setting_text(const char **t)
{
    if (!loaded || !ok) setting_reload();      /* again after a failure: ISFS may not have been up yet */
    if (t) *t = text;
    return ok;
}

bool setting_get(const char *key, char *out, size_t n)
{
    if (!setting_text(NULL)) return false;
    size_t kl = strlen(key);
    for (const char *p = text; *p;) {
        const char *end = p + strcspn(p, "\r\n"), *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == kl && !memcmp(p, key, kl)) {
            size_t vl = (size_t)(end - eq - 1);
            if (vl >= n) return false;
            memcpy(out, eq + 1, vl);
            out[vl] = '\0';
            return true;
        }
        p = end + strspn(end, "\r\n");
    }
    return false;
}

int setting_model(void)
{
    u32 id = 0;
    if (ES_GetDeviceID(&id) == 0 && (id >> 28) == 2) return MODEL_VWII;
    char m[32];
    if (!setting_get("MODEL", m, sizeof(m))) return MODEL_UNKNOWN;
    if (!strncmp(m, "RVL-001", 7)) return MODEL_WII;
    if (!strncmp(m, "RVL-101", 7)) return MODEL_FAMILY;
    if (!strncmp(m, "RVL-201", 7)) return MODEL_MINI;
    if (!strncmp(m, "RVT", 3) || !strncmp(m, "RVD", 3)) return MODEL_NDEV;
    return MODEL_UNKNOWN;
}

const char *setting_model_name(int model)
{
    switch (model) {
    case MODEL_WII:    return "Wii";
    case MODEL_FAMILY: return "Wii Family Edition";
    case MODEL_MINI:   return "Wii mini";
    case MODEL_NDEV:   return "development unit (NDEV)";
    case MODEL_VWII:   return "vWii (Wii U)";
    default:           return "unknown";
    }
}

bool setting_is_mini(void) { return setting_model() == MODEL_MINI; }

/* ---------------------------------------------------------------- editing */

typedef struct {
    char key[16], val[48];
    int at, len;            /* where the value is in the text as read */
} field_t;

static int parse(field_t *f)
{
    int n = 0;
    memset(f, 0, sizeof(field_t) * MAX_FIELDS);
    for (const char *p = text; *p && n < MAX_FIELDS;) {
        const char *end = p + strcspn(p, "\r\n"), *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && eq > p && (size_t)(eq - p) < sizeof(f->key) && (size_t)(end - eq - 1) < sizeof(f->val)) {
            memcpy(f[n].key, p, (size_t)(eq - p));
            f[n].key[eq - p] = '\0';
            memcpy(f[n].val, eq + 1, (size_t)(end - eq - 1));
            f[n].val[end - eq - 1] = '\0';
            f[n].at = (int)(eq + 1 - text);
            f[n].len = (int)(end - eq - 1);
            n++;
        }
        p = end + strspn(end, "\r\n");
    }
    return n;
}

/* The text as read with each value replaced in place, so everything else (line ends, other lines) stays
 * byte for byte. The length, or -1 if it doesn't fit in the file. */
static int build(const field_t *f, int n, char *out)
{
    int len = 0, pos = 0, tl = (int)strlen(text);
    for (int i = 0; i <= n; i++) {
        int upto = i < n ? f[i].at : tl;
        const char *v = i < n ? f[i].val : "";
        int vl = (int)strlen(v);
        if (len + (upto - pos) + vl >= SETTING_SIZE) return -1;
        memcpy(out + len, text + pos, (size_t)(upto - pos));
        len += upto - pos;
        memcpy(out + len, v, (size_t)vl);
        len += vl;
        if (i < n) pos = f[i].at + f[i].len;
    }
    out[len] = '\0';
    return len;
}

static const char *const AREAS[] = { "JPN", "USA", "EUR", "KOR", "AUS", "BRA", "TWN", "ROC", "CHN", "HKG",
                                     "ASI", "LTN", "SAF", NULL };
static const char *const VIDEOS[] = { "NTSC", "PAL", "MPAL", NULL };
static const char *const GAMES[] = { "JP", "US", "EU", "KR", "CN", NULL };
static const char *const DVDS[] = { "0", "1", NULL };

static const char *about(const char *key, const char *const **choices)
{
    *choices = NULL;
    if (!strcmp(key, "AREA"))  { *choices = AREAS;  return "The console's region: the Wii Shop and region checks use it."; }
    if (!strcmp(key, "GAME"))  { *choices = GAMES;  return "Which discs and channels the Wii Menu treats as this region's."; }
    if (!strcmp(key, "VIDEO")) { *choices = VIDEOS; return "The video standard games are told the TV uses."; }
    if (!strcmp(key, "DVD"))   { *choices = DVDS;   return "1 on Wiis whose drive can read DVD-Video (early models)."; }
    if (!strcmp(key, "MODEL")) return "The model shown in Wii Settings. RVL-201 is a Wii mini, RVL-101 a Family Edition.";
    if (!strcmp(key, "SERNO")) return "The serial number (without its letters, which are CODE). The Wii Shop uses it.";
    if (!strcmp(key, "CODE"))  return "The serial number's letters (LU, LE, LJ...).";
    if (!strcmp(key, "MPCH"))  return "Which Wi-Fi channels may be used, as a bit mask (0x7FFE: 1 to 14).";
    return "";
}

static bool valid(const char *v)
{
    if (!*v) return false;
    for (; *v; v++)
        if (*v < 0x20 || *v > 0x7E || *v == '=') return false;
    return true;
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

/* The encrypted file as it is and its text, to <drive>/GodModeWii/setting-backups/<date-time>/. */
static bool backup(const u8 *raw, char *where, size_t wsz)
{
    int dev = -1;
    if (g_devs[DEV_SD].mounted && !vfs_is_readonly("sd:/")) dev = DEV_SD;
    for (int d = DEV_USB0; d <= DEV_USB7 && dev < 0; d++)
        if (g_devs[d].mounted && !vfs_is_readonly(g_devs[d].root)) dev = d;
    if (dev < 0) return false;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(where, wsz, "%sGodModeWii/setting-backups/%04d%02d%02d-%02d%02d%02d", g_devs[dev].root,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (!mkdirs(where)) return false;
    char path[VFS_PATH_MAX];
    vfs_file f;
    bool good = vfs_join(path, sizeof(path), where, "setting.txt") && vfs_open_write(&f, path);
    if (good) {
        good = vfs_write(&f, raw, SETTING_SIZE) == SETTING_SIZE;
        good = vfs_close_checked(&f) && good;
    }
    if (good && vfs_join(path, sizeof(path), where, "setting-decrypted.txt") && vfs_open_write(&f, path)) {
        vfs_write(&f, text, (u32)strlen(text));
        vfs_close(&f);
    }
    return good;
}

/* Writes the new text over the file and reads it back. 0, or why not. */
static const char *save(const char *plain)
{
    static u8 raw[SETTING_SIZE] __attribute__((aligned(32))), back[SETTING_SIZE] __attribute__((aligned(32)));
    size_t len = strlen(plain);
    memset(raw, 0, sizeof(raw));
    memcpy(raw, plain, len);
    crypt(raw);
    memset(raw + len, 0, SETTING_SIZE - len);                /* stored as zeros, not encrypted ones */
    if (memchr(raw, 0, len))                                 /* the Wii Menu would stop reading there */
        return "a byte of it encrypts to 0, which ends the file for the Wii Menu; change a value slightly";
    s32 fd = ISFS_Open(SETTING_PATH, ISFS_OPEN_WRITE);
    if (fd < 0) return "couldn't open it for writing";
    s32 w = ISFS_Write(fd, raw, SETTING_SIZE);
    ISFS_Close(fd);
    if (w != SETTING_SIZE) return "the write failed";
    if (!read_raw(back) || memcmp(back, raw, SETTING_SIZE)) return "it reads back different";
    return NULL;
}

void setting_editor(void)
{
    setting_reload();
    if (!ok) {
        ui_message("SETTING.TXT", C_RED, "Couldn't read " SETTING_PATH " from the NAND.");
        return;
    }
    field_t f[MAX_FIELDS], orig[MAX_FIELDS];
    int n = parse(f), sel = 0;
    memcpy(orig, f, sizeof(f));
    char labels[MAX_FIELDS + 1][96];
    const char *items[MAX_FIELDS + 1];
    for (;;) {
        bool dirty = false;
        for (int i = 0; i < n; i++)
            dirty |= strcmp(f[i].val, orig[i].val) != 0;
        for (int i = 0; i < n; i++) {
            bool changed = strcmp(f[i].val, orig[i].val) != 0;
            snprintf(labels[i], sizeof(labels[i]), "%-6.15s %.47s%s", f[i].key, f[i].val, changed ? "   (changed)" : "");
            items[i] = labels[i];
        }
        snprintf(labels[n], sizeof(labels[n]), dirty ? "Save the changes to the NAND" : "(nothing changed yet)");
        items[n] = labels[n];
        bool on[MAX_FIELDS + 1];
        for (int i = 0; i < n; i++) on[i] = true;
        on[n] = dirty;
        ui_clear();
        int r = ui_menu_ex("SETTING.TXT", items, on, n + 1, sel, "A Change   B Leave");
        if (r < 0) {
            if (dirty && !ui_confirm("SETTING.TXT", C_YELLOW, "Leave without saving? The changes are dropped."))
                continue;
            return;
        }
        sel = r;
        if (r < n) {
            const char *const *choices;
            const char *info = about(f[r].key, &choices);
            char v[sizeof(f[r].val)];
            snprintf(v, sizeof(v), "%s", f[r].val);
            if (choices) {
                const char *ci[16];
                int nc = 0, at = 0;
                for (; choices[nc] && nc < 15; nc++) {
                    ci[nc] = choices[nc];
                    if (!strcmp(choices[nc], v)) at = nc;
                }
                ci[nc] = "Type another value";
                char title[80];
                snprintf(title, sizeof(title), "%s: %s", f[r].key, info);
                ui_clear();
                int c = ui_menu(title, ci, NULL, nc + 1, at);
                if (c < 0) continue;
                if (c < nc) snprintf(v, sizeof(v), "%s", choices[c]);
                else if (!ui_text_edit(f[r].key, v, (int)sizeof(v) - 1)) continue;
            } else {
                if (*info) ui_message(f[r].key, C_CYAN, info);
                if (!ui_text_edit(f[r].key, v, (int)sizeof(v) - 1)) continue;
            }
            if (!valid(v)) {
                ui_message("SETTING.TXT", C_YELLOW, "Use letters, digits and punctuation only (no \"=\"), and not nothing.");
                continue;
            }
            char before[sizeof(v)];
            snprintf(before, sizeof(before), "%s", f[r].val);
            snprintf(f[r].val, sizeof(f[r].val), "%s", v);
            char test[SETTING_SIZE];
            if (build(f, n, test) < 0) {
                snprintf(f[r].val, sizeof(f[r].val), "%s", before);
                ui_message("SETTING.TXT", C_YELLOW, "That doesn't fit: setting.txt holds 256 bytes.");
            }
            continue;
        }

        /* save */
        char plain[SETTING_SIZE];
        if (build(f, n, plain) < 0) continue;
        if (!ui_confirm("SAVE SETTING.TXT", C_YELLOW,
                        "Write these settings to the NAND?\n\nA wrong region (AREA / GAME / VIDEO) can make discs, "
                        "channels or the Wii Shop refuse to work, and a wrong serial number can lock you out of the "
                        "Wii Shop. The file is copied to GodModeWii/setting-backups/ on the SD card or a USB drive "
                        "first; copy it back to undo."))
            continue;
        if (!risk_ensure_unlocked()) continue;
        static u8 raw[SETTING_SIZE] __attribute__((aligned(32)));
        char where[VFS_PATH_MAX] = "", msg[VFS_PATH_MAX + 160];
        if (!read_raw(raw) || !backup(raw, where, sizeof(where))) {
            ui_message("SETTING.TXT", C_RED,
                       "Couldn't back up setting.txt (no writable SD card or USB drive?), so nothing was changed.");
            continue;
        }
        const char *why = save(plain);
        if (why) {
            /* put the old file back */
            s32 fd = ISFS_Open(SETTING_PATH, ISFS_OPEN_WRITE);
            if (fd >= 0) { ISFS_Write(fd, raw, SETTING_SIZE); ISFS_Close(fd); }
            snprintf(msg, sizeof(msg), "Couldn't save setting.txt: %s. The old one was written back.\n\nBackup: %s", why,
                     where);
            ui_message("SETTING.TXT", C_RED, msg);
            setting_reload();
            n = parse(f);
            memcpy(orig, f, sizeof(f));
            continue;
        }
        setting_reload();
        n = parse(f);
        memcpy(orig, f, sizeof(f));
        snprintf(msg, sizeof(msg), "Saved. It takes effect when the Wii restarts.\n\nBackup: %s", where);
        ui_message("SETTING.TXT", C_GREEN, msg);
    }
}

#ifdef GMW_AUTOTEST
/* "@setting": the text, the model, then a changed DVD= written, read back and the original put back. */
void setting_autotest(void)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    setting_reload();
    fprintf(lg, "setting read %d model %s mini %d\n", ok, setting_model_name(setting_model()), setting_is_mini());
    for (const char *p = text; *p; p++)
        fputc(*p == '\r' ? '|' : *p == '\n' ? ' ' : *p, lg);
    fputc('\n', lg);
    static u8 raw[SETTING_SIZE] __attribute__((aligned(32)));
    field_t f[MAX_FIELDS];
    int n = parse(f);
    char plain[SETTING_SIZE], before[SETTING_SIZE + 1];
    snprintf(before, sizeof(before), "%s", text);
    bool got = read_raw(raw);
    int k = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(f[i].key, "DVD")) k = i;
    if (got && k >= 0 && build(f, n, plain) >= 0) {
        fprintf(lg, "setting rebuild same %d\n", !strcmp(plain, before));
        snprintf(f[k].val, sizeof(f[k].val), "%s", strcmp(f[k].val, "1") ? "1" : "0");
        build(f, n, plain);
        const char *why = save(plain);
        setting_reload();
        char dvd[8] = "";
        setting_get("DVD", dvd, sizeof(dvd));
        fprintf(lg, "setting save %s, DVD now %s\n", why ? why : "ok", dvd);
        static u8 back[SETTING_SIZE] __attribute__((aligned(32)));
        size_t tl = strlen(text), z = 0;
        if (read_raw(back))
            for (size_t i = tl; i < SETTING_SIZE; i++) z += back[i] == 0;
        fprintf(lg, "setting padding zeros %u of %u\n", (unsigned)z, (unsigned)(SETTING_SIZE - tl));
        s32 fd = ISFS_Open(SETTING_PATH, ISFS_OPEN_WRITE);
        if (fd >= 0) { ISFS_Write(fd, raw, SETTING_SIZE); ISFS_Close(fd); }
        setting_reload();
        fprintf(lg, "setting restored %d\n", !strcmp(text, before));
    }
    fclose(lg);
}
#endif

/*
 * Miis: the Wii's Mii database (or an RFL_DB.dat opened from a drive) as a list, and single Mii
 * files (.miigx / .mii / .rcd / .rsd). Each Mii is drawn whole on the Wii by MiiKit (miiface.c)
 * and shown with its details; 2 changes the expression. Marked Miis are saved to the SD card or a
 * USB drive as /savemiis/<Name>.miigx, the raw 74-byte record in SaveGame Manager GX's layout, so
 * it can import them again. The database itself is only read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#include "mii.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "arcfs.h"
#include "miiface.h"
#include "media.h"

#define RFL_PATH  "nand:/shared2/menu/FaceLib/RFL_DB.dat"
#define RFL_SLOTS 100
#define MII_SIZE  0x4A
#define MII_DIR   "savemiis"
#define MII_EXT   ".miigx"

typedef struct {
    u8 data[MII_SIZE];
    char name[32];       /* for the screen: ASCII */
    char file[64];       /* for the file name: UTF-8, with unsafe characters replaced */
    char creator[32];
    bool marked;
} mii_t;

static mii_t *miis;
static int nmiis;
static bool faces;                      /* the face models are loaded */
static int expression;                  /* the faces', for the session */

static const char *const COLOR_NAMES[12] = { "Red",   "Orange", "Yellow", "Light green", "Green", "Blue",
                                             "Light blue", "Pink", "Purple", "Brown", "White", "Black" };
static const char *const MONTHS[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int fav_color(const u8 *r)
{
    int c = (r[0] << 8 | r[1]) >> 1 & 15;
    return c < 12 ? c : 0;
}

/* Pixel position of a console cell (the console starts 16 pixels in; cells are 8 x 16). */
static int cell_x(int col) { return 16 + col * 8; }
static int cell_y(int row) { return 16 + row * 16; }

#define MII_ROWS (MIIFACE_HEIGHT / 16)  /* the picture's size in console cells */
#define MII_COLS (MIIFACE_WIDTH / 8)

/* The Mii at a cell, on the screen's black; a word in its middle when there isn't one. */
static void draw_face(const u8 *rec, int row, int col)
{
    if (miiface_draw(rec, expression, cell_x(col), cell_y(row)))
        return;
    const char *why = faces ? "(no picture)" : "(no faces)";
    int w = strlen(why);
    ui_color(C_WHITE, false);
    ui_field(row + MII_ROWS / 2, col + (MII_COLS - w) / 2, w, "%s", why);
    ui_reset();
}

/* The face models, or a message saying why there are none. */
static void open_faces(void)
{
    char err[256];
    faces = miiface_open(err, sizeof(err));
    if (!faces)
        ui_message("MII FACES", C_YELLOW, err);
}

/* Once a session: Dolphin's GPU kept a face to itself, so they all come out blank. */
static bool tell_dolphin(void)
{
    static bool told;
    if (told || !miiface_dolphin_blank())
        return false;
    told = true;
    ui_message("MII FACES", C_YELLOW,
               "Dolphin keeps the faces on its GPU. To see them, turn off Graphics > Hacks > "
               "\"Store EFB Copies to Texture Only\".");
    return true;
}

/* Name, creator, birthday, color, height and weight, a line each from (row, col). */
static void draw_details(const u8 *r, const char *name, const char *creator, int row, int col, int width)
{
    unsigned f = r[0] << 8 | r[1];
    unsigned month = f >> 10 & 15, day = f >> 5 & 31;
    char bday[24] = "not set";
    if (month >= 1 && month <= 12 && day) snprintf(bday, sizeof(bday), "%s %u", MONTHS[month], day);
    ui_color(C_WHITE, true);
    ui_field(row, col, width, "%s", name);
    ui_reset();
    ui_color(C_WHITE, false);
    ui_field(row + 1, col, width, "by %s", creator[0] ? creator : "(no creator)");
    ui_field(row + 2, col, width, "Gender    %s", f >> 14 & 1 ? "Female" : "Male");
    ui_field(row + 3, col, width, "Birthday  %s", bday);
    ui_field(row + 4, col, width, "Color     %s", COLOR_NAMES[fav_color(r)]);
    ui_field(row + 5, col, width, "Height    %u / 127", r[0x16] & 0x7F);
    ui_field(row + 6, col, width, "Weight    %u / 127", r[0x17] & 0x7F);
    ui_field(row + 7, col, width, "Favorite  %s", f & 1 ? "Yes" : "No");
    ui_field(row + 8, col, width, "Mii ID    %02X%02X%02X%02X", r[0x18], r[0x19], r[0x1A], r[0x1B]);
    ui_reset();
}

/* 10 UTF-16BE characters. Nintendo's private-use glyphs (hearts, symbols) and anything a FAT
 * name can't hold become '_'; the screen gets '?' for whatever its font can't draw. */
static void utf16_name(const u8 *be, char *out, size_t sz, bool for_file)
{
    size_t o = 0;
    for (int i = 0; i < 10; i++) {
        unsigned c = be[i * 2] << 8 | be[i * 2 + 1];
        if (!c)
            break;
        char buf[4];
        int n = 0;
        if (c < 0x20 || (c >= 0xE000 && c <= 0xF8FF) || (c < 0x80 && strchr("\\/:*?\"<>|", (int)c)))
            buf[n++] = for_file ? '_' : '?';
        else if (c < 0x80)
            buf[n++] = (char)c;
        else if (!for_file)
            buf[n++] = '?';
        else if (c < 0x800) {
            buf[n++] = (char)(0xC0 | c >> 6);
            buf[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            buf[n++] = (char)(0xE0 | c >> 12);
            buf[n++] = (char)(0x80 | (c >> 6 & 0x3F));
            buf[n++] = (char)(0x80 | (c & 0x3F));
        }
        if (o + n >= sz)
            break;
        memcpy(out + o, buf, n);
        o += n;
    }

    /* FAT drops trailing spaces and dots from names */
    while (for_file && o && (out[o - 1] == ' ' || out[o - 1] == '.'))
        o--;
    out[o] = '\0';
}

static bool load(const char *path)
{
    /* ISFS only reads into 32-byte-aligned buffers. */
    static u8 db[4 + RFL_SLOTS * MII_SIZE] __attribute__((aligned(32)));
    vfs_file f;
    nmiis = 0;
    if (!vfs_open_read(&f, path))
        return false;
    s32 got = vfs_read(&f, db, sizeof(db));
    vfs_close(&f);
    if (got != (s32)sizeof(db) || memcmp(db, "RNOD", 4))
        return false;

    miis = calloc(RFL_SLOTS, sizeof(*miis));
    if (!miis)
        return false;

    for (int i = 0; i < RFL_SLOTS; i++) {
        const u8 *r = db + 4 + i * MII_SIZE;
        static const u8 zero[8];
        if (!memcmp(r + 0x18, zero, sizeof(zero)))     /* no CreateID: an empty slot */
            continue;
        mii_t *m = &miis[nmiis++];
        memcpy(m->data, r, MII_SIZE);
        utf16_name(r + 0x02, m->name, sizeof(m->name), false);
        utf16_name(r + 0x02, m->file, sizeof(m->file), true);
        utf16_name(r + 0x36, m->creator, sizeof(m->creator), false);
        if (!m->file[0])
            snprintf(m->file, sizeof(m->file), "Mii %03d", i + 1);
        if (!m->name[0])
            snprintf(m->name, sizeof(m->name), "(no name)");
    }

    return true;
}

static int marked_count(void)
{
    int n = 0;
    for (int i = 0; i < nmiis; i++)
        n += miis[i].marked;
    return n;
}

#define PANEL_W 26                      /* the Mii and its details on the right */

/* Everything is drawn in place, over what was there: clearing the screen first flickers. full
 * clears it, after a dialog. */
static void draw(const char *title, int cursor, int top, int rows, bool full)
{
    int cols = ui_cols(), list_w = cols - PANEL_W - 1, pc = cols - PANEL_W + 1;
    if (full) {
        ui_clear();
        ui_bg(C_BLUE);
        ui_color(C_WHITE, true);
        ui_field(0, 0, cols - 1, " %s", title);
        ui_reset();
    }
    ui_color(C_CYAN, true);
    ui_field(1, 1, cols - 2, "Miis: %d    Marked: %d", nmiis, marked_count());
    ui_reset();
    for (int i = 0; i < rows && top + i < nmiis; i++) {
        const mii_t *m = &miis[top + i];
        if (top + i == cursor) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else
            ui_color(C_WHITE, m->marked);
        ui_field(3 + i, 0, list_w, " %s %-12s  by %s", m->marked ? "[x]" : "[ ]", m->name, m->creator);
        ui_reset();
    }
    const mii_t *m = &miis[cursor];
    draw_details(m->data, m->name, m->creator, 3 + MII_ROWS, pc, PANEL_W - 2);
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, cols - 2, "A Mark   1 Mark all   + Export   2 Face: %s   B Back",
             miiface_expression_name(expression));
    ui_reset();
    draw_face(m->data, 3, pc + (PANEL_W - 2 - MII_COLS) / 2);  /* last: text would draw over it */
}

/* Mounted, writable drives a Mii can go to. */
static int pick_drive(void)
{
    static const int cand[] = { DEV_SD, DEV_GCA, DEV_GCB, DEV_USB0, DEV_USB1, DEV_USB2, DEV_USB3,
                                DEV_USB4, DEV_USB5, DEV_USB6, DEV_USB7 };
    const char *items[11];
    char labels[11][48];
    int devs[11], n = 0;
    for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        int d = cand[i];
        if (!g_devs[d].mounted || vfs_is_virtual(d))
            continue;
        snprintf(labels[n], sizeof(labels[n]), "%s (%s)", g_devs[d].name, g_devs[d].root);
        items[n] = labels[n];
        devs[n++] = d;
    }
    if (!n) {
        ui_message("EXPORT MIIS", C_YELLOW, "No SD card or USB drive to save to.");
        return -1;
    }
    int r = ui_menu("SAVE MIIS TO", items, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

/* "Name.miigx", or "Name (2).miigx" when that exists already. */
static bool unique_path(char *out, size_t sz, const char *dir, const char *base)
{
    for (int i = 1; i < 1000; i++) {
        char name[96];
        if (i == 1) snprintf(name, sizeof(name), "%s%s", base, MII_EXT);
        else        snprintf(name, sizeof(name), "%s (%d)%s", base, i, MII_EXT);
        if (!vfs_join(out, sz, dir, name))
            return false;
        if (!vfs_exists(out))
            return true;
    }
    return false;
}

static void export(int dev, int only)
{
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX], last[VFS_PATH_MAX] = "", failed[VFS_PATH_MAX] = "";
    vfs_join(dir, sizeof(dir), vfs_dev_root(dev), MII_DIR);
    if (!vfs_exists(dir))
        vfs_mkdir(dir);
    int ok = 0, total = 0;
    for (int i = 0; i < nmiis; i++) {
        if (only >= 0 ? i != only : !miis[i].marked)
            continue;
        total++;
        vfs_file f;
        bool good = unique_path(path, sizeof(path), dir, miis[i].file) && vfs_open_write(&f, path);
        if (good) {
            good = vfs_write(&f, miis[i].data, MII_SIZE) == MII_SIZE;
            good = vfs_close_checked(&f) && good;
            if (!good)
                vfs_remove(path, false);
        }
        if (good) {
            ok++;
            snprintf(last, sizeof(last), "%s", path);
        } else if (!failed[0]) {
            snprintf(failed, sizeof(failed), "%s", path[0] ? path : dir);
        }
    }

    char msg[VFS_PATH_MAX * 2 + 96];
    if (ok == total && total == 1)
        snprintf(msg, sizeof(msg), "Saved as %s", last);
    else if (ok == total)
        snprintf(msg, sizeof(msg), "Saved %d Miis to %s/", ok, dir);
    else
        snprintf(msg, sizeof(msg), "Saved %d of %d Miis to %s/\n\nCould not write:\n%s", ok, total,
                 dir, failed);
    if (ok == total) {
        ui_message("DONE", C_GREEN, msg);
    } else {
        ui_alert_window(true);
        ui_message("FINISHED WITH ERRORS", C_RED, msg);
        ui_alert_window(false);
    }
}

static void mii_list(const char *path, const char *title)
{
    ui_splash("Reading the Mii database...");
    if (!load(path)) {
        free(miis);
        miis = NULL;
        char msg[VFS_PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "Couldn't read the Mii database (%s).", path);
        ui_message(title, C_YELLOW, msg);
        return;
    }

    if (!nmiis) {
        free(miis);
        miis = NULL;
        ui_message(title, C_YELLOW, "There are no Miis in this database yet.");
        return;
    }

    open_faces();
    int cursor = 0, top = 0, rows = ui_rows() - 6;
    bool full = true;
    u32 p = 0;
    for (;;) {
        if (cursor < top) top = cursor;
        if (cursor >= top + rows) top = cursor - rows + 1;
        if (full || p)
            draw(title, cursor, top, rows, full);
        full = tell_dolphin();
        if (full)
            continue;
        UI_TEST_SHOT();
        p = input_pressed();
        if (p & WPAD_BUTTON_UP)    cursor = cursor ? cursor - 1 : nmiis - 1;
        if (p & WPAD_BUTTON_DOWN)  cursor = (cursor + 1) % nmiis;
        if (p & WPAD_BUTTON_LEFT)  cursor = cursor > rows ? cursor - rows : 0;
        if (p & WPAD_BUTTON_RIGHT) cursor = cursor + rows < nmiis ? cursor + rows : nmiis - 1;
        if (p & WPAD_BUTTON_A)     miis[cursor].marked = !miis[cursor].marked;
        if (p & WPAD_BUTTON_1) {
            bool all = marked_count() == nmiis;
            for (int i = 0; i < nmiis; i++)
                miis[i].marked = !all;
        }
        if (p & WPAD_BUTTON_PLUS) {
            int dev = pick_drive();
            if (dev >= 0)
                export(dev, marked_count() ? -1 : cursor);
            full = true;
        }
        if (p & WPAD_BUTTON_2)
            expression = (expression + 1) % MIIFACE_EXPRESSIONS;
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            break;
    }

    miiface_close();
    free(miis);
    miis = NULL;
}

void mii_export(void)
{
    mii_list(RFL_PATH, "MIIS");
}

/* One Mii from a file: its face, large, and its details. */
static bool mii_single(const char *path)
{
    u8 rec[MII_SIZE + 2] __attribute__((aligned(32)));
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    s32 got = vfs_read(&f, rec, sizeof(rec));
    vfs_close(&f);
    if (got < MII_SIZE) return false;
    char name[32], creator[32];
    utf16_name(rec + 0x02, name, sizeof(name), false);
    utf16_name(rec + 0x36, creator, sizeof(creator), false);
    if (!name[0]) snprintf(name, sizeof(name), "(no name)");

    open_faces();
    bool full = true;
    u32 p = 0;
    for (;;) {
        int cols = ui_cols();
        if (full) {
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " MII  %s", vfs_basename(path));
            ui_reset();
            draw_details(rec, name, creator, 4, 28, cols - 29);
        }
        if (full || p) {
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 2, 1, cols - 2, "2 Face: %s   B Back", miiface_expression_name(expression));
            ui_reset();
            draw_face(rec, 3, 1);
        }
        full = tell_dolphin();
        if (full)
            continue;
        UI_TEST_SHOT();
        p = input_pressed();
        if (p & WPAD_BUTTON_2)
            expression = (expression + 1) % MIIFACE_EXPRESSIONS;
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            break;
    }
    miiface_close();
    return true;
}

bool mii_file_view(const char *path)
{
    vfs_file f;
    u8 head[4] __attribute__((aligned(32)));
    if (!vfs_open_read(&f, path)) return false;
    s32 got = vfs_read(&f, head, sizeof(head));
    vfs_close(&f);
    if (got == 4 && !memcmp(head, "RNOD", 4)) {
        mii_list(path, "MII DATABASE");
        return true;
    }
    return mii_single(path);
}

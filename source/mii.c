/*
 * Miis: the Wii's Mii database (or an RFL_DB.dat opened from a drive), its plaza or its Mii Parade
 * (the Miis it has met), the ten Miis a Wii Remote carries, and single Mii files (.miigx / .mii /
 * .rcd / .rsd). Each Mii is drawn on the Wii by MiiKit (miiface.c) with its details; 2 changes the
 * expression. Marked Miis are saved as /savemiis/<Name>.miigx, the raw 74-byte record in SaveGame
 * Manager GX's layout. - opens a Mii's actions: a QR code of those 74 bytes (on the screen or as a
 * PNG), a copy to a Wii Remote, a copy onto this Wii, and gold pants.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <png.h>
#include <wiiuse/wpad.h>

#include "mii.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "arcfs.h"
#include "miiface.h"
#include "media.h"
#include "miiimport.h"
#include "miikit/mii.h"
#include "qrcodegen.h"
#include "mii3ds.h"
#include "pda.h"

#define RFL_PATH  "nand:/shared2/menu/FaceLib/RFL_DB.dat"
#define RFL_SLOTS 100
#define MII_DIR   "savemiis"
#define MII_EXT   ".miigx"

typedef struct {
    u8 data[MII_SIZE];
    char name[32];       /* for the screen: ASCII */
    char file[64];       /* for the file name: UTF-8, with unsafe characters replaced */
    char creator[32];
    int slot;            /* where it is: the plaza or Remote slot, or the parade's index */
    bool marked;
} mii_t;

enum { SRC_PLAZA, SRC_PARADE, SRC_REMOTE };

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
static int cell_x(int col) { return ui_cell_x(col); }
static int cell_y(int row) { return ui_cell_y(row); }

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

/* One more Mii in the list, from its 74 bytes. */
static void add(const u8 *r, int slot)
{
    mii_t *m = &miis[nmiis++];
    memcpy(m->data, r, MII_SIZE);
    m->slot = slot;
    utf16_name(r + 0x02, m->name, sizeof(m->name), false);
    utf16_name(r + 0x02, m->file, sizeof(m->file), true);
    utf16_name(r + 0x36, m->creator, sizeof(m->creator), false);
    if (!m->file[0])
        snprintf(m->file, sizeof(m->file), "Mii %03d", slot + 1);
    if (!m->name[0])
        snprintf(m->name, sizeof(m->name), "(no name)");
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
        if (memcmp(r + 0x18, zero, sizeof(zero)))     /* no CreateID: an empty slot */
            add(r, i);
    }

    return true;
}

/* The Mii Parade: the Miis the Wii has met (up to 10,000), kept in the same file after the plaza. */
static bool load_parade(const char *path)
{
    nmiis = 0;
    u8 *file = memalign(32, MII_DB_FILE_SIZE);
    vfs_file f;
    bool ok = file && vfs_open_read(&f, path);
    if (ok) {
        ok = vfs_read(&f, file, MII_DB_FILE_SIZE) == (s32)MII_DB_FILE_SIZE;
        vfs_close(&f);
    }
    mii_db db = { 0 };
    ok = ok && mii_db_attach(&db, file, MII_DB_FILE_SIZE) == MII_OK;
    int n = ok ? mii_hidden_count(&db) : 0;
    miis = ok ? calloc(n ? n : 1, sizeof(*miis)) : NULL;
    if (miis) {
        for (int i = mii_hidden_first(&db); i >= 0 && nmiis < n; i = mii_hidden_next(&db, i)) {
            mii m;
            u8 raw[MII_SIZE];
            if (mii_hidden_get(&db, i, &m) != MII_OK)
                continue;
            mii_pack(&m, raw);
            add(raw, i);
        }
    }
    mii_db_free(&db);
    free(file);
    return miis != NULL;
}

/* The ten slots of a Wii Remote's memory; err says why not. */
static int remote_chan;
static mii_remote remote_set;

static bool load_remote(int chan, int *err)
{
    nmiis = 0;
    remote_chan = chan;
    *err = mii_remote_read(chan, &remote_set);
    if (*err != MII_OK)
        return false;
    miis = calloc(MII_REMOTE_SLOTS, sizeof(*miis));
    if (!miis)
        return false;
    for (int i = 0; i < MII_REMOTE_SLOTS; i++) {
        mii m;
        u8 raw[MII_SIZE];
        if (mii_remote_get(&remote_set, i, &m) != MII_OK)
            continue;
        mii_pack(&m, raw);
        add(raw, i);
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
        ui_field(3 + i, 0, list_w, " %s %c %-12s  %s%s", m->marked ? "[x]" : "[ ]", m->data[0] & 0x40 ? 'F' : 'M',
                 m->name, m->creator[0] ? "by " : "", m->creator);
        ui_reset();
    }
    const mii_t *m = &miis[cursor];
    draw_details(m->data, m->name, m->creator, 3 + MII_ROWS, pc, PANEL_W - 2);
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, cols - 2, "A Mark  1 Mark all  + Export  - Actions  2 Face: %s  B Back",
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

/* ---------------------------------------------------------------- QR codes */

#define QR_QUIET 4                      /* the white border a reader needs, in modules */

/* The Mii as the QR code a 3DS or Wii U's Mii Maker scans (converted to their format and encrypted); its size in
 * modules, 0 if it didn't fit. */
static int mii_qr(const u8 *rec, u8 qr[qrcodegen_BUFFER_LEN_MAX])
{
    static u8 tmp[qrcodegen_BUFFER_LEN_MAX];
    mii m;
    mii_unpack(rec, &m);
    mii_qr_payload(&m, tmp);
    if (!qrcodegen_encodeBinary(tmp, MII3DS_QR_SIZE, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return 0;
    return qrcodegen_getSize(qr);
}

static void qr_show(const mii_t *m)
{
    static u8 qr[qrcodegen_BUFFER_LEN_MAX];
    int n = mii_qr(m->data, qr);
    if (!n) return;
    int side = n + 2 * QR_QUIET, scale = (cell_y(ui_rows() - 3) - cell_y(2)) / side;
    if (scale < 1) scale = 1;
    u8 *rgba = malloc((size_t)side * side * 4);
    if (!rgba) return;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            bool dark = qrcodegen_getModule(qr, x - QR_QUIET, y - QR_QUIET);  /* false outside */
            u8 *p = rgba + (y * side + x) * 4;
            p[0] = p[1] = p[2] = dark ? 0 : 255;
            p[3] = 255;
        }
    int cols = ui_cols();
    ui_clear();
    ui_bg(C_BLUE);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " QR CODE  %s", m->name);
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, cols - 2, "Scan with Mii Maker on a 3DS or Wii U   B Back");
    ui_reset();
    ui_blit_rgba(cell_x(cols / 2) - side * scale / 2, cell_y(2), rgba, side, side, scale);
    free(rgba);
    for (;;) {
        UI_TEST_SHOT();
        if (input_pressed() & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_HOME))
            break;
    }
}

/* The QR code as GodModeWii/miis/<Name>.png, 8 pixels a module. */
static bool qr_save(const mii_t *m, char *out, size_t n)
{
    static u8 qr[qrcodegen_BUFFER_LEN_MAX];
    int size = mii_qr(m->data, qr);
    char dir[VFS_PATH_MAX];
    if (!size || !pda_dir("miis", dir, sizeof(dir), true)) return false;
    snprintf(out, n, "%s/%s.png", dir, m->file);
    const int S = 8, side = (size + 2 * QR_QUIET) * S;
    u8 *row = malloc(side);
    FILE *f = row ? fopen(out, "wb") : NULL;
    png_structp png = f ? png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL) : NULL;
    png_infop info = png ? png_create_info_struct(png) : NULL;
    bool ok = false;
    if (info && !setjmp(png_jmpbuf(png))) {
        png_init_io(png, f);
        png_set_IHDR(png, info, side, side, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++)
                row[x] = qrcodegen_getModule(qr, x / S - QR_QUIET, y / S - QR_QUIET) ? 0 : 255;
            png_write_row(png, row);
        }
        png_write_end(png, NULL);
        ok = true;
    }
    if (png) png_destroy_write_struct(&png, info ? &info : NULL);
    if (f && fclose(f)) ok = false;
    free(row);
    if (!ok && f) remove(out);
    return ok;
}

/* ---------------------------------------------------------------- Wii Remotes */

static bool remote_there(int chan)
{
    u32 type;
    return WPAD_Probe(chan, &type) == WPAD_ERR_NONE;
}

/* A connected Wii Remote, asked for; -1 for none. */
static int pick_remote(const char *title, int skip)
{
    const char *items[4];
    char labels[4][40];
    bool on[4];
    int first = -1;
    for (int c = 0; c < 4; c++) {
        on[c] = c != skip && remote_there(c);
        snprintf(labels[c], sizeof(labels[c]), "Wii Remote %d%s", c + 1, on[c] ? "" : " (not connected)");
        items[c] = labels[c];
        if (on[c] && first < 0) first = c;
    }
    if (first < 0) {
        ui_message(title, C_YELLOW, "No other Wii Remote is connected. Sync one in the Wii Menu first.");
        return -1;
    }
    return ui_menu(title, items, on, 4, first);
}

static void remote_error(const char *title, int err)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "The Wii Remote: %s.", mii_strerror(err));
    ui_message(title, C_RED, msg);
}

/* Its memory as it was, to GodModeWii/wiimote-backups/ first. */
static bool remote_backup(int chan)
{
    static u8 mem[2 * MII_REMOTE_BLOCK] __attribute__((aligned(32)));
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX];
    if (mii_remote_read_raw(chan, mem) != MII_OK || !pda_dir("wiimote-backups", dir, sizeof(dir), true))
        return false;
    for (int i = 1; i < 1000; i++) {
        snprintf(path, sizeof(path), "%s/remote_miis_%03d.bin", dir, i);
        if (!vfs_exists(path)) break;
    }
    vfs_file f;
    if (!vfs_open_write(&f, path)) return false;
    bool ok = vfs_write(&f, mem, sizeof(mem)) == (s32)sizeof(mem);
    return vfs_close_checked(&f) && ok;
}

/* The Mii copied into a free slot of a Wii Remote. */
static void to_remote(const mii_t *src, int skip)
{
    const char *T = "COPY TO A WII REMOTE";
    int chan = pick_remote(T, skip);
    if (chan < 0) return;
    static mii_remote set;
    mii m;
    mii_unpack(src->data, &m);
    int r = mii_remote_read(chan, &set);
    int slot = r == MII_OK ? mii_remote_free_slot(&set) : r;
    for (int i = 0; r == MII_OK && i < MII_REMOTE_SLOTS; i++) {
        mii o;
        if (mii_remote_get(&set, i, &o) == MII_OK && mii_same_id(o.id, m.id)) slot = i;  /* there already */
    }
    if (slot == MII_E_FULL) { ui_message(T, C_YELLOW, "That Wii Remote has 10 Miis already."); return; }
    if (slot < 0) { remote_error(T, slot); return; }
    char q[160];
    snprintf(q, sizeof(q), "Put %s on Wii Remote %d (slot %d)?", src->name, chan + 1, slot + 1);
    if (!ui_confirm(T, C_YELLOW, q)) return;
    ui_splash("Writing to the Wii Remote...");
    if (!remote_backup(chan)) {
        ui_message(T, C_RED, "Couldn't back up the Wii Remote's Miis to the SD card or a USB drive, so nothing was written.");
        return;
    }
    if ((r = mii_remote_put(&set, slot, &m)) == MII_OK)
        r = mii_remote_write(chan, &set);
    if (r != MII_OK) remote_error(T, r);
    else ui_message("DONE", C_GREEN, "Copied. The Mii Channel shows it on the Wii Remote's page.");
}

/* A Mii put into this Wii's database (asks plaza or parade), through a file as any import. */
static void to_wii(const mii_t *m)
{
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX];
    if (!pda_dir("miis", dir, sizeof(dir), true)) return;
    snprintf(path, sizeof(path), "%s/%s%s", dir, m->file, MII_EXT);
    vfs_file f;
    if (!vfs_open_write(&f, path)) return;
    bool ok = vfs_write(&f, m->data, MII_SIZE) == MII_SIZE;
    if (vfs_close_checked(&f) && ok)
        miiimport_file(path);
    vfs_remove(path, false);
}

/* What - does for the Mii under the cursor. True: the list changed (load it again). */
static bool actions(int src, bool nand, mii_t *m)
{
    enum { A_QR, A_PNG, A_REMOTE, A_WII, A_REMOVE, A_GOLD };
    const char *items[6];
    int act[6], n = 0;
    items[n] = "Show as a QR code";               act[n++] = A_QR;
    items[n] = "Save the QR code as a PNG";       act[n++] = A_PNG;
    items[n] = "Copy to a Wii Remote...";         act[n++] = A_REMOTE;
    if (!nand || src != SRC_PLAZA) { items[n] = "Put on this Wii...";  act[n++] = A_WII; }
    if (src == SRC_REMOTE)         { items[n] = "Remove from this Wii Remote"; act[n++] = A_REMOVE; }
    if (nand && src == SRC_PLAZA)  { items[n] = "Gold pants on / off"; act[n++] = A_GOLD; }
    int r = ui_menu(m->name, items, NULL, n, 0);
    if (r < 0) return false;
    char path[VFS_PATH_MAX], msg[VFS_PATH_MAX + 32];
    switch (act[r]) {
    case A_QR:
        qr_show(m);
        return false;
    case A_PNG:
        if (qr_save(m, path, sizeof(path))) {
            snprintf(msg, sizeof(msg), "Saved as %s", path);
            ui_message("DONE", C_GREEN, msg);
        } else
            ui_message("QR CODE", C_RED, "Couldn't save it to the SD card or a USB drive.");
        return false;
    case A_REMOTE:
        to_remote(m, src == SRC_REMOTE ? remote_chan : -1);
        return false;
    case A_WII:
        to_wii(m);
        return false;
    case A_REMOVE: {
        snprintf(msg, sizeof(msg), "Remove %s from Wii Remote %d?", m->name, remote_chan + 1);
        if (!ui_confirm("REMOVE", C_YELLOW, msg)) return false;
        ui_splash("Writing to the Wii Remote...");
        int e = !remote_backup(remote_chan) ? MII_E_IO : mii_remote_remove(&remote_set, m->slot);
        if (e == MII_OK) e = mii_remote_write(remote_chan, &remote_set);
        if (e != MII_OK) remote_error("REMOVE", e);
        return true;
    }
    case A_GOLD: {
        u8 id[8];
        memcpy(id, m->data + 0x18, sizeof(id));
        return miiimport_gold_pants(id);
    }
    }
    return false;
}

static bool load_src(int src, const char *path, int chan)
{
    int err = MII_OK;
    bool ok = src == SRC_PARADE ? load_parade(path) : src == SRC_REMOTE ? load_remote(chan, &err) : load(path);
    if (!ok && src == SRC_REMOTE) {
        free(miis);
        miis = NULL;
        remote_error("WII REMOTE", err);
    }
    return ok;
}

static void mii_list(int src, const char *path, int chan, const char *title)
{
    ui_splash(src == SRC_REMOTE ? "Reading the Wii Remote..." : "Reading the Mii database...");
    if (!load_src(src, path, chan)) {
        free(miis);
        miis = NULL;
        if (src != SRC_REMOTE) {
            char msg[VFS_PATH_MAX + 64];
            snprintf(msg, sizeof(msg), "Couldn't read the Mii database (%s).", path);
            ui_message(title, C_YELLOW, msg);
        }
        return;
    }

    if (!nmiis) {
        free(miis);
        miis = NULL;
        ui_message(title, C_YELLOW, src == SRC_REMOTE ? "There are no Miis on this Wii Remote."
                                    : src == SRC_PARADE ? "The Mii Parade is empty."
                                    : "There are no Miis in this database yet.");
        return;
    }

    open_faces();
    bool nand = path && !strcmp(path, RFL_PATH);    /* the Wii's own database: it can be changed */
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
        if (p & WPAD_BUTTON_MINUS) {
            if (actions(src, nand, &miis[cursor])) {
                free(miis);
                miis = NULL;
                if (!load_src(src, path, chan) || !nmiis)
                    break;
                if (cursor >= nmiis)
                    cursor = nmiis - 1;
            }
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

/* Where the Miis come from: a database's plaza or parade, or a Wii Remote. */
static void mii_sources(const char *path, bool remotes)
{
    int start = 0;
    for (;;) {
        const char *items[6];
        char labels[4][40];
        bool on[6] = { true, true };
        items[0] = "Miis (the Mii Plaza)";
        items[1] = "Mii Parade (Miis met)";
        int n = 2;
        for (int c = 0; remotes && c < 4; c++, n++) {
            on[n] = remote_there(c);
            snprintf(labels[c], sizeof(labels[c]), "Wii Remote %d%s", c + 1, on[n] ? "" : " (not connected)");
            items[n] = labels[c];
        }
        int r = ui_menu("MIIS", items, on, n, start);
        if (r < 0) return;
        start = r;
        char title[32];
        if (r == 0)      mii_list(SRC_PLAZA, path, 0, remotes ? "MIIS" : "MII DATABASE");
        else if (r == 1) mii_list(SRC_PARADE, path, 0, "MII PARADE");
        else {
            snprintf(title, sizeof(title), "WII REMOTE %d", r - 1);
            mii_list(SRC_REMOTE, NULL, r - 2, title);
        }
    }
}

#ifdef GMW_AUTOTEST
/* "@miis": the NAND's plaza and parade counted, the parade's list and a QR code shot, a QR PNG saved. */
void mii_autotest(void)
{
    extern bool ui_test_dump(void);
    FILE *lf = fopen("sd:/gmwtest.log", "a");
    bool ok = load(RFL_PATH);
    if (lf) fprintf(lf, "miis: plaza %d %d\n", ok, nmiis);
    free(miis);
    miis = NULL;
    ok = load_parade(RFL_PATH);
    if (lf) fprintf(lf, "miis: parade %d %d first %s\n", ok, nmiis, nmiis ? miis[0].name : "-");
    if (ok && nmiis) {
        char path[VFS_PATH_MAX];
        bool saved = qr_save(&miis[0], path, sizeof(path));
        if (lf) fprintf(lf, "miis: qr png %d %s\n", saved, path);
        draw("MII PARADE", 0, 0, ui_rows() - 6, true);
        ui_test_dump();
        qr_show(&miis[0]);
    }
    if (lf) fclose(lf);
    free(miis);
    miis = NULL;
}
#endif

void mii_export(void)
{
    mii_sources(RFL_PATH, true);
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
        mii_sources(path, true);
        return true;
    }
    return mii_single(path);
}

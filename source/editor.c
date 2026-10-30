#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "editor.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "risk.h"

/* The same file builds for the Wii and the Wii U: only the button names and
 * the way text reaches the screen differ. */
#ifdef BTN_UP
#define K_UP    BTN_UP
#define K_DOWN  BTN_DOWN
#define K_LEFT  BTN_LEFT
#define K_RIGHT BTN_RIGHT
#define K_PLUS  BTN_PLUS
#define K_MINUS BTN_MINUS
#define K_A     BTN_A
#define K_B     BTN_B
#define K_1     BTN_1
#define K_2     BTN_2
#define K_HOME  BTN_HOME
#define ed_printf ui_printf
#define ed_putc   ui_putc
#else
#define K_UP    WPAD_BUTTON_UP
#define K_DOWN  WPAD_BUTTON_DOWN
#define K_LEFT  WPAD_BUTTON_LEFT
#define K_RIGHT WPAD_BUTTON_RIGHT
#define K_PLUS  WPAD_BUTTON_PLUS
#define K_MINUS WPAD_BUTTON_MINUS
#define K_A     WPAD_BUTTON_A
#define K_B     WPAD_BUTTON_B
#define K_1     WPAD_BUTTON_1
#define K_2     WPAD_BUTTON_2
#define K_HOME  WPAD_BUTTON_HOME
#define ed_printf printf
#define ed_putc   putchar
#endif

/* Files are edited in memory and written back whole, so they are capped. */
#define HEX_MAX_BYTES  (4u * 1024 * 1024)
#define TEXT_MAX_BYTES (1024u * 1024)
#define TEXT_MAX_LINES 20000
#define LINE_EDIT_MAX  400     /* longest line the roller will take */

static bool editable_here(const char *path)
{
    if (vfs_is_readonly(path)) {
        ui_message("READ-ONLY", C_YELLOW, "This file is read-only. Copy it to the SD card or a USB drive to edit it.");
        return false;
    }
#ifndef BTN_UP
    int dev = vfs_dev_of(path);
    if (dev >= 0 && vfs_is_virtual(dev)) {
        ui_message("READ-ONLY", C_YELLOW, "Virtual drives can't be edited. Copy the file to the SD card or a USB drive first.");
        return false;
    }
#endif
    return risk_authorize(path, "edit");
}

static bool write_back(const char *path, const u8 *data, u32 len)
{
    vfs_file f;
    if (!vfs_open_write(&f, path)) {
        ui_message("ERROR", C_RED, "Cannot open the file for writing.");
        return false;
    }
    bool ok = true;
    u32 done = 0;
    while (ok && done < len) {
        u32 n = len - done > 32768 ? 32768 : len - done;
        ok = vfs_write(&f, data + done, n) == (s32)n;
        done += n;
    }
    if (!vfs_close_checked(&f))
        ok = false;
    if (!ok)
        ui_message("ERROR", C_RED, g_write_err ? g_write_err : "Writing failed. The file may be incomplete.");
    return ok;
}

/* Reads the whole file into a 32-byte aligned buffer with `extra` spare bytes. */
static u8 *load_file(const char *path, u32 max, u32 *len, size_t extra)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) {
        ui_message("ERROR", C_RED, "Cannot open the file for reading.");
        return NULL;
    }
    if (f.size > max) {
        char lim[24], msg[160];
        ui_fmt_size(lim, sizeof(lim), max);
        snprintf(msg, sizeof(msg), "This file is too big to edit here (the limit is %s).", lim);
        vfs_close(&f);
        ui_message("TOO BIG", C_YELLOW, msg);
        return NULL;
    }
    u32 size = (u32)f.size;
    u8 *buf = memalign(32, size + extra + 32);
    if (!buf) {
        vfs_close(&f);
        ui_message("ERROR", C_RED, "Out of memory.");
        return NULL;
    }
    u32 got = 0;
    while (got < size) {
        u32 n = size - got > 32768 ? 32768 : size - got;
        s32 r = vfs_read(&f, buf + got, n);
        if (r <= 0)
            break;
        got += r;
    }
    vfs_close(&f);
    if (got != size) {
        free(buf);
        ui_message("ERROR", C_RED, "Reading the file failed part way through.");
        return NULL;
    }
    *len = size;
    return buf;
}

/* ------------------------------------------------------------- hex editor */

void editor_hex(const char *path)
{
    if (!editable_here(path))
        return;
    u32 size;
    u8 *buf = load_file(path, HEX_MAX_BYTES, &size, 0);
    if (!buf)
        return;
    if (size == 0) {
        free(buf);
        ui_message("EMPTY FILE", C_YELLOW, "The hex editor changes bytes that are already there, and this file has none.");
        return;
    }
    u8 *orig = malloc(size);
    if (!orig) {
        free(buf);
        ui_message("ERROR", C_RED, "Out of memory.");
        return;
    }
    memcpy(orig, buf, size);

    int cols = ui_cols(), rows = ui_rows();
    int L = rows - 5;
    const int per = 16;
    u32 cur = 0, top = 0;
    u32 changed = 0;
    bool editing = false, redraw = true;
    const u32 page = (u32)L * per;

    for (;;) {
        if (cur < top) top = cur / per * per;
        if (cur >= top + page) top = cur / per * per - (page - per);
        if (redraw) {
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " HEX EDIT  %s%s", vfs_basename(path), changed ? "  *" : "");
            ui_reset();
            ui_color(C_WHITE, false);
            ui_fill(1, 0, cols - 1, '-');
            for (int r = 0; r < L; r++) {
                u32 base = top + (u32)r * per;
                if (base >= size) break;
                ui_goto(2 + r, 1);
                ui_color(C_CYAN, true);
                ed_printf("%08X ", (unsigned)base);
                for (int c = 0; c < per; c++) {
                    u32 i = base + c;
                    if (i >= size) { ed_printf("   "); continue; }
                    bool on = i == cur;
                    bool dirty = buf[i] != orig[i];
                    if (on) { ui_bg(editing ? C_YELLOW : C_CYAN); ui_color(C_BLACK, false); }
                    else    { ui_bg(C_BLACK); ui_color(dirty ? C_GREEN : C_WHITE, dirty); }
                    ed_printf("%02X", buf[i]);
                    ui_bg(C_BLACK);
                    ed_putc(' ');
                }
                for (int c = 0; c < per && base + c < size; c++) {
                    u32 i = base + c;
                    u8 b = buf[i];
                    if (i == cur) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
                    else          { ui_bg(C_BLACK); ui_color(buf[i] != orig[i] ? C_GREEN : C_YELLOW, false); }
                    ed_putc(b >= 32 && b < 127 ? b : '.');
                }
                ui_bg(C_BLACK);
            }
            ui_reset();
            char tsz[24];
            ui_fmt_size(tsz, sizeof(tsz), size);
            ui_color(C_CYAN, true);
            ui_field(rows - 3, 1, cols - 2, "Offset %08X / %s   Byte %02X (was %02X)   %u changed", (unsigned)cur, tsz,
                     buf[cur], orig[cur], (unsigned)changed);
            if (editing)
                ui_field(rows - 2, 1, cols - 2, "U/D +-1  L/R +-16  +/- +-128  A Keep  B Undo byte");
            else
                ui_field(rows - 2, 1, cols - 2, "Arrows Move  +/- Page  A Edit byte  1 Save  B Close");
            ui_reset();
            redraw = false;
        }

        u32 p = input_pressed();
        if (!p)
            continue;
        if (editing) {
            int d = 0;
            if (p & K_UP)    d = 1;
            if (p & K_DOWN)  d = -1;
            if (p & K_RIGHT) d = 16;
            if (p & K_LEFT)  d = -16;
            if (p & K_PLUS)  d = 128;
            if (p & K_MINUS) d = -128;
            if (d) {
                bool was = buf[cur] != orig[cur];
                buf[cur] = (u8)(buf[cur] + d);
                changed += (buf[cur] != orig[cur]) - was;
                redraw = true;
            }
            if (p & K_A) { editing = false; redraw = true; }
            if (p & (K_B | K_HOME)) {
                if (buf[cur] != orig[cur]) changed--;
                buf[cur] = orig[cur];
                editing = false;
                redraw = true;
            }
            continue;
        }

        u32 c = cur;
        if (p & K_RIGHT) c = cur + 1 < size ? cur + 1 : cur;
        if (p & K_LEFT)  c = cur ? cur - 1 : 0;
        if (p & K_DOWN)  c = cur + per < size ? cur + per : size - 1;
        if (p & K_UP)    c = cur >= (u32)per ? cur - per : cur;
        if (p & K_PLUS)  c = cur + page < size ? cur + page : size - 1;
        if (p & K_MINUS) c = cur >= page ? cur - page : 0;
        if (c != cur) { cur = c; redraw = true; }
        if (p & K_A) { editing = true; redraw = true; }
        if (p & K_1) {
            if (!changed) {
                ui_message("NOTHING TO SAVE", C_CYAN, "No bytes have been changed.");
            } else if (ui_confirm("SAVE", C_YELLOW, "Write the changed bytes back to the file?")) {
                if (write_back(path, buf, size)) {
                    memcpy(orig, buf, size);
                    changed = 0;
                }
            }
            redraw = true;
        }
        if (p & (K_B | K_HOME)) {
            if (!changed || ui_confirm("DISCARD CHANGES", C_YELLOW, "Close without saving the changed bytes?"))
                break;
            redraw = true;
        }
    }
    free(orig);
    free(buf);
}

/* ------------------------------------------------------------ text editor */

typedef struct {
    char **line;
    int n, cap;
} doc;

static bool doc_insert(doc *d, int at, const char *s, size_t len)
{
    if (d->n >= TEXT_MAX_LINES)
        return false;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 256;
        char **nl = realloc(d->line, nc * sizeof(char *));
        if (!nl) return false;
        d->line = nl;
        d->cap = nc;
    }
    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, s, len);
    copy[len] = '\0';
    memmove(d->line + at + 1, d->line + at, (d->n - at) * sizeof(char *));
    d->line[at] = copy;
    d->n++;
    return true;
}

static void doc_free(doc *d)
{
    for (int i = 0; i < d->n; i++) free(d->line[i]);
    free(d->line);
}

static bool save_doc(const char *path, const doc *d, bool crlf, bool final_nl)
{
    size_t total = 0;
    for (int i = 0; i < d->n; i++) total += strlen(d->line[i]) + (crlf ? 2 : 1);
    u8 *out = memalign(32, total + 32);
    if (!out) {
        ui_message("ERROR", C_RED, "Out of memory.");
        return false;
    }
    size_t o = 0;
    for (int i = 0; i < d->n; i++) {
        size_t l = strlen(d->line[i]);
        memcpy(out + o, d->line[i], l);
        o += l;
        if (i + 1 < d->n || final_nl) {
            if (crlf) out[o++] = '\r';
            out[o++] = '\n';
        }
    }
    bool ok = write_back(path, out, (u32)o);
    free(out);
    return ok;
}

void editor_text(const char *path)
{
    if (!editable_here(path))
        return;
    u32 size;
    u8 *buf = load_file(path, TEXT_MAX_BYTES, &size, 0);
    if (!buf)
        return;

    u32 probe = size < 1024 ? size : 1024, odd = 0;
    for (u32 i = 0; i < probe; i++)
        if (buf[i] == 0 || (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t'))
            odd++;
    if (probe && odd * 10 > probe &&
        !ui_confirm("BINARY FILE", C_YELLOW, "This doesn't look like text, and saving it as text could damage it. Edit it anyway?\n\nThe hex editor is the safe way to change it.")) {
        free(buf);
        return;
    }

    /* Split into lines. Line endings are CRLF if the file uses them, and the
     * text is written back in the same style. */
    doc d = {0};
    bool crlf = false, final_nl = size > 0 && buf[size - 1] == '\n';
    u32 s = 0;
    bool ok = true;
    for (u32 i = 0; i <= size && ok; i++) {
        if (i == size || buf[i] == '\n') {
            if (i == size && s == size && size > 0)
                break;                       /* text ended with a newline */
            u32 e = i;
            if (e > s && buf[e - 1] == '\r') { e--; crlf = true; }
            ok = doc_insert(&d, d.n, (const char *)buf + s, e - s);
            s = i + 1;
        }
    }
    free(buf);
    if (ok && d.n == 0)
        ok = doc_insert(&d, 0, "", 0);
    if (!ok) {
        doc_free(&d);
        ui_message("ERROR", C_RED, "Out of memory, or the file has too many lines (limit 20000).");
        return;
    }

    int cols = ui_cols(), rows = ui_rows();
    int width = cols - 8, L = rows - 5;
    int cur = 0, top = 0;
    bool dirty = false, redraw = true;
    char edit_buf[LINE_EDIT_MAX + 1];

    for (;;) {
        if (cur < top) top = cur;
        if (cur >= top + L) top = cur - L + 1;
        if (redraw) {
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " TEXT EDIT  %s%s", vfs_basename(path), dirty ? "  *" : "");
            ui_reset();
            ui_color(C_WHITE, false);
            ui_fill(1, 0, cols - 1, '-');
            for (int r = 0; r < L; r++) {
                int li = top + r;
                if (li >= d.n) break;
                ui_color(C_CYAN, false);
                ui_field(2 + r, 0, 6, "%5d ", li + 1);
                if (li == cur) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
                else           { ui_bg(C_BLACK); ui_color(C_WHITE, true); }
                ui_field(2 + r, 6, width + 1, " %s", d.line[li]);
                ui_bg(C_BLACK);
            }
            ui_reset();
            ui_color(C_CYAN, true);
            ui_field(rows - 3, 1, cols - 2, "Line %d / %d   %s", cur + 1, d.n, crlf ? "CRLF" : "LF");
            ui_field(rows - 2, 1, cols - 2, "U/D Line  L/R Page  A Edit  2 Insert  1 Delete  + Save  B Close");
            ui_reset();
            redraw = false;
        }

        u32 p = input_pressed();
        if (!p)
            continue;
        int c = cur;
        if (p & K_DOWN)  c = cur + 1 < d.n ? cur + 1 : cur;
        if (p & K_UP)    c = cur ? cur - 1 : 0;
        if (p & K_RIGHT) c = cur + L - 1 < d.n ? cur + L - 1 : d.n - 1;
        if (p & K_LEFT)  c = cur >= L - 1 ? cur - (L - 1) : 0;
        if (c != cur) { cur = c; redraw = true; }

        if (p & K_A) {
            if (strlen(d.line[cur]) > LINE_EDIT_MAX) {
                ui_message("LINE TOO LONG", C_YELLOW, "This line is longer than 400 characters, too long to edit with the roller.");
            } else {
                strcpy(edit_buf, d.line[cur]);
                if (ui_text_edit("EDIT LINE", edit_buf, LINE_EDIT_MAX) && strcmp(edit_buf, d.line[cur])) {
                    char *copy = strdup(edit_buf);
                    if (copy) {
                        free(d.line[cur]);
                        d.line[cur] = copy;
                        dirty = true;
                    }
                }
            }
            redraw = true;
        }
        if (p & K_2) {                       /* new empty line below, then edit it */
            if (doc_insert(&d, cur + 1, "", 0)) {
                cur++;
                dirty = true;
            } else {
                ui_message("ERROR", C_RED, "No room for another line.");
            }
            redraw = true;
        }
        if (p & K_1) {
            if (d.n > 1 && ui_confirm("DELETE LINE", C_YELLOW, "Delete this line?")) {
                free(d.line[cur]);
                memmove(d.line + cur, d.line + cur + 1, (d.n - cur - 1) * sizeof(char *));
                d.n--;
                if (cur >= d.n) cur = d.n - 1;
                dirty = true;
            } else if (d.n == 1) {
                d.line[0][0] = '\0';
                dirty = true;
            }
            redraw = true;
        }
        if (p & K_PLUS) {
            if (!dirty)
                ui_message("NOTHING TO SAVE", C_CYAN, "The text hasn't been changed.");
            else if (save_doc(path, &d, crlf, final_nl || size == 0))
                dirty = false;
            redraw = true;
        }
        if (p & (K_B | K_HOME)) {
            if (!dirty || ui_confirm("DISCARD CHANGES", C_YELLOW, "Close without saving your changes?"))
                break;
            redraw = true;
        }
    }
    doc_free(&d);
}

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>

#include "app.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "ops.h"
#include "risk.h"
#include "mii.h"
#include "virt.h"
#include "ftp.h"
#include "arcfs.h"

volatile bool g_quit_request;
volatile bool g_power_request;

/* ---------------------------------------------------------------- state */

static char cwd[VFS_PATH_MAX];       /* "" = drive list */
static vfs_entry *ents;
static bool *marks;
static int nents, nmarks;
static int cursor, scroll;
static int home_cursor, home_scroll;

static char **clip;
static int clip_n;
static bool clip_cut;

static char status_msg[96];
static int status_color = C_WHITE;
static bool nand_warned;
static int exit_req = EXIT_LOADER;

static void status(int color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status(int color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
    va_end(ap);
    status_color = color;
}

static int list_rows(void) { return ui_rows() - 8; }

/* ---------------------------------------------------------------- clipboard */

static void clip_clear(void)
{
    for (int i = 0; i < clip_n; i++)
        free(clip[i]);
    free(clip);
    clip = NULL;
    clip_n = 0;
}

/* Paths of the marked entries, or the entry under the cursor. */
static char **targets(int *count)
{
    int off = clip_n ? 1 : 0;
    int n = nmarks ? nmarks : ((cursor - off >= 0 && cursor - off < nents) ? 1 : 0);
    *count = n;
    if (!n)
        return NULL;
    char **out = calloc(n, sizeof(char *));
    int k = 0;
    for (int i = 0; i < nents && k < n; i++) {
        if (nmarks ? marks[i] : (i == cursor - off)) {
            out[k] = malloc(VFS_PATH_MAX);
            vfs_join(out[k], VFS_PATH_MAX, cwd, ents[i].name);
            k++;
        }
    }
    return out;
}

static void free_paths(char **p, int n)
{
    for (int i = 0; i < n; i++)
        free(p[i]);
    free(p);
}

/* ---------------------------------------------------------------- directory */

static void free_dir(void)
{
    free(ents);
    free(marks);
    ents = NULL;
    marks = NULL;
    nents = nmarks = 0;
}

static bool load_dir(const char *path, const char *select_name)
{
    vfs_entry *list;
    int n;
    if (!vfs_list_visible(path, &list, &n)) {
        if (vfs_is_archive(path)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s.", arcfs_error());
            ui_message("CAN'T OPEN", C_RED, msg);
        } else
            ui_message("ERROR", C_RED, "Cannot open this location.\nThe drive may have been removed - try \"Remount drives\" in the HOME menu.");
        return false;
    }
    free_dir();
    ents = list;
    nents = n;
    marks = calloc(n ? n : 1, sizeof(bool));
    snprintf(cwd, sizeof(cwd), "%s", path);
    cursor = scroll = 0;
    int off = clip_n ? 1 : 0;
    if (select_name)
        for (int i = 0; i < n; i++)
            if (!strcmp(ents[i].name, select_name)) {
                cursor = i + off;
                break;
            }
    return true;
}

static void reload_keep(void)
{
    int c = cursor, s = scroll;
    char keep[VFS_PATH_MAX];
    snprintf(keep, sizeof(keep), "%s", cwd);
    if (load_dir(keep, NULL)) {
        int total = nents + (clip_n ? 1 : 0);
        cursor = c < total ? c : (total ? total - 1 : 0);
        scroll = s;
    }
}

/* ---------------------------------------------------------------- drawing */

/* The drive list warns while the NAND is unlocked; inside a drive, only the NAND itself does. */
static bool here_risky(void)
{
    return risk_unlocked() && (!cwd[0] || vfs_dev_of(cwd) == DEV_NAND);
}

static void draw_header(void)
{
    int cols = ui_cols();
    char left[64], right[48];
    bool risky = here_risky();
    snprintf(left, sizeof(left), " GodModeWii v%s", GMW_VERSION);
    snprintf(right, sizeof(right), "%s%s ", g_is_vwii ? "vWii  " : "",
             risky ? "[ RISKY MODE ON ]" : "[ SAFE MODE ]");
    ui_bg(risky ? C_RED : C_BLUE);
    ui_color(C_WHITE, true);
    int gap = cols - 1 - (int)strlen(left) - (int)strlen(right);
    if (gap < 1) gap = 1;
    ui_field(0, 0, cols - 1, "%s%*s%s", left, gap, "", right);
    ui_reset();
}

static void draw_space_line(int row, int dev, bool tag)
{
    int cols = ui_cols();
    vfs_dev *d = &g_devs[dev];
    ui_reset();
    ui_field(row, 0, cols - 1, "");
    if (tag) {
        ui_color(C_CYAN, true);
        ui_field(row, 1, 6, "%s", d->tag);
    }
    if (!d->space_known) {
        ui_color(C_WHITE, false);
        ui_field(row, 8, cols - 10, "free space unknown");
        ui_reset();
        return;
    }
    char f[24], t[24];
    ui_fmt_size(f, sizeof(f), d->free);
    ui_fmt_size(t, sizeof(t), d->total);
    int used = d->total ? (int)((d->total - d->free) * 100 / d->total) : 0;
    int bw = 22;
    ui_bar(row, 8, bw, used, used < 75 ? C_GREEN : (used < 90 ? C_YELLOW : C_RED));
    ui_color(C_WHITE, true);
    ui_field(row, 8 + bw + 1, cols - 10 - bw, "%s free of %s", f, t);
    ui_reset();
}

static void draw_browser(void)
{
    int cols = ui_cols(), rows = ui_rows();
    int L = list_rows();
    int off = clip_n ? 1 : 0;
    int total = nents + off;

    ui_clear();
    draw_header();

    ui_color(C_CYAN, true);
    int pl = strlen(cwd);
    if (pl > cols - 3)
        ui_field(1, 1, cols - 2, "...%s", cwd + pl - (cols - 6));
    else
        ui_field(1, 1, cols - 2, "%s", cwd);
    ui_reset();
    ui_color(C_WHITE, false);
    ui_fill(2, 0, cols - 1, '-');
    ui_reset();

    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + L) scroll = cursor - L + 1;

    int nw = cols - 1 - 2 - 12;
    for (int i = 0; i < L; i++) {
        int idx = scroll + i;
        int row = 3 + i;
        if (idx >= total) {
            if (total == 0 && i == 0) {
                ui_color(C_WHITE, false);
                ui_field(row, 2, cols - 3, "(empty)");
                ui_reset();
            }
            continue;
        }
        bool sel = idx == cursor;
        if (sel) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }

        if (off && idx == 0) {
            if (!sel) ui_color(C_GREEN, true);
            ui_field(row, 0, cols - 1, "> Paste %d item(s) here  [%s]", clip_n, clip_cut ? "MOVE" : "COPY");
            ui_reset();
            continue;
        }
        int e = idx - off;
        vfs_entry *en = &ents[e];
        if (!sel) {
            if (marks[e]) ui_color(C_GREEN, true);
            else if (en->is_dir) ui_color(C_YELLOW, true);
            else ui_color(C_WHITE, true);
        }
        char sz[24];
        if (en->is_dir) snprintf(sz, sizeof(sz), "<DIR>");
        else ui_fmt_size(sz, sizeof(sz), en->size);
        char shown[256];
        ui_ascii(shown, sizeof(shown), en->name);
        ui_field(row, 0, cols - 1, "%c %-*.*s %11s", marks[e] ? '*' : ' ', nw, nw, shown, sz);
        ui_reset();
    }

    ui_color(C_WHITE, false);
    ui_fill(rows - 5, 0, cols - 1, '-');
    ui_reset();

    int dev = vfs_dev_of(cwd);
    if (dev >= 0 && vfs_is_card(dev)) {
        ui_reset();
        ui_field(rows - 4, 0, cols - 1, "");
        ui_color(C_CYAN, true);
        ui_field(rows - 4, 1, 6, "%s", g_devs[dev].tag);
        ui_color(C_WHITE, false);
        ui_field(rows - 4, 8, cols - 10, "memory card - paste .gci saves here to restore them");
        ui_reset();
    } else if (dev >= 0 && vfs_is_archive(cwd)) {
        ui_reset();
        ui_field(rows - 4, 0, cols - 1, "");
        ui_color(C_CYAN, true);
        bool wfs = arcfs_dev_pinned(dev);
        ui_field(rows - 4, 1, 6, "%s", wfs ? "WFS" : "ARC");
        ui_color(C_WHITE, false);
        ui_field(rows - 4, 8, cols - 10, wfs ? "Wii U WFS volume (read-only) - copy files out to SD/USB"
                                             : "inside an archive (read-only) - copy files out to SD/USB");
        ui_reset();
    } else if (dev >= 0 && vfs_is_virtual(dev)) {
        ui_reset();
        ui_field(rows - 4, 0, cols - 1, "");
        ui_color(C_CYAN, true);
        ui_field(rows - 4, 1, 6, "%s", g_devs[dev].tag);
        ui_color(C_WHITE, false);
        ui_field(rows - 4, 8, cols - 10, "read-only virtual drive - copy files out to SD/USB");
        ui_reset();
    } else if (dev >= 0) {
        draw_space_line(rows - 4, dev, true);
    }

    /* status line: marks / clipboard / last message */
    ui_reset();
    ui_field(rows - 3, 0, cols - 1, "");
    int col = 1;
    if (nmarks) {
        ui_color(C_GREEN, true);
        ui_field(rows - 3, col, 14, "%d marked", nmarks);
        col += 14;
    }
    if (clip_n) {
        ui_color(clip_cut ? C_RED : C_GREEN, true);
        ui_field(rows - 3, col, 20, "Clip: %s %d", clip_cut ? "CUT" : "COPY", clip_n);
        col += 20;
    }
    if (status_msg[0]) {
        ui_color(status_color, true);
        ui_field(rows - 3, col, cols - 1 - col, "%s", status_msg);
    }
    ui_reset();

    ui_color(C_CYAN, true);
    if (off && cursor == 0)
        ui_field(rows - 2, 1, cols - 2, "A Paste  B Back  1 Clear clipboard  2 Menu  + Copy  - Cut");
    else
        ui_field(rows - 2, 1, cols - 2, "A Open  B Back  1 Mark  2 Menu  + Copy  - Cut");
    ui_color(C_WHITE, false);
    ui_field(rows - 1, 1, cols - 2, "HOME System menu    D-Pad L/R Page");
    ui_reset();
}

/* Drives shown on the home screen. */
static int home_list(int *out)
{
    int n = 0;
    for (int i = 0; i < DEV_COUNT; i++)
        if (g_devs[i].always_show || g_devs[i].mounted)
            out[n++] = i;
    return n;
}

static void draw_home(void)
{
    int cols = ui_cols(), rows = ui_rows();
    int devs[DEV_COUNT];
    int n = home_list(devs);
    int vis = rows - 7;
    if (vis < 1) vis = 1;
    if (home_cursor >= n) home_cursor = n - 1;
    if (home_cursor < home_scroll) home_scroll = home_cursor;
    if (home_cursor >= home_scroll + vis) home_scroll = home_cursor - vis + 1;

    ui_clear();
    draw_header();
    ui_color(C_CYAN, true);
    ui_field(1, 1, cols - 2, "Select a drive");
    ui_color(C_WHITE, false);
    ui_fill(2, 0, cols - 1, '-');
    ui_reset();

    for (int i = 0; i < vis && home_scroll + i < n; i++) {
        int idx = home_scroll + i;
        vfs_dev *d = &g_devs[devs[idx]];
        int row = 3 + i;
        bool sel = idx == home_cursor;
        if (sel) { ui_bg(C_CYAN); ui_color(C_BLACK, false); }
        else ui_color(d->mounted ? C_WHITE : C_RED, d->mounted);
        char note[56] = "";
        if (devs[idx] == DEV_NAND)
            snprintf(note, sizeof(note), "%s", risk_unlocked() ? "UNLOCKED - BRICK RISK" : "protected (Konami code to edit)");
        else if (!d->mounted && d->note[0])
            snprintf(note, sizeof(note), "%s", d->note);
        else if (!d->mounted)
            snprintf(note, sizeof(note), "not detected");
        else if (vfs_is_virtual(devs[idx]))
            snprintf(note, sizeof(note), "%s", d->note);
        else if (d->exfat)
            snprintf(note, sizeof(note), "exFAT");
        else if (d->ntfs)
            snprintf(note, sizeof(note), "NTFS");
        else if (d->ext)
            snprintf(note, sizeof(note), "ext%d  read-only", d->ext);
        ui_field(row, 0, cols - 1, " [%-4s]  %-20s %-10s %s", d->tag, d->name, d->root, note);
        ui_reset();
    }

    if (n > vis) {
        ui_color(C_WHITE, false);
        ui_field(rows - 4, 1, cols - 2, "%d/%d", home_cursor + 1, n);
    }
    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2, "A Open drive  HOME System menu");
    if (status_msg[0]) {
        ui_color(status_color, true);
        ui_field(rows - 3, 1, cols - 2, "%s", status_msg);
    }
    ui_reset();
}

/* ---------------------------------------------------------------- viewers */

static void hexview(const char *path)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) {
        ui_message("ERROR", C_RED, "Cannot open the file for reading.");
        return;
    }
    int cols = ui_cols(), rows = ui_rows();
    int L = rows - 5;
    int per = 16;
    u8 *buf = memalign(32, (size_t)L * per + 32);
    u64 off = 0;
    bool redraw = true;
    int got = 0;

    for (;;) {
        if (redraw) {
            vfs_seek(&f, off);
            got = vfs_read(&f, buf, L * per);
            if (got < 0) got = 0;
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " HEX  %s", vfs_basename(path));
            ui_reset();
            ui_color(C_WHITE, false);
            ui_fill(1, 0, cols - 1, '-');
            for (int r = 0; r < L; r++) {
                int base = r * per;
                if (base >= got) break;
                ui_goto(2 + r, 1);
                ui_color(C_CYAN, true);
                printf("%08X ", (unsigned)(off + base));
                ui_color(C_WHITE, false);
                for (int c = 0; c < per; c++) {
                    if (base + c < got) printf("%02X ", buf[base + c]);
                    else printf("   ");
                }
                ui_color(C_YELLOW, false);
                for (int c = 0; c < per && base + c < got; c++) {
                    u8 b = buf[base + c];
                    putchar(b >= 32 && b < 127 ? b : '.');
                }
            }
            ui_reset();
            char tsz[24];
            ui_fmt_size(tsz, sizeof(tsz), f.size);
            ui_color(C_CYAN, true);
            ui_field(rows - 3, 1, cols - 2, "Offset %08X / %s", (unsigned)off, tsz);
            ui_field(rows - 2, 1, cols - 2, "U/D Line  L/R Page  +/- x16 pages  B Close");
            ui_reset();
            redraw = false;
        }
        u32 p = input_pressed();
        u64 page = (u64)L * per;
        u64 last = f.size > page ? ((f.size - 1) / per * per) - (page - per) : 0;
        if (f.size <= page) last = 0;
        u64 o = off;
        if (p & WPAD_BUTTON_DOWN) o += per;
        if (p & WPAD_BUTTON_UP)   o = o >= per ? o - per : 0;
        if (p & WPAD_BUTTON_RIGHT) o += page;
        if (p & WPAD_BUTTON_LEFT)  o = o >= page ? o - page : 0;
        if (p & WPAD_BUTTON_PLUS)  o += page * 16;
        if (p & WPAD_BUTTON_MINUS) o = o >= page * 16 ? o - page * 16 : 0;
        if (o > last) o = last;
        if (o != off) { off = o; redraw = true; }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME | WPAD_BUTTON_A))
            break;
    }
    free(buf);
    vfs_close(&f);
}

/* Plain text viewer: loads the first MiB, wraps it to the screen width and
 * scrolls by line. Tabs are expanded; ui_field folds UTF-8 to ASCII. */
#define TEXT_MAX_BYTES (1024 * 1024)
#define TEXT_MAX_LINES 200000

static void textview(const char *path)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) {
        ui_message("ERROR", C_RED, "Cannot open the file for reading.");
        return;
    }
    u32 want = f.size < TEXT_MAX_BYTES ? (u32)f.size : TEXT_MAX_BYTES;
    u8 *buf = memalign(32, want + 32);
    u32 got = 0;
    while (buf && got < want) {
        u32 n = want - got > 32768 ? 32768 : want - got;
        s32 r = vfs_read(&f, buf + got, n);
        if (r <= 0)
            break;
        got += r;
    }
    bool truncated = f.size > got;
    vfs_close(&f);
    if (!buf) {
        ui_message("ERROR", C_RED, "Out of memory.");
        return;
    }

    /* Warn before dumping a binary file on the screen. */
    u32 probe = got < 1024 ? got : 1024, odd = 0;
    for (u32 i = 0; i < probe; i++)
        if (buf[i] == 0 || (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t'))
            odd++;
    if (probe && odd * 10 > probe &&
        !ui_confirm("BINARY FILE", C_YELLOW, "This doesn't look like text. Open it as text anyway?\n\nThe hex viewer suits it better.")) {
        free(buf);
        return;
    }

    int cols = ui_cols(), rows = ui_rows();
    int width = cols - 2;
    int L = rows - 5;

    u32 cap = 1024, nl = 0;
    u32 *lines = malloc(cap * sizeof(u32));
    if (!lines) { free(buf); return; }
    lines[nl++] = 0;
    int col = 0;
    for (u32 i = 0; i < got && nl < TEXT_MAX_LINES; i++) {
        u8 c = buf[i];
        if (nl + 2 >= cap) {
            cap *= 2;
            u32 *nlines = realloc(lines, cap * sizeof(u32));
            if (!nlines) break;
            lines = nlines;
        }
        if (c == '\n') {
            lines[nl++] = i + 1;
            col = 0;
        } else if (c != '\r') {
            int w = c == '\t' ? 4 - (col % 4) : 1;
            if (col + w > width) {
                lines[nl++] = i;
                col = 0;
                w = c == '\t' ? 4 : 1;
            }
            col += w;
        }
    }
    lines[nl] = got;       /* sentinel (cap always leaves room) */

    u32 top = 0;
    bool redraw = true;
    char row_text[256];
    for (;;) {
        if (redraw) {
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " TEXT  %s", vfs_basename(path));
            ui_reset();
            ui_color(C_WHITE, false);
            ui_fill(1, 0, cols - 1, '-');
            ui_color(C_WHITE, true);
            for (int r = 0; r < L; r++) {
                u32 li = top + r;
                if (li >= nl) break;
                u32 s0 = lines[li], e0 = (li + 1 < nl) ? lines[li + 1] : got;
                int k = 0, cc = 0;
                for (u32 i = s0; i < e0 && k < (int)sizeof(row_text) - 5; i++) {
                    u8 c = buf[i];
                    if (c == '\n' || c == '\r') continue;
                    if (c == '\t') {
                        int w = 4 - (cc % 4);
                        while (w-- && k < (int)sizeof(row_text) - 5) { row_text[k++] = ' '; cc++; }
                    } else {
                        row_text[k++] = (c >= 32 && c != 127) ? (char)c : '.';
                        if ((c & 0xC0) != 0x80)
                            cc++;
                    }
                }
                row_text[k] = '\0';
                ui_field(2 + r, 1, width, "%s", row_text);
            }
            ui_reset();
            int pct = nl > 1 ? (int)((u64)(top + 1) * 100 / nl) : 100;
            char tsz[24];
            ui_fmt_size(tsz, sizeof(tsz), f.size);
            ui_color(C_CYAN, true);
            ui_field(rows - 3, 1, cols - 2, "Line %u / %u  (%d%%)   %s%s", (unsigned)(top + 1), (unsigned)nl, pct, tsz,
                     truncated ? "  - first 1 MiB shown" : "");
            ui_field(rows - 2, 1, cols - 2, "U/D Line  L/R Page  +/- x10 pages  B Close");
            ui_reset();
            redraw = false;
        }
        u32 p = input_pressed();
        u32 t = top, last = nl > (u32)L ? nl - L : 0;
        if (p & WPAD_BUTTON_DOWN)  t += 1;
        if (p & WPAD_BUTTON_UP)    t = t ? t - 1 : 0;
        if (p & WPAD_BUTTON_RIGHT) t += L - 1;
        if (p & WPAD_BUTTON_LEFT)  t = t > (u32)(L - 1) ? t - (L - 1) : 0;
        if (p & WPAD_BUTTON_PLUS)  t += (L - 1) * 10;
        if (p & WPAD_BUTTON_MINUS) t = t > (u32)((L - 1) * 10) ? t - (L - 1) * 10 : 0;
        if (t > last) t = last;
        if (t != top) { top = t; redraw = true; }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME | WPAD_BUTTON_A))
            break;
    }
    free(lines);
    free(buf);
}

static void show_info(char **paths, int n)
{
    u64 bytes = 0;
    u32 files = 0, dirs = 0;
    for (int i = 0; i < n; i++)
        ops_tree_info(paths[i], &bytes, &files, &dirs);
    char sz[32], msg[512];
    ui_fmt_size(sz, sizeof(sz), bytes);
    if (n == 1)
        snprintf(msg, sizeof(msg), "%s\n\n%u file(s), %u folder(s)\nTotal: %s (%llu bytes)",
                 paths[0], (unsigned)files, (unsigned)dirs, sz, (unsigned long long)bytes);
    else
        snprintf(msg, sizeof(msg), "%d items selected\n\n%u file(s), %u folder(s)\nTotal: %s (%llu bytes)",
                 n, (unsigned)files, (unsigned)dirs, sz, (unsigned long long)bytes);
    ui_message("INFO", C_CYAN, msg);
}

static void show_credits(void)
{
    ui_message("ABOUT / CREDITS", C_CYAN,
        "GodModeWii " GMW_VERSION " - a GodMode9-style file manager for Wii and Wii U (vWii).\n"
        "Inspired by GodMode9 (d0k3), GodMode9i (DS-Homebrew), ftpii and WiiXplorer (Dimok, niuus).\n"
        "NAND access + IOS patching: FSToolBox (nickasa), the Lupo96 mod, the WiiDatabase FSToolBox-Mod "
        "and libruntimeiospatch (Nano, Joseph Jordan, damysteryman, DarkMatterCore, megazig, FIX94).\n"
        "Controller input from GlowWii, USB keyboard code from Priiloader (Dacoslove).\n"
        "Built with devkitPPC, libogc and libfat (devkitPro). GPL-2.0. See README.");
}

/* ---------------------------------------------------------------- actions */

static void do_copy_cut(bool cut)
{
    int n;
    char **t = targets(&n);
    if (!n) return;
    /* Cutting from the NAND removes files there: ask for the code now. */
    if (cut && vfs_dev_of(t[0]) == DEV_NAND && !risk_ensure_unlocked()) {
        free_paths(t, n);
        return;
    }
    clip_clear();
    clip = t;
    clip_n = n;
    clip_cut = cut;
    for (int i = 0; i < nents; i++) marks[i] = false;
    nmarks = 0;
    if (cursor == 0) cursor = 0;
    status(cut ? C_RED : C_GREEN, "%d item(s) in clipboard - open the target and press A on the Paste row", n);
}

static void do_clip_clear(void)
{
    clip_clear();
    if (cursor > 0) cursor--;
    if (scroll > 0) scroll--;
    status(C_GREEN, "Clipboard cleared.");
}

static void do_paste(void)
{
    if (!clip_n || !cwd[0]) return;
    if (vfs_is_readonly(cwd)) {
        ui_message("READ-ONLY", C_YELLOW, "You can't paste here: this location is read-only.");
        return;
    }
    bool ok = ops_paste(clip, clip_n, clip_cut, cwd);
    if (ok && clip_cut)
        clip_clear();
    reload_keep();
    status_msg[0] = 0;
}

static void do_delete(void)
{
    int n;
    char **t = targets(&n);
    if (!n) return;
    char msg[300];
    if (n == 1)
        snprintf(msg, sizeof(msg), "Delete \"%s\"?\n\nThis cannot be undone.", vfs_basename(t[0]));
    else
        snprintf(msg, sizeof(msg), "Delete %d items?\n\nThis cannot be undone.", n);
    if (ui_confirm("DELETE", C_RED, msg)) {
        ops_delete(t, n);
        reload_keep();
    }
    free_paths(t, n);
}

static void do_rename(void)
{
    int off = clip_n ? 1 : 0;
    int e = cursor - off;
    if (e < 0 || e >= nents) return;
    char old[VFS_PATH_MAX];
    vfs_join(old, sizeof(old), cwd, ents[e].name);
    if (!risk_authorize(old, "rename"))
        return;
    int dev = vfs_dev_of(cwd);
    char name[VFS_NAME_MAX];
    snprintf(name, sizeof(name), "%s", ents[e].name);
    if (!ui_text_input("RENAME", name, dev == DEV_NAND ? NAND_NAME_MAX : 120))
        return;
    char dst[VFS_PATH_MAX];
    vfs_join(dst, sizeof(dst), cwd, name);
    if (vfs_exists(dst))
        ui_message("ERROR", C_RED, "Something with that name already exists.");
    else if (!vfs_rename(old, dst))
        ui_message("ERROR", C_RED, "Rename failed.");
    else
        status(C_GREEN, "Renamed.");
    load_dir(cwd, name);
}

static void do_mkdir(void)
{
    if (!risk_authorize(cwd, "create a folder"))
        return;
    char name[VFS_NAME_MAX] = "";
    int dev = vfs_dev_of(cwd);
    if (!ui_text_input("NEW FOLDER", name, dev == DEV_NAND ? NAND_NAME_MAX : 120))
        return;
    char dst[VFS_PATH_MAX];
    vfs_join(dst, sizeof(dst), cwd, name);
    if (vfs_exists(dst))
        ui_message("ERROR", C_RED, "Something with that name already exists.");
    else if (!vfs_mkdir(dst))
        ui_message("ERROR", C_RED, "Could not create the folder.");
    else
        vfs_update_space(dev);
    load_dir(cwd, name);
}

static void select_all(bool all)
{
    nmarks = 0;
    for (int i = 0; i < nents; i++) {
        marks[i] = all ? true : false;
        nmarks += marks[i];
    }
}

static void invert_marks(void)
{
    nmarks = 0;
    for (int i = 0; i < nents; i++) {
        marks[i] = !marks[i];
        nmarks += marks[i];
    }
}

enum { A_OPEN, A_PASTE, A_CLIP_CLEAR, A_COPY, A_CUT, A_DELETE, A_RENAME, A_MKDIR, A_INFO, A_HEX, A_TEXT, A_ALL, A_NONE, A_INVERT, A_REFRESH, A_COUNT };

static bool open_archive(const vfs_entry *en)
{
    char file[VFS_PATH_MAX], top[VFS_PATH_MAX + 4];
    if (!vfs_join(file, sizeof(file), cwd, en->name))
        return false;
    snprintf(top, sizeof(top), "%s|/", file);
    if (load_dir(top, NULL))
        return true;
    return false;
}

static void actions_menu(void)
{
    int off = clip_n ? 1 : 0;
    int e = cursor - off;
    bool on_entry = e >= 0 && e < nents;
    int dev = vfs_dev_of(cwd);
    bool ro = vfs_is_readonly(cwd);
    bool card = dev >= 0 && vfs_is_card(dev);

    const char *items[A_COUNT];
    bool en[A_COUNT];
    int code[A_COUNT];
    int n = 0;
    char paste_label[48];
    const char *del_label = "Delete", *ren_label = "Rename";

    items[n] = "Open as archive"; en[n] = on_entry && !nmarks && !ents[e].is_dir; code[n++] = A_OPEN;
    if (clip_n) {
        snprintf(paste_label, sizeof(paste_label), "Paste %d item(s) here", clip_n);
        items[n] = paste_label; en[n] = !ro; code[n++] = A_PASTE;
        items[n] = "Clear clipboard"; en[n] = true; code[n++] = A_CLIP_CLEAR;
    }
    items[n] = "Copy";   en[n] = on_entry || nmarks; code[n++] = A_COPY;
    items[n] = "Cut";    en[n] = (on_entry || nmarks) && !ro; code[n++] = A_CUT;
    items[n] = del_label; en[n] = (on_entry || nmarks) && !ro; code[n++] = A_DELETE;
    items[n] = ren_label; en[n] = on_entry && !nmarks && !ro && !card; code[n++] = A_RENAME;
    items[n] = "New folder"; en[n] = !ro && !card; code[n++] = A_MKDIR;
    items[n] = "Info / folder size"; en[n] = on_entry || nmarks; code[n++] = A_INFO;
    items[n] = "Text viewer"; en[n] = on_entry && !nmarks && !ents[e].is_dir; code[n++] = A_TEXT;
    items[n] = "Hex viewer"; en[n] = on_entry && !nmarks && !ents[e].is_dir; code[n++] = A_HEX;
    items[n] = "Select all"; en[n] = nents > 0; code[n++] = A_ALL;
    items[n] = "Deselect all"; en[n] = nmarks > 0; code[n++] = A_NONE;
    items[n] = "Invert selection"; en[n] = nents > 0; code[n++] = A_INVERT;
    items[n] = "Refresh"; en[n] = true; code[n++] = A_REFRESH;

    int r = ui_menu("ACTIONS", items, en, n, 0);
    if (r < 0)
        return;
    if (!en[r]) {
        if (ro)
            ui_message("READ-ONLY", C_YELLOW, "This is read-only. Copy its files to the SD card or a USB drive instead.");
        return;
    }
    int t; char **tp;
    switch (code[r]) {
    case A_OPEN:    open_archive(&ents[e]); break;
    case A_PASTE:   do_paste(); break;
    case A_CLIP_CLEAR: do_clip_clear(); break;
    case A_COPY:    do_copy_cut(false); break;
    case A_CUT:     do_copy_cut(true); break;
    case A_DELETE:  do_delete(); break;
    case A_RENAME:  do_rename(); break;
    case A_MKDIR:   do_mkdir(); break;
    case A_INFO:
        tp = targets(&t);
        if (t) { show_info(tp, t); free_paths(tp, t); }
        break;
    case A_HEX:
        tp = targets(&t);
        if (t) { hexview(tp[0]); free_paths(tp, t); }
        break;
    case A_TEXT:
        tp = targets(&t);
        if (t) { textview(tp[0]); free_paths(tp, t); }
        break;
    case A_ALL:     select_all(true); break;
    case A_NONE:    select_all(false); break;
    case A_INVERT:  invert_marks(); break;
    case A_REFRESH: vfs_update_space(dev); reload_keep(); break;
    }
}

/* The FTP server may have changed anything: refresh what is on screen. */
static void reload_keep_safe(void)
{
    for (int i = 0; i < DEV_COUNT; i++)
        vfs_update_space(i);
    if (cwd[0])
        reload_keep();
}

/* Returns true if the user wants to leave the program. */
static bool system_menu(void)
{
    enum { M_LOCK, M_REMOUNT, M_FTP_RO, M_FTP_RW, M_FTP_SD, M_FTP_USB, M_MIIS, M_ABOUT,
           M_LOADER, M_WIIMENU, M_RESTART, M_POWER };
    for (;;) {
        if (cwd[0]) draw_browser(); else draw_home();
        const char *items[12];
        int code[12];
        int n = 0;
        if (risk_unlocked() && !risk_forced()) { items[n] = "Lock risky options again"; code[n++] = M_LOCK; }
        items[n] = "Remount drives (SD / Gecko / USB)"; code[n++] = M_REMOUNT;
        items[n] = "FTP server (read-only)";            code[n++] = M_FTP_RO;
        items[n] = "FTP server (read/write)";           code[n++] = M_FTP_RW;
        items[n] = "FTP install server (archives to SD)";  code[n++] = M_FTP_SD;
        items[n] = "FTP install server (archives to USB)"; code[n++] = M_FTP_USB;
        items[n] = "Export Miis";                       code[n++] = M_MIIS;
        items[n] = "About / credits";                   code[n++] = M_ABOUT;
        items[n] = "Exit to loader (Homebrew Channel)"; code[n++] = M_LOADER;
        items[n] = "Exit to Wii Menu";                  code[n++] = M_WIIMENU;
        items[n] = g_is_vwii ? "Restart Wii (to Wii U menu)" : "Restart Wii"; code[n++] = M_RESTART;
        items[n] = "Power off";                         code[n++] = M_POWER;

        /* HOME closes the menu like the Wii's own HOME Menu; leaving is always an explicit choice. */
        int r = ui_menu_ex("SYSTEM", items, NULL, n, 0, "A Select   B / HOME Back");
        if (r < 0)
            return false;
        switch (code[r]) {
        case M_LOCK:
            risk_lock();
            status(C_GREEN, "Risky options locked.");
            return false;
        case M_REMOUNT:
            ui_splash("Looking for drives...");
            vfs_remount();
            clip_clear();
            free_dir();
            cwd[0] = '\0';
            status(C_GREEN, "Drives re-scanned.");
            return false;
        case M_FTP_RO:
        case M_FTP_RW:
        case M_FTP_SD:
        case M_FTP_USB:
            ftp_run(code[r] == M_FTP_RW, code[r] == M_FTP_SD ? FTP_INSTALL_SD : code[r] == M_FTP_USB ? FTP_INSTALL_USB : FTP_INSTALL_OFF);
            reload_keep_safe();
            return false;
        case M_MIIS:
            mii_export();
            return false;
        /* Only shows a box, so B comes back to the SYSTEM menu, redrawn over the browser. */
        case M_ABOUT: show_credits(); break;
        case M_LOADER: exit_req = EXIT_LOADER; return true;
        case M_WIIMENU: exit_req = EXIT_MENU; return true;
        case M_RESTART: exit_req = EXIT_RESTART; return true;
        default: exit_req = EXIT_POWEROFF; return true;
        }
    }
}

/* ---------------------------------------------------------------- main loop */

static void open_drive(int dev)
{
    vfs_dev *d = &g_devs[dev];
    if (dev == DEV_DISC) {          /* discs come and go: re-check on every visit */
        ui_splash("Reading disc...");
        virt_refresh(DEV_DISC);
    }
    if (dev == DEV_MCA || dev == DEV_MCB) {  /* cards get swapped: re-check on every visit */
        ui_splash("Reading memory card...");
        virt_refresh(dev);
    }
    if (dev == DEV_KEYS) {
        ui_splash("Reading console keys...");
        virt_refresh(DEV_KEYS);
        if (d->mounted)
            ui_message("PRIVATE KEYS", C_YELLOW, "These keys are unique to your console. Copy them out for your own backups and keep them private - never share them.");
    }
    if (!d->mounted && d->maybe_wiiu) {
        ui_message("NOT AVAILABLE", C_YELLOW,
                   "This drive isn't FAT32 or exFAT.\n\n"
                   "If a Wii U formatted it, GodModeWii can read it (read-only) with that console's keys: in GodModeU "
                   "on the Wii U side, pick SYSTEM > \"Save Wii U keys for GodModeWii\", or copy otp.bin + seeprom.bin "
                   "to sd:/wfs/. Then use \"Remount drives\" in the HOME menu.");
        return;
    }
    if (!d->mounted) {
        char why[160];
        snprintf(why, sizeof(why), "%s%s%s", d->name, d->note[0] ? ": " : " is not available.", d->note);
        if (!d->note[0])
            snprintf(why, sizeof(why), "Nothing detected here. Insert a card or drive and use \"Remount drives\" in the HOME menu.");
        ui_message("NOT AVAILABLE", C_YELLOW, why);
        return;
    }
    if (0) {
        ui_message("NOT AVAILABLE", C_YELLOW, "Nothing detected here. Insert a card or drive and use \"Remount drives\" in the HOME menu.");
        return;
    }
    if (dev == DEV_NAND && !nand_warned) {
        nand_warned = true;
        if (!g_nand_full)
            ui_message("NOTE", C_YELLOW, "NAND permissions are not patched (HW_AHBPROT is on), so some protected files may be unreadable. Launch from the Homebrew Channel (IOS58) for full access.");
    }
    vfs_update_space(dev);
    load_dir(d->root, NULL);
}

int app_run(void)
{
    bool dirty = true;

    while (!g_quit_request) {
        if (dirty) {
            if (cwd[0]) draw_browser(); else draw_home();
            dirty = false;
        }
        u32 p = input_pressed();
        if (!p)
            continue;
        status_msg[0] = '\0';
        dirty = true;

        if (p & WPAD_BUTTON_HOME) {
            if (system_menu())
                break;
            continue;
        }

        if (!cwd[0]) {
            int devs[DEV_COUNT];
            int n = home_list(devs);
            if (p & WPAD_BUTTON_UP)   home_cursor = (home_cursor + n - 1) % n;
            if (p & WPAD_BUTTON_DOWN) home_cursor = (home_cursor + 1) % n;
            if (p & WPAD_BUTTON_A)    open_drive(devs[home_cursor]);
            continue;
        }

        int off = clip_n ? 1 : 0;
        int total = nents + off;
        int L = list_rows();

        if (p & WPAD_BUTTON_UP)    { if (total) cursor = (cursor + total - 1) % total; }
        if (p & WPAD_BUTTON_DOWN)  { if (total) cursor = (cursor + 1) % total; }
        if (p & WPAD_BUTTON_LEFT)  { cursor -= L - 1; if (cursor < 0) cursor = 0; }
        if (p & WPAD_BUTTON_RIGHT) { cursor += L - 1; if (cursor >= total) cursor = total ? total - 1 : 0; }

        if (p & WPAD_BUTTON_B) {
            char parent[VFS_PATH_MAX];
            char from[VFS_NAME_MAX];
            snprintf(from, sizeof(from), "%s", vfs_basename(cwd));
            char container[VFS_PATH_MAX];
            if (vfs_is_archive(cwd) && arcfs_container(cwd, container, sizeof(container)) && cwd[strlen(container) + 2] == '\0')
                snprintf(from, sizeof(from), "%s", vfs_basename(container));
            if (vfs_parent(cwd, parent, sizeof(parent))) {
                load_dir(parent, from);
            } else {
                free_dir();
                cwd[0] = '\0';
            }
        }

        if (p & WPAD_BUTTON_A) {
            if (off && cursor == 0) {
                do_paste();
            } else if (cursor - off >= 0 && cursor - off < nents) {
                vfs_entry *en = &ents[cursor - off];
                if (en->is_dir) {
                    char path[VFS_PATH_MAX];
                    if (vfs_join(path, sizeof(path), cwd, en->name))
                        load_dir(path, NULL);
                } else {
                    char path[VFS_PATH_MAX];
                    if (vfs_join(path, sizeof(path), cwd, en->name) && arcfs_can_open(path))
                        open_archive(en);
                    else
                        actions_menu();
                }
            }
        }

        if (p & WPAD_BUTTON_1) {
            int e = cursor - off;
            if (e >= 0 && e < nents) {
                marks[e] = !marks[e];
                nmarks += marks[e] ? 1 : -1;
                if (cursor + 1 < total) cursor++;
            } else if (off && cursor == 0) {
                do_clip_clear();
            }
        }
        if (p & WPAD_BUTTON_2)     actions_menu();
        if (p & WPAD_BUTTON_PLUS)  do_copy_cut(false);
        if (p & WPAD_BUTTON_MINUS) {
            if (vfs_is_readonly(cwd))
                ui_message("READ-ONLY", C_YELLOW, "Nothing can be cut from a read-only location. Use + to copy instead.");
            else
                do_copy_cut(true);
        }
    }

    /* Reset / power buttons end up here through the quit flags. */
    if (g_power_request)
        return EXIT_POWEROFF;
    return exit_req;
}

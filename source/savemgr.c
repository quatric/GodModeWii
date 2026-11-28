/*
 * GodModeWii - save manager. See savemgr.h.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * Wii banner.bin (WIBN): title and subtitle in UTF-16 at 0x20 / 0x60, a 192x64 RGB5A3 banner at 0xA0, then up to
 * eight 48x48 RGB5A3 icons; the icon speed at 0x08 (2 bits a frame). GameCube saves: the .gci header's banner
 * format (0x07), image offset (0x2C), icon formats and speeds (0x30 / 0x32, 2 bits an icon) and comment offset
 * (0x3C); a 96x32 banner and 32x32 icons in RGB5A3 or CI8 with an RGB5A3 palette (shared or each its own).
 * The layouts as rom-properties (GerbilSoft) and Dolphin document them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/isfs.h>

#include "savemgr.h"
#include "saves.h"
#include "databin.h"
#include "wadinstall.h"
#include "covers.h"
#include "vfs.h"
#include "ops.h"
#include "ui.h"
#include "input.h"
#include "risk.h"

enum { V_WII, V_GCA, V_GCB, V_DATABIN, V_GCI, V_COUNT };
static const char *const VIEW_NAME[V_COUNT] = { "Wii saves (NAND)", "Memory card A", "Memory card B",
                                                 "data.bin files (SD / USB)", ".gci files (SD / USB)" };

typedef struct {
    u64 tid;
    char path[VFS_PATH_MAX];
    char id6[7];
    char title[66], sub[66];
    u64 size;
    u8 *icon;                       /* first frame, RGBA */
    int iw;                         /* 48 (Wii) or 32 (GameCube); 0: no icon */
} item_t;

static item_t *items;
static int n_items, view;


/* ---------------------------------------------------------------- pictures */

static void px5a3(u16 v, u8 *o)
{
    if (v & 0x8000) {
        o[0] = (u8)(((v >> 10) & 31) * 255 / 31), o[1] = (u8)(((v >> 5) & 31) * 255 / 31), o[2] = (u8)((v & 31) * 255 / 31);
        o[3] = 255;
    } else {
        o[0] = (u8)(((v >> 8) & 15) * 17), o[1] = (u8)(((v >> 4) & 15) * 17), o[2] = (u8)((v & 15) * 17);
        o[3] = (u8)(((v >> 12) & 7) * 255 / 7);
    }
}

/* 4x4 tiles of RGB5A3 */
static void dec_5a3(const u8 *src, int w, int h, u8 *out)
{
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++, src += 2) px5a3(be16(src), out + ((ty + y) * w + tx + x) * 4);
}

/* 8x4 tiles of 8-bit indices into an RGB5A3 palette */
static void dec_ci8(const u8 *src, const u8 *pal, int w, int h, u8 *out)
{
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 8)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++) px5a3(be16(pal + *src++ * 2), out + ((ty + y) * w + tx + x) * 4);
}

/* The banner and icon frames of a save: Wii banner.bin or a GameCube .gci (its header and image area). */
typedef struct {
    u8 *banner;                     /* RGBA, bw x bh */
    int bw, bh;
    u8 *frame[8];                   /* RGBA, iw x iw */
    int delay[8], frames, iw;
} pics_t;

static void pics_free(pics_t *p)
{
    free(p->banner);
    for (int i = 0; i < 8; i++) free(p->frame[i]);
    memset(p, 0, sizeof(*p));
}

static void wii_pics(const u8 *b, u32 size, pics_t *p, bool banner)
{
    memset(p, 0, sizeof(*p));
    if (size < 0x60A0 + 0x1200 || be32(b) != 0x5749424E) return;    /* "WIBN" */
    p->iw = 48;
    if (banner && (p->banner = malloc(192 * 64 * 4))) {
        dec_5a3(b + 0xA0, 192, 64, p->banner);
        p->bw = 192, p->bh = 64;
    }
    int n = (int)((size - 0x60A0) / 0x1200), speed = be16(b + 8);
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        int d = (speed >> (2 * i)) & 3;
        if (i && !d) break;
        if (!(p->frame[i] = malloc(48 * 48 * 4))) break;
        dec_5a3(b + 0x60A0 + i * 0x1200, 48, 48, p->frame[i]);
        p->delay[i] = d ? d : 1;
        p->frames = i + 1;
        if (!banner) break;                                         /* the list only needs the first */
    }
}

/* h: the .gci's 0x40-byte header; img: its data from the image offset on, img_len bytes of it */
static void gc_pics(const u8 *h, const u8 *img, u32 img_len, pics_t *p, bool banner)
{
    memset(p, 0, sizeof(*p));
    u32 off = 0;
    int bfmt = h[7] & 3, ifmt = be16(h + 0x30), speed = be16(h + 0x32);
    if (bfmt == 1 && img_len >= 96 * 32 + 512) {
        if (banner && (p->banner = malloc(96 * 32 * 4))) dec_ci8(img, img + 96 * 32, 96, 32, p->banner);
        off = 96 * 32 + 512;
    } else if (bfmt == 2 && img_len >= 96 * 32 * 2) {
        if (banner && (p->banner = malloc(96 * 32 * 4))) dec_5a3(img, 96, 32, p->banner);
        off = 96 * 32 * 2;
    }
    if (p->banner) p->bw = 96, p->bh = 32;
    u32 at[8];
    int fmt[8];
    for (int i = 0; i < 8; i++) {                                   /* where each icon is */
        fmt[i] = (ifmt >> (2 * i)) & 3;
        at[i] = off;
        off += fmt[i] == 2 ? 2048 : fmt[i] == 1 ? 1024 : fmt[i] == 3 ? 1024 + 512 : 0;
    }
    u32 shared = off;                                               /* after the icons: the shared palette */
    p->iw = 32;
    for (int i = 0; i < 8; i++) {
        int d = (speed >> (2 * i)) & 3;
        if (!fmt[i] || (i && !d)) break;
        u32 need = at[i] + (fmt[i] == 2 ? 2048 : 1024);
        const u8 *pal = fmt[i] == 3 ? img + at[i] + 1024 : img + shared;
        if (need > img_len || (fmt[i] != 2 && (u32)(pal - img) + 512 > img_len)) break;
        if (!(p->frame[i] = malloc(32 * 32 * 4))) break;
        if (fmt[i] == 2) dec_5a3(img + at[i], 32, 32, p->frame[i]);
        else dec_ci8(img + at[i], pal, 32, 32, p->frame[i]);
        p->delay[i] = d ? d : 1;
        p->frames = i + 1;
        if (!banner) break;
    }
}

/* UTF-16BE (Wii) or 8-bit (GameCube) text to printable ASCII */
static void text16(char *out, size_t n, const u8 *s, int chars)
{
    size_t k = 0;
    for (int i = 0; i < chars && k + 1 < n; i++) {
        u16 c = be16(s + i * 2);
        if (!c) break;
        out[k++] = c == '\n' ? ' ' : c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    out[k] = 0;
}

static void text8(char *out, size_t n, const u8 *s, int chars)
{
    size_t k = 0;
    for (int i = 0; i < chars && s[i] && k + 1 < n; i++) out[k++] = s[i] >= 0x20 && s[i] < 0x7F ? (char)s[i] : '?';
    out[k] = 0;
}

/* ---------------------------------------------------------------- reading files */

static u8 *read_file(const char *path, u32 *len, u32 max)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    u32 n = f.size < max ? (u32)f.size : max;
    u8 *b = memalign(32, n + 32);
    if (b && vfs_read(&f, b, n) != (s32)n) { free(b); b = NULL; }
    vfs_close(&f);
    if (b) *len = n;
    return b;
}

/* A .gci: its header and pictures (icon only, or banner too) and comment. */
static bool gci_load(const char *path, item_t *it, pics_t *p, bool banner)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return false;
    u8 h[0x40] __attribute__((aligned(32)));
    bool ok = vfs_read(&f, h, 0x40) == 0x40;
    if (ok && it) {
        memcpy(it->id6, h, 6);
        it->id6[6] = 0;
        for (int i = 0; i < 6; i++) if (!isalnum((unsigned char)it->id6[i])) it->id6[i] = '_';
        it->size = f.size;
    }
    u32 io = be32(h + 0x2C), co = be32(h + 0x3C);
    u8 *buf = ok ? memalign(32, 0x8000) : NULL;
    if (buf && it && co != 0xFFFFFFFF && vfs_seek(&f, 0x40 + (u64)co) && vfs_read(&f, buf, 64) == 64) {
        text8(it->title, sizeof(it->title), buf, 32);
        text8(it->sub, sizeof(it->sub), buf + 32, 32);
    }
    if (buf && p && io != 0xFFFFFFFF && vfs_seek(&f, 0x40 + (u64)io)) {
        s32 r = vfs_read(&f, buf, 0x8000);
        if (r > 0) gc_pics(h, buf, (u32)r, p, banner);
    }
    free(buf);
    vfs_close(&f);
    return ok;
}

/* ---------------------------------------------------------------- the lists */

static void clear_items(void)
{
    for (int i = 0; i < n_items; i++) free(items[i].icon);
    free(items);
    items = NULL;
    n_items = 0;
}

static item_t *add_item(void)
{
    item_t *n = realloc(items, (n_items + 1) * sizeof(item_t));
    if (!n) return NULL;
    items = n;
    memset(&items[n_items], 0, sizeof(item_t));
    return &items[n_items++];
}

static void icon_from(item_t *it, pics_t *p)
{
    if (p->frames) {
        it->icon = p->frame[0];
        it->iw = p->iw;
        p->frame[0] = NULL;
    }
    pics_free(p);
}

static void load_wii(void)
{
    static const char *const KINDS[] = { "00010000", "00010001", "00010004" };
    for (int k = 0; k < 3; k++) {
        char dir[48];
        snprintf(dir, sizeof(dir), "nand:/title/%s", KINDS[k]);
        vfs_entry *l;
        int n;
        if (!vfs_list(dir, &l, &n)) continue;
        for (int i = 0; i < n; i++) {
            if (!l[i].is_dir) continue;
            char data[VFS_PATH_MAX], bpath[VFS_PATH_MAX + 16];
            snprintf(data, sizeof(data), "%s/%s/data", dir, l[i].name);
            u64 b = 0;
            u32 f = 0, d = 0;
            bool is_dir;
            if (!vfs_stat(data, &is_dir, NULL) || !is_dir || !ops_tree_info(data, &b, &f, &d) || !f) continue;
            item_t *it = add_item();
            if (!it) break;
            snprintf(it->path, sizeof(it->path), "%s", data);
            it->tid = strtoull(KINDS[k], NULL, 16) << 32 | strtoul(l[i].name, NULL, 16);
            it->size = b;
            for (int c = 0; c < 4; c++) it->id6[c] = (char)(it->tid >> (24 - 8 * c));
            /* the save folder's group is the maker code: the rest of the six-character ID */
            u32 uid;
            u16 gid = 0;
            u8 a, o, g, t;
            ISFS_GetAttr(data + 5, &uid, &gid, &a, &o, &g, &t);
            it->id6[4] = (char)(gid >> 8), it->id6[5] = (char)gid;
            for (int c = 0; c < 6; c++) if (!isalnum((unsigned char)it->id6[c])) it->id6[c] = c < 4 ? '_' : 0;
            snprintf(bpath, sizeof(bpath), "%s/banner.bin", data);
            u32 bl = 0;
            u8 *bn = read_file(bpath, &bl, 0xF0A0);
            if (bn && bl >= 0xA0 && be32(bn) == 0x5749424E) {
                text16(it->title, sizeof(it->title), bn + 0x20, 32);
                text16(it->sub, sizeof(it->sub), bn + 0x60, 32);
                pics_t p;
                wii_pics(bn, bl, &p, false);
                icon_from(it, &p);
            } else {
                snprintf(it->title, sizeof(it->title), "%s-%s", KINDS[k], l[i].name);
            }
            free(bn);
        }
        free(l);
    }
}

static void load_card(int dev)
{
    if (!g_devs[dev].mounted) return;
    vfs_entry *l;
    int n;
    if (!vfs_list(g_devs[dev].root, &l, &n)) return;
    for (int i = 0; i < n; i++) {
        if (l[i].is_dir) continue;
        item_t *it = add_item();
        if (!it) break;
        snprintf(it->path, sizeof(it->path), "%s%s", g_devs[dev].root, l[i].name);
        pics_t p;
        if (!gci_load(it->path, it, &p, false)) { n_items--; continue; }
        icon_from(it, &p);
        if (!it->title[0]) snprintf(it->title, sizeof(it->title), "%s", l[i].name);
    }
    free(l);
}

/* data.bin files: <drive>/private/wii/title/<ID>/data.bin, and the same inside GodModeWii/save-export/<date>/ */
static void scan_databins(const char *titles)
{
    vfs_entry *l;
    int n;
    if (!vfs_list(titles, &l, &n)) return;
    for (int i = 0; i < n; i++) {
        if (!l[i].is_dir) continue;
        char p[VFS_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s/data.bin", titles, l[i].name);
        u64 tid;
        u8 *bn;
        u32 bl;
        if (!databin_banner(p, &tid, &bn, &bl)) continue;
        item_t *it = add_item();
        if (!it) { free(bn); break; }
        snprintf(it->path, sizeof(it->path), "%s", p);
        it->tid = tid;
        for (int c = 0; c < 4; c++) it->id6[c] = (char)(tid >> (24 - 8 * c));
        text16(it->title, sizeof(it->title), bn + 0x20, 32);
        text16(it->sub, sizeof(it->sub), bn + 0x60, 32);
        pics_t pc;
        wii_pics(bn, bl, &pc, false);
        icon_from(it, &pc);
        free(bn);
        bool d;
        vfs_stat(p, &d, &it->size);
    }
    free(l);
}

static void load_files(bool gci)
{
    for (int dev = DEV_SD; dev <= DEV_USB7; dev++) {
        if (dev == DEV_GCA || dev == DEV_GCB || !g_devs[dev].mounted) continue;
        const char *root = g_devs[dev].root;
        char d[VFS_PATH_MAX];
        if (!gci) {
            snprintf(d, sizeof(d), "%sprivate/wii/title", root);
            scan_databins(d);
            snprintf(d, sizeof(d), "%sGodModeWii/save-export", root);
            vfs_entry *l;
            int n;
            if (vfs_list(d, &l, &n)) {
                for (int i = 0; i < n; i++) {
                    char t[VFS_PATH_MAX];
                    snprintf(t, sizeof(t), "%s/%s/private/wii/title", d, l[i].name);
                    if (l[i].is_dir) scan_databins(t);
                }
                free(l);
            }
            continue;
        }
        static const char *const dirs[] = { "GodModeWii/gci", "gci", "card", "" };   /* "card": GCMM's folder */
        for (int k = 0; k < 4; k++) {
            snprintf(d, sizeof(d), "%s%s", root, dirs[k]);
            vfs_entry *l;
            int n;
            if (!vfs_list(d, &l, &n)) continue;
            for (int i = 0; i < n; i++) {
                size_t nl = strlen(l[i].name);
                if (l[i].is_dir || nl < 5 || strcasecmp(l[i].name + nl - 4, ".gci")) continue;
                item_t *it = add_item();
                if (!it) break;
                snprintf(it->path, sizeof(it->path), "%s%s%s", d, dirs[k][0] ? "/" : "", l[i].name);
                pics_t p;
                if (!gci_load(it->path, it, &p, false)) { n_items--; continue; }
                icon_from(it, &p);
                if (!it->title[0]) snprintf(it->title, sizeof(it->title), "%s", l[i].name);
            }
            free(l);
        }
    }
}

static void load_view(void)
{
    clear_items();
    ui_splash("Reading the saves...");
    if (view == V_WII) load_wii();
    else if (view == V_GCA) load_card(DEV_GCA);
    else if (view == V_GCB) load_card(DEV_GCB);
    else load_files(view == V_GCI);
}

/* ---------------------------------------------------------------- drawing */

#define ROWS_PER 3

static void draw_list(int cursor, int top, int per_page)
{
    int cols = ui_cols();
    ui_clear();
    ui_bg(C_BLUE);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " SAVE MANAGER - %s", VIEW_NAME[view]);
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(1, 1, cols - 2, "%d save(s)%s", n_items,
             view == V_WII && !g_nand_full ? "   (NAND permission patch off: launch from the Homebrew Channel)" : "");
    ui_reset();
    for (int i = 0; i < per_page && top + i < n_items; i++) {
        const item_t *it = &items[top + i];
        int r = 3 + i * ROWS_PER;
        bool sel = top + i == cursor;
        char sz[24];
        ui_fmt_size(sz, sizeof(sz), it->size);
        if (sel) { ui_bg(C_CYAN); ui_color(C_BLACK, false); } else ui_color(C_WHITE, true);
        ui_field(r, 8, cols - 9, " %s", it->title);
        if (!sel) ui_color(C_WHITE, false);
        ui_field(r + 1, 8, cols - 9, " %s", it->sub);
        if (!sel) ui_color(C_CYAN, false);
        ui_field(r + 2, 8, cols - 9, " %s   %s", it->id6, sz);
        ui_reset();
    }
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, cols - 2, "A Open  + Next list  1 Box art  2 Disc art%s  B Back",
             view == V_WII ? "  - Export all" : "");
    ui_reset();
    for (int i = 0; i < per_page && top + i < n_items; i++) {      /* pictures last: text would draw over them */
        const item_t *it = &items[top + i];
        if (!it->icon) continue;
        int y = ui_cell_y(3 + i * ROWS_PER);
        ui_blit_rgba(ui_cell_x(1) + (48 - it->iw) / 2, y + (48 - it->iw) / 2, it->icon, it->iw, it->iw, 1);
    }
}

/* The banner, the icon animating and what is known about the save, until B. */
static void show_banner(const item_t *it)
{
    pics_t p;
    memset(&p, 0, sizeof(p));
    if (view == V_WII || view == V_DATABIN) {
        u8 *bn = NULL;
        u32 bl = 0;
        u64 tid;
        if (view == V_DATABIN) databin_banner(it->path, &tid, &bn, &bl);
        else {
            char bp[VFS_PATH_MAX + 16];
            snprintf(bp, sizeof(bp), "%s/banner.bin", it->path);
            bn = read_file(bp, &bl, 0xF0A0);
        }
        if (bn) wii_pics(bn, bl, &p, true);
        free(bn);
    } else {
        gci_load(it->path, NULL, &p, true);
    }
    int cols = ui_cols(), f = 0, tick = 0;
    bool full = true;
    for (;;) {
        if (full) {
            ui_clear();
            ui_bg(C_BLUE);
            ui_color(C_WHITE, true);
            ui_field(0, 0, cols - 1, " %s", it->title);
            ui_reset();
            char sz[24];
            ui_fmt_size(sz, sizeof(sz), it->size);
            ui_color(C_WHITE, false);
            ui_field(14, 2, cols - 4, "%s", it->title);
            ui_field(15, 2, cols - 4, "%s", it->sub);
            ui_color(C_CYAN, false);
            if (it->tid) ui_field(17, 2, cols - 4, "Title %08x-%08x   ID %s   %s", (unsigned)(it->tid >> 32), (unsigned)it->tid, it->id6, sz);
            else ui_field(17, 2, cols - 4, "ID %s   %s", it->id6, sz);
            ui_field(18, 2, cols - 4, "%s", it->path);
            ui_color(C_CYAN, true);
            ui_field(ui_rows() - 2, 1, cols - 2, "1 Box art  2 Disc art  B Back");
            ui_reset();
            if (p.banner) {
                int s = p.bw == 96 ? 4 : 2;
                ui_blit_rgba(ui_cell_x(cols / 2) - p.bw * s / 2, ui_cell_y(2), p.banner, p.bw, p.bh, s);
            }
            full = false;
        }
        if (p.frames) {
            if (++tick >= p.delay[f] * 4) { tick = 0; f = (f + 1) % p.frames; }
            int s = p.iw == 32 ? 3 : 2;
            ui_blit_rgba(ui_cell_x(2), ui_cell_y(14) - p.iw * s - 8, p.frame[f], p.iw, p.iw, s);
        }
        u32 b = input_pressed();
        if (b & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (b & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) {
            covers_show(it->id6, b & WPAD_BUTTON_2);
            full = true;
        }
    }
    pics_free(&p);
}

/* ---------------------------------------------------------------- actions */

static int pick_drive(const char *title)
{
    const char *names[1 + USB_SLOTS];
    int devs[1 + USB_SLOTS], n = 0;
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (d == DEV_GCA || d == DEV_GCB || !g_devs[d].mounted || vfs_is_readonly(g_devs[d].root)) continue;
        names[n] = g_devs[d].name;
        devs[n++] = d;
    }
    if (!n) { ui_message(title, C_RED, "There is no writable SD card or USB drive."); return -1; }
    if (n == 1) return devs[0];
    int r = ui_menu(title, names, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

static void stamp(char *out, size_t n)
{
    time_t now = time(NULL);
    strftime(out, n, "%Y%m%d-%H%M%S", localtime(&now));
}

static void export_wii(const item_t *it, bool as_bin)
{
    const char *T = "EXPORT SAVE";
    int dev = pick_drive("EXPORT TO");
    if (dev < 0) return;
    char st[24], base[VFS_PATH_MAX], msg[VFS_PATH_MAX * 2 + 200];
    stamp(st, sizeof(st));
    snprintf(base, sizeof(base), "%sGodModeWii/save-export/%s", g_devs[dev].root, st);
    if (as_bin) {
        w2b_keys_t keys;
        char why[160], sub[64], dir[VFS_PATH_MAX + 64], out[VFS_PATH_MAX + 80], err[160] = "";
        if (!wad_console_keys(&keys, why, sizeof(why))) {
            snprintf(msg, sizeof(msg), "data.bin files are signed with this console's keys. %s", why);
            ui_message(T, C_RED, msg);
            return;
        }
        databin_dir(it->tid, sub, sizeof(sub));
        snprintf(dir, sizeof(dir), "%s/%s", base, sub);
        snprintf(out, sizeof(out), "%s/data.bin", dir);
        char tmp[VFS_PATH_MAX + 64];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *s = strchr(tmp + strlen(g_devs[dev].root), '/'); s; s = strchr(s + 1, '/')) { *s = 0; vfs_mkdir(tmp); *s = '/'; }
        vfs_mkdir(tmp);
        ui_splash("Making the data.bin...");
        int rc = databin_export(it->tid, it->path, out, &keys, err, sizeof(err));
        memset(&keys, 0, sizeof(keys));
        if (rc) { ui_message(T, C_RED, err); return; }
        snprintf(msg, sizeof(msg), "Saved %s\n\nCopy its private folder to the SD card's root to restore it in Data "
                 "Management, here or on another Wii.", out);
    } else {
        char dst[VFS_PATH_MAX + 48], tmp[VFS_PATH_MAX + 48];
        snprintf(dst, sizeof(dst), "%s/%08x-%08x %s", base, (unsigned)(it->tid >> 32), (unsigned)it->tid, it->id6);
        snprintf(tmp, sizeof(tmp), "%s", dst);
        for (char *s = strchr(tmp + strlen(g_devs[dev].root), '/'); s; s = strchr(s + 1, '/')) { *s = 0; vfs_mkdir(tmp); *s = '/'; }
        vfs_mkdir(tmp);
        char *src[1] = { (char *)it->path }, *dd[1] = { dst };
        if (!ops_copy_each(src, dd, 1, "EXPORTING THE SAVE")) return;
        snprintf(msg, sizeof(msg), "Copied the save's files to\n%s", dst);
    }
    ui_message("SAVE EXPORTED", C_GREEN, msg);
}

/* The save's files deleted, its folder kept (the game makes a new save); the old one backed up first. */
static bool delete_wii(const item_t *it)
{
    char q[300];
    snprintf(q, sizeof(q), "Delete the save of \"%s\" (%s) from the NAND?\n\nIt is copied to GodModeWii/save-backups/ "
             "first. The game starts as if it had never been played.", it->title, it->id6);
    if (!ui_confirm("DELETE SAVE", C_RED, q) || !risk_ensure_unlocked()) return false;
    char where[VFS_PATH_MAX] = "";
    u32 before = 0;
    ui_splash("Backing up the save...");
    if (wad_backup_save(it->tid, where, sizeof(where), &before) < 0) {
        ui_message("DELETE SAVE", C_RED, "Couldn't back the save up (no writable SD card or USB drive?), so nothing was deleted.");
        return false;
    }
    vfs_entry *l;
    int n;
    if (!vfs_list(it->path, &l, &n)) return false;
    char **paths = calloc(n ? n : 1, sizeof(char *));
    int k = 0;
    for (int i = 0; paths && i < n; i++) {
        if ((paths[k] = malloc(VFS_PATH_MAX))) snprintf(paths[k++], VFS_PATH_MAX, "%s/%s", it->path, l[i].name);
    }
    free(l);
    bool ok = paths && ops_delete(paths, k);
    for (int i = 0; i < k; i++) free(paths[i]);
    free(paths);
    char msg[VFS_PATH_MAX + 120];
    snprintf(msg, sizeof(msg), "%s\n\nThe backup is in %s", ok ? "The save was deleted." : "Not everything could be deleted.", where);
    ui_message("DELETE SAVE", ok ? C_GREEN : C_RED, msg);
    return ok;
}

static bool copy_gci(const item_t *it, const char *dst_dir, const char *title)
{
    char *src[1] = { (char *)it->path };
    bool ok = ops_paste(src, 1, false, dst_dir);
    if (ok) {
        char msg[VFS_PATH_MAX * 2 + 40];
        snprintf(msg, sizeof(msg), "Copied %s\nto %s", it->path, dst_dir);
        ui_message(title, C_GREEN, msg);
    }
    return ok;
}

/* The actions for one save; true when the list has to be read again. */
static bool open_item(int idx)
{
    item_t *it = &items[idx];
    const char *acts[8];
    int code[8], n = 0;
    enum { A_LOOK, A_BIN, A_DIR, A_DEL, A_GCI, A_OTHER, A_IMPORT, A_TOCARD };
    acts[n] = "Look at the banner and icon"; code[n++] = A_LOOK;
    if (view == V_WII) {
        acts[n] = "Export as a data.bin (Data Management)"; code[n++] = A_BIN;
        acts[n] = "Export the files (a folder)";            code[n++] = A_DIR;
        acts[n] = "Delete this save (Risky Mode)";          code[n++] = A_DEL;
    } else if (view == V_GCA || view == V_GCB) {
        acts[n] = "Export as a .gci (to GodModeWii/gci)";   code[n++] = A_GCI;
        acts[n] = "Copy to the other memory card";          code[n++] = A_OTHER;
        acts[n] = "Delete from the card (Risky Mode)";      code[n++] = A_DEL;
    } else if (view == V_DATABIN) {
        acts[n] = "Copy to the NAND (replaces the save there)"; code[n++] = A_IMPORT;
    } else {
        acts[n] = "Copy to a memory card";                  code[n++] = A_TOCARD;
    }
    int r = ui_menu(it->title[0] ? it->title : "SAVE", acts, NULL, n, 0);
    if (r < 0) return false;
    switch (code[r]) {
    case A_LOOK: show_banner(it); return false;
    case A_BIN: export_wii(it, true); return false;
    case A_DIR: export_wii(it, false); return false;
    case A_DEL:
        if (view == V_WII) return delete_wii(it);
        else {
            char q[200];
            snprintf(q, sizeof(q), "Delete \"%s\" from the memory card? Export it first if you want to keep it.", it->title);
            if (!ui_confirm("DELETE SAVE", C_RED, q) || !risk_ensure_unlocked()) return false;
            char *src[1] = { it->path };
            return ops_delete(src, 1);
        }
    case A_GCI: {
        int dev = pick_drive("EXPORT TO");
        if (dev < 0) return false;
        char d[VFS_PATH_MAX];
        snprintf(d, sizeof(d), "%sGodModeWii", g_devs[dev].root);
        vfs_mkdir(d);
        snprintf(d, sizeof(d), "%sGodModeWii/gci", g_devs[dev].root);
        vfs_mkdir(d);
        copy_gci(it, d, "SAVE EXPORTED");
        return false;
    }
    case A_OTHER: {
        int other = view == V_GCA ? DEV_GCB : DEV_GCA;
        if (!g_devs[other].mounted) { ui_message("COPY SAVE", C_YELLOW, "There is no card in the other slot."); return false; }
        copy_gci(it, g_devs[other].root, "SAVE COPIED");
        return false;
    }
    case A_TOCARD: {
        const char *cards[2];
        int dv[2], k = 0;
        if (g_devs[DEV_GCA].mounted) { cards[k] = "Memory card A"; dv[k++] = DEV_GCA; }
        if (g_devs[DEV_GCB].mounted) { cards[k] = "Memory card B"; dv[k++] = DEV_GCB; }
        if (!k) { ui_message("COPY SAVE", C_YELLOW, "There is no memory card in either slot."); return false; }
        int c = k == 1 ? 0 : ui_menu("COPY TO", cards, NULL, k, 0);
        if (c >= 0) copy_gci(it, g_devs[dv[c]].root, "SAVE COPIED");
        return false;
    }
    case A_IMPORT: databin_import(it->path); return false;
    }
    return false;
}

void savemgr_run(void)
{
    view = V_WII;
    load_view();
    int cursor = 0, top = 0, per_page = (ui_rows() - 6) / ROWS_PER;
    bool full = true;
    for (;;) {
        if (cursor >= n_items) cursor = n_items ? n_items - 1 : 0;
        if (cursor < top) top = cursor;
        if (cursor >= top + per_page) top = cursor - per_page + 1;
        if (full) draw_list(cursor, top, per_page);
        full = false;
        u32 b = input_pressed();
        if (!b) continue;
        full = true;
        if (b & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (b & WPAD_BUTTON_UP) cursor = cursor ? cursor - 1 : (n_items ? n_items - 1 : 0);
        if (b & WPAD_BUTTON_DOWN) cursor = n_items ? (cursor + 1) % n_items : 0;
        if (b & WPAD_BUTTON_LEFT) cursor = cursor > per_page ? cursor - per_page : 0;
        if (b & WPAD_BUTTON_RIGHT) cursor = cursor + per_page < n_items ? cursor + per_page : (n_items ? n_items - 1 : 0);
        if (b & WPAD_BUTTON_PLUS) { view = (view + 1) % V_COUNT; cursor = top = 0; load_view(); }
        if ((b & WPAD_BUTTON_MINUS) && view == V_WII) { saves_export_all(); load_view(); }
        if (!n_items) continue;
        if (b & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) covers_show(items[cursor].id6, b & WPAD_BUTTON_2);
        if ((b & WPAD_BUTTON_A) && open_item(cursor)) load_view();
    }
    clear_items();
}

#ifdef GMW_AUTOTEST
/* "@saves": every list read and logged, and the first save's banner screen shot. */
void savemgr_autotest(void)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    for (view = 0; view < V_COUNT; view++) {
        load_view();
        fprintf(lg, "saves %s: %d\n", VIEW_NAME[view], n_items);
        for (int i = 0; i < n_items; i++)
            fprintf(lg, "  %s | %s | %s | icon %d\n", items[i].id6, items[i].title, items[i].sub, items[i].iw);
        if (n_items) {
            draw_list(0, 0, (ui_rows() - 6) / ROWS_PER);
            ui_test_dump();
        }
    }
    clear_items();
    fclose(lg);
}
#endif

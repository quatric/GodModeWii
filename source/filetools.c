/*
 * GodModeWii - comparing two files in hex, replacing a file's contents or injecting one file into another,
 * and showing what a WAD / TMD / ticket / installed title says. See filetools.h.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <ctype.h>
#include <gccore.h>

#include "filetools.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include "risk.h"
#include "archive.h"

#define CHUNK (64 * 1024)
#define MEM_MAX (32u << 20)

static s32 read_at(vfs_file *f, u64 off, u8 *buf, u32 len)
{
    if (off >= f->size) return 0;
    if (off + len > f->size) len = (u32)(f->size - off);
    if (!vfs_seek(f, off)) return -1;
    u32 got = 0;
    while (got < len) {
        s32 r = vfs_read(f, buf + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    return (s32)got;
}

/* ---------------------------------------------------------------- compare */

#define ROWB 8

/* The first offset >= from (or < from, going back) where the files differ; -1: none. */
static s64 find_diff(vfs_file *a, vfs_file *b, u64 from, bool back, u8 *ba, u8 *bb)
{
    u64 common = a->size < b->size ? a->size : b->size, end = a->size > b->size ? a->size : b->size;
    if (!back) {
        for (u64 off = from; off < common;) {
            u32 n = common - off < CHUNK ? (u32)(common - off) : CHUNK;
            if (read_at(a, off, ba, n) != (s32)n || read_at(b, off, bb, n) != (s32)n) return -1;
            for (u32 i = 0; i < n; i++) if (ba[i] != bb[i]) return (s64)(off + i);
            off += n;
            if (input_cancel_held()) return -1;
        }
        return from < end && common < end ? (s64)(from > common ? from : common) : -1;
    }
    if (from > end) from = end;
    if (from > common && common < end) return (s64)from - 1;      /* past the shorter file: all different */
    for (u64 hi = from; hi > 0;) {
        u32 n = hi < CHUNK ? (u32)hi : CHUNK;
        u64 off = hi - n;
        if (read_at(a, off, ba, n) != (s32)n || read_at(b, off, bb, n) != (s32)n) return -1;
        for (u32 i = n; i-- > 0;) if (ba[i] != bb[i]) return (s64)(off + i);
        hi = off;
        if (input_cancel_held()) return -1;
    }
    return -1;
}

void hexcompare(const char *pa, const char *pb)
{
    vfs_file a, b;
    if (!vfs_open_read(&a, pa)) { ui_message("COMPARE", C_RED, "The first file can't be opened."); return; }
    if (!vfs_open_read(&b, pb)) { vfs_close(&a); ui_message("COMPARE", C_RED, "The copied file can't be opened."); return; }
    u8 *ba = memalign(32, CHUNK), *bb = memalign(32, CHUNK);
    if (!ba || !bb) { free(ba); free(bb); vfs_close(&a); vfs_close(&b); return; }

    /* how many bytes differ (B stops the count) */
    ui_splash("Comparing...   (hold B to skip the count)");
    u64 common = a.size < b.size ? a.size : b.size, ndiff = 0;
    bool counted = true;
    for (u64 off = 0; off < common;) {
        u32 n = common - off < CHUNK ? (u32)(common - off) : CHUNK;
        if (read_at(&a, off, ba, n) != (s32)n || read_at(&b, off, bb, n) != (s32)n) { counted = false; break; }
        for (u32 i = 0; i < n; i++) ndiff += ba[i] != bb[i];
        off += n;
        if (input_cancel_held()) { counted = false; break; }
    }
    ndiff += (a.size > b.size ? a.size : b.size) - common;
    input_flush();

    int rows = ui_rows(), cols = ui_cols(), L = rows - 7;
    u64 end = a.size > b.size ? a.size : b.size, off = 0;
    s64 first = find_diff(&a, &b, 0, false, ba, bb);
    if (first > 0) off = (u64)first / ROWB * ROWB;
    u8 ra[ROWB * 64], rb[ROWB * 64];
    for (;;) {
        ui_clear();
        ui_color(C_CYAN, true);
        ui_field(0, 0, cols - 1, " COMPARE  %s", vfs_basename(pa));
        ui_field(1, 0, cols - 1, "    with  %s", vfs_basename(pb));
        ui_reset();
        char sz[2][24];
        ui_fmt_size(sz[0], sizeof(sz[0]), a.size);
        ui_fmt_size(sz[1], sizeof(sz[1]), b.size);
        ui_color(ndiff ? C_YELLOW : C_GREEN, true);
        if (counted)
            ui_field(2, 0, cols - 1, " %s / %s   %llu byte%s differ%s", sz[0], sz[1], (unsigned long long)ndiff,
                     ndiff == 1 ? "" : "s", ndiff ? "" : ": the files are the same");
        else
            ui_field(2, 0, cols - 1, " %s / %s", sz[0], sz[1]);
        ui_reset();
        u32 want = (u32)L * ROWB;
        s32 na = read_at(&a, off, ra, want), nb = read_at(&b, off, rb, want);
        for (int r = 0; r < L; r++) {
            u64 o = off + (u64)r * ROWB;
            if (o >= end) break;
            int row = 4 + r;
            ui_color(C_WHITE, false);
            ui_field(row, 0, 10, "%08llX", (unsigned long long)o);
            for (int i = 0; i < ROWB; i++) {
                int k = r * ROWB + i;
                bool ha = k < na, hb = k < nb, diff = ha != hb || (ha && ra[k] != rb[k]);
                ui_color(diff ? C_RED : C_WHITE, diff);
                if (ha) ui_field(row, 10 + i * 3, 3, "%02X", ra[k]); else ui_field(row, 10 + i * 3, 3, "--");
                if (hb) ui_field(row, 37 + i * 3, 3, "%02X", rb[k]); else ui_field(row, 37 + i * 3, 3, "--");
                char ca = ha && isprint(ra[k]) ? ra[k] : '.', cb = hb && isprint(rb[k]) ? rb[k] : '.';
                if (cols >= 80) { ui_field(row, 63 + i, 1, "%c", ca); ui_field(row, 72 + i, 1, "%c", cb); }
            }
        }
        ui_reset();
        ui_color(C_CYAN, false);
        ui_field(rows - 2, 1, cols - 2, "Up/Down Scroll  Left/Right Page  A Next difference  1 Previous  B Back");
        ui_reset();
        u32 p;
        do p = input_pressed(); while (!p);
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        u64 page = (u64)L * ROWB, last = end > page ? (end - 1) / ROWB * ROWB - page + ROWB : 0;
        if (p & WPAD_BUTTON_DOWN) off = off + ROWB <= last ? off + ROWB : last;
        if (p & WPAD_BUTTON_UP) off = off >= ROWB ? off - ROWB : 0;
        if (p & WPAD_BUTTON_RIGHT) off = off + page <= last ? off + page : last;
        if (p & WPAD_BUTTON_LEFT) off = off >= page ? off - page : 0;
        if (p & (WPAD_BUTTON_A | WPAD_BUTTON_1)) {
            bool back = (p & WPAD_BUTTON_1) != 0;
            ui_splash("Looking for the next difference...   (hold B to stop)");
            s64 d = find_diff(&a, &b, back ? off : off + ROWB, back, ba, bb);
            input_flush();
            if (d >= 0) off = (u64)d / ROWB * ROWB;
            else ui_message("COMPARE", C_GREEN, back ? "No difference before this." : "No difference after this.");
        }
    }
    free(ba);
    free(bb);
    vfs_close(&a);
    vfs_close(&b);
}

/* ---------------------------------------------------------------- replace / inject */

static u8 *read_all(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > MEM_MAX) { vfs_close(&f); return NULL; }
    u8 *b = memalign(32, (u32)f.size + 32);
    s32 n = b ? read_at(&f, 0, b, (u32)f.size) : -1;
    vfs_close(&f);
    if (n != (s32)f.size) { free(b); return NULL; }
    *len = (u32)f.size;
    return b;
}

/* dst's new contents: written over it (the file itself stays, so the NAND keeps its owner and permissions) */
static bool write_all(const char *dst, const u8 *b, u32 len)
{
    vfs_file f;
    if (!vfs_open_write(&f, dst)) return false;
    u32 done = 0;
    while (done < len) {
        s32 w = vfs_write(&f, b + done, len - done > CHUNK ? CHUNK : len - done);
        if (w <= 0) break;
        done += w;
    }
    return vfs_close_checked(&f) && done == len;
}

/* big files: through a copy next to it, then swapped in */
static bool stream_replace(const char *dst, const char *src, u64 at, bool inject)
{
    char tmp[VFS_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.gmwtmp", dst);
    vfs_file in = { 0 }, ins, out;
    if (!vfs_open_write(&out, tmp)) return false;
    bool ok = vfs_open_read(&ins, src);
    bool have_in = inject && ok && vfs_open_read(&in, dst);
    if (inject && !have_in) ok = false;
    u8 *b = memalign(32, CHUNK);
    ok = ok && b;
    u64 total = inject ? (in.size > at + ins.size ? in.size : at + ins.size) : ins.size;
    for (u64 o = 0; ok && o < total;) {
        u32 n = total - o < CHUNK ? (u32)(total - o) : CHUNK;
        /* the stretch from the injected file, the rest from the original */
        if (inject && (o + n <= at || o >= at + ins.size)) {
            ok = read_at(&in, o, b, n) == (s32)n;
        } else if (inject && o < at) {
            n = (u32)(at - o);
            ok = read_at(&in, o, b, n) == (s32)n;
        } else {
            u64 so = inject ? o - at : o;
            if (inject && so + n > ins.size) n = (u32)(ins.size - so);
            ok = read_at(&ins, so, b, n) == (s32)n;
        }
        ok = ok && vfs_write(&out, b, n) == (s32)n;
        o += n;
        ui_busy_progress(o, total);
    }
    free(b);
    if (have_in) vfs_close(&in);
    vfs_close(&ins);
    ok = vfs_close_checked(&out) && ok;
    if (ok) ok = vfs_remove(dst, false) && vfs_rename(tmp, dst);
    if (!ok) vfs_remove(tmp, false);
    return ok;
}

bool file_replace(const char *dst, const char *src)
{
    const char *T = "REPLACE";
    if (!strcmp(dst, src)) { ui_message(T, C_YELLOW, "That's the same file."); return false; }
    if (!risk_authorize(dst, "replace")) return false;
    char q[VFS_PATH_MAX * 2 + 120];
    snprintf(q, sizeof(q), "Replace the contents of\n%s\nwith those of\n%s?\n\nThe name and place stay; what was in it is lost.",
             vfs_basename(dst), vfs_basename(src));
    if (!ui_confirm(T, C_YELLOW, q)) return false;
    ui_busy_begin("Replacing...");
    u32 len;
    u8 *b = read_all(src, &len);
    bool ok = b ? write_all(dst, b, len) : vfs_dev_of(dst) != DEV_NAND && stream_replace(dst, src, 0, false);
    free(b);
    ui_busy_end();
    ui_message(T, ok ? C_GREEN : C_RED, ok ? "Replaced." : "It couldn't be replaced (the file may be too big for the NAND).");
    return ok;
}

bool file_inject(const char *dst, const char *src)
{
    const char *T = "INJECT";
    if (!strcmp(dst, src)) { ui_message(T, C_YELLOW, "That's the same file."); return false; }
    bool d1, d2;
    u64 dsz = 0, ssz = 0;
    if (!vfs_stat(dst, &d1, &dsz) || !vfs_stat(src, &d2, &ssz) || d1 || d2) return false;
    char hex[20] = "0";
    if (!ui_text_input("OFFSET (HEX)", hex, sizeof(hex) - 1)) return false;
    char *e;
    u64 at = strtoull(hex, &e, 16);
    if (*e || at > dsz) {
        char m[96];
        snprintf(m, sizeof(m), "The offset has to be hex, from 0 to %llX (the file's size).", (unsigned long long)dsz);
        ui_message(T, C_RED, m);
        return false;
    }
    if (!risk_authorize(dst, "inject into")) return false;
    char q[VFS_PATH_MAX * 2 + 160];
    snprintf(q, sizeof(q), "Write %s (%llu bytes) into %s at 0x%llX?%s", vfs_basename(src), (unsigned long long)ssz,
             vfs_basename(dst), (unsigned long long)at, at + ssz > dsz ? "\n\nThe file gets longer." : "");
    if (!ui_confirm(T, C_YELLOW, q)) return false;
    ui_busy_begin("Injecting...");
    bool ok = false;
    if (dsz <= MEM_MAX && at + ssz <= MEM_MAX) {
        u32 dl, sl;
        u8 *d = read_all(dst, &dl), *s = read_all(src, &sl);
        u32 nl = (u32)(at + sl > dl ? at + sl : dl);
        u8 *n = d && s ? realloc(d, nl + 32) : NULL;
        if (n) {
            memcpy(n + at, s, sl);
            ok = write_all(dst, n, nl);
            d = n;
        }
        free(d);
        free(s);
    } else if (vfs_dev_of(dst) != DEV_NAND) {
        ok = stream_replace(dst, src, at, true);
    }
    ui_busy_end();
    ui_message(T, ok ? C_GREEN : C_RED, ok ? "Injected." : "It couldn't be written.");
    return ok;
}

/* ---------------------------------------------------------------- WAD / TMD / ticket */

static u16 g16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 g32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u64 g64(const u8 *p) { return (u64)g32(p) << 32 | g32(p + 4); }

static bool ends(const char *s, const char *x)
{
    size_t a = strlen(s), b = strlen(x);
    return a >= b && !strcasecmp(s + a - b, x);
}

/* <nand>/title/<hi>/<lo>: the Wii's NAND, or an emulated one (a folder with sys/ beside title/). The NAND's root,
 * "nand:/" or "sd:/nands/x/", in root (when not NULL). */
static bool nand_title_dir_in(const char *path, u32 *hi, u32 *lo, char *root, size_t rn)
{
    const char *p = strstr(path, "/title/");
    if (!p) return false;
    char h[9], l[9], r[VFS_PATH_MAX], sys[VFS_PATH_MAX];
    if (sscanf(p + 7, "%8[0-9a-fA-F]/%8[0-9a-fA-F]", h, l) != 2 || strlen(h) != 8 || strlen(l) != 8) return false;
    snprintf(r, sizeof(r), "%.*s", (int)(p - path + 1), path);
    bool is_dir;
    snprintf(sys, sizeof(sys), "%ssys", r);
    if (vfs_dev_of(path) != DEV_NAND && !(vfs_stat(sys, &is_dir, NULL) && is_dir)) return false;
    *hi = strtoul(h, NULL, 16);
    *lo = strtoul(l, NULL, 16);
    if (root) snprintf(root, rn, "%s", r);
    return true;
}

static bool nand_title_dir(const char *path, u32 *hi, u32 *lo)
{
    return nand_title_dir_in(path, hi, lo, NULL, 0);
}

bool titleinfo_can(const char *path)
{
    const char *b = vfs_basename(path);
    u32 hi, lo;
    return ends(b, ".wad") || ends(b, ".tmd") || ends(b, ".tik") || !strcasecmp(b, "cetk") || !strcasecmp(b, "tmd") ||
           !strncasecmp(b, "tmd.", 4) || nand_title_dir(path, &hi, &lo);
}

typedef struct { char *t; size_t n, cap; } strbuf_t;
static void add(strbuf_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(strbuf_t *s, const char *fmt, ...)
{
    if (s->n + 300 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        char *t = realloc(s->t, nc);
        if (!t) return;
        s->t = t;
        s->cap = nc;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s->t + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (w > 0) s->n += (size_t)w < s->cap - s->n ? (size_t)w : s->cap - s->n - 1;
}

static const char *type_name(u32 hi)
{
    switch (hi) {
    case 0x00000001: return "system (IOS, Wii Menu, BC, MIOS)";
    case 0x00010000: return "disc game";
    case 0x00010001: return "channel (WiiWare, Virtual Console...)";
    case 0x00010002: return "system channel";
    case 0x00010004: return "game channel (from a disc)";
    case 0x00010005: return "downloadable content";
    case 0x00010008: return "hidden title";
    default: return "other";
    }
}

static void tid_text(char *out, size_t n, u64 tid)
{
    u32 lo = (u32)tid;
    char a[5];
    for (int i = 0; i < 4; i++) a[i] = (char)(lo >> (24 - 8 * i));
    a[4] = 0;
    bool txt = isalnum((unsigned char)a[0]) && isalnum((unsigned char)a[1]) && isalnum((unsigned char)a[2]) &&
               isalnum((unsigned char)a[3]);
    snprintf(out, n, "%08X-%08X%s%s%s", (unsigned)(tid >> 32), (unsigned)lo, txt ? "  (" : "", txt ? a : "", txt ? ")" : "");
}

static bool zero(const u8 *p, u32 n) { while (n--) if (*p++) return false; return true; }

static void sig_text(strbuf_t *s, const u8 *b, u32 len, const char *what)
{
    if (len < 0x180 || g32(b) != 0x00010001) { add(s, "%s signature: type %08X\n", what, len >= 4 ? (unsigned)g32(b) : 0); return; }
    char iss[65];
    memcpy(iss, b + 0x140, 64);
    iss[64] = 0;
    add(s, "%s issuer:     %s%s\n", what, iss, arc_key_is_dev(b + 0x140) ? "  (development console, RVT)" : "");
    add(s, "%s signature:  %s\n", what, zero(b + 4, 0x100) ? "fakesigned (zeroed)" : "RSA-2048 (real or trucha)");
}

static void tmd_text(strbuf_t *s, const u8 *t, u32 len)
{
    if (len < 0x1E4) { add(s, "The TMD is too short.\n"); return; }
    bool wiiu = t[0x180] == 1;              /* version 1: a Wii U TMD, its contents after a 64-entry info table */
    u32 base = wiiu ? 0xB04 : 0x1E4, rsz = wiiu ? 0x30 : 0x24;
    u64 tid = g64(t + 0x18C), sysver = g64(t + 0x184);
    u16 ver = g16(t + 0x1DC), n = g16(t + 0x1DE);
    char id[40];
    tid_text(id, sizeof(id), tid);
    static const char *const regions[] = { "Japan", "USA", "Europe", "region free", "Korea" };
    u16 region = g16(t + 0x19C);
    add(s, "TITLE METADATA (TMD)\n");
    add(s, "Title ID:     %s\n", id);
    add(s, "Type:         %s\n", type_name((u32)(tid >> 32)));
    add(s, "Version:      %u  (v%u.%u)\n", ver, ver >> 8, ver & 0xFF);
    if (!wiiu && sysver >> 32 == 1) add(s, "Runs on:      IOS%u\n", (unsigned)(u32)sysver);
    else if (sysver) add(s, "Runs on:      %016llX\n", (unsigned long long)sysver);
    else add(s, "Runs on:      (none: this is an IOS or boot2)\n");
    add(s, "Region:       %s\n", region < 5 ? regions[region] : "unknown");
    add(s, "Group ID:     %04X   (the maker; 3031 = \"01\", Nintendo)\n", g16(t + 0x198));
    u32 ar = g32(t + 0x1D8);
    add(s, "Access:       %08X%s%s\n", (unsigned)ar, ar & 1 ? "  hardware (AHBPROT)" : "", ar & 2 ? "  DVD video" : "");
    add(s, "For:          %s\n", wiiu ? "Wii U" : t[0x183] ? "vWii (Wii U)" : "Wii");
    sig_text(s, t, len, "TMD");
    add(s, "Contents:     %u, boot index %u\n\n", n, g16(t + 0x1E0));
    add(s, "  ID        index  type    size        SHA-1 (start)\n");
    for (u16 i = 0; i < n && base + (i + 1) * rsz <= len; i++) {
        const u8 *r = t + base + i * rsz;
        u16 ty = g16(r + 6);
        char sz[16];
        ui_fmt_size(sz, sizeof(sz), g64(r + 8));
        add(s, "  %08X  %5u  %-6s  %-10s  %02x%02x%02x%02x%02x%02x...\n", (unsigned)g32(r), g16(r + 4),
            ty & 0x8000 ? "shared" : ty & 0x4000 ? "DLC" : wiiu && (ty & 2) ? "hashed" : ty & 1 ? "normal" : "other", sz, r[16], r[17], r[18], r[19],
            r[20], r[21]);
    }
}

static void tik_text(strbuf_t *s, const u8 *k, u32 len)
{
    if (len < 0x2A4) { add(s, "The ticket is too short.\n"); return; }
    char id[40];
    tid_text(id, sizeof(id), g64(k + 0x1DC));
    static const char *const keys[] = { "Wii", "Korean Wii", "vWii (Wii U)" };
    u8 ck = k[0x1F1];
    add(s, "\nTICKET\n");
    add(s, "Title ID:     %s\n", id);
    add(s, "Ticket ID:    %016llX\n", (unsigned long long)g64(k + 0x1D0));
    u32 console = g32(k + 0x1D8);
    if (console) add(s, "For console:  %08X  (personalised: only that console)\n", (unsigned)console);
    else add(s, "For console:  any\n");
    add(s, "Common key:   %s\n", arc_key_is_dev(k + 0x140) ? "development (RVT)" : k[0x1BC] == 1 ? "Wii U"
                                : ck < 3 ? keys[ck] : "unknown index");
    add(s, "Title key:    %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x  (encrypted)\n",
        k[0x1BF], k[0x1C0], k[0x1C1], k[0x1C2], k[0x1C3], k[0x1C4], k[0x1C5], k[0x1C6], k[0x1C7], k[0x1C8], k[0x1C9],
        k[0x1CA], k[0x1CB], k[0x1CC], k[0x1CD], k[0x1CE]);
    add(s, "ECDH data:    %s\n", zero(k + 0x180, 0x3C) ? "none" : "present (bought from the Wii Shop)");
    sig_text(s, k, len, "Ticket");
    bool lim = false;
    for (int i = 0; i < 8; i++) {
        u32 t = g32(k + 0x264 + i * 8), v = g32(k + 0x268 + i * 8);
        if (!t) continue;
        add(s, "Limit:        %s %u\n", t == 1 ? "minutes" : t == 4 ? "launches" : "type", (unsigned)v);
        lim = true;
    }
    if (!lim) add(s, "Limits:       none\n");
}

static void show(const char *title, strbuf_t *s)
{
    if (!s->t) return;
    /* pages of the screen's height */
    const char *pages[64];
    int np = 0, per = ui_rows() - 8, line = 0;
    pages[np++] = s->t;
    for (char *p = s->t; *p && np < 64; p++) {
        if (*p != '\n') continue;
        if (++line % per == 0 && p[1]) { *p = 0; pages[np++] = p + 1; }
    }
    ui_message_pages(title, C_CYAN, pages, np);
}

void titleinfo_show(const char *path)
{
    strbuf_t s = { 0 };
    const char *b = vfs_basename(path);
    u32 hi, lo;
    u8 *tmd = NULL, *tik = NULL;
    u32 tl = 0, kl = 0;
    char root[VFS_PATH_MAX];
    if (nand_title_dir_in(path, &hi, &lo, root, sizeof(root))) {
        char p[VFS_PATH_MAX + 64];
        snprintf(p, sizeof(p), "%stitle/%08x/%08x/content/title.tmd", root, (unsigned)hi, (unsigned)lo);
        tmd = read_all(p, &tl);
        snprintf(p, sizeof(p), "%sticket/%08x/%08x.tik", root, (unsigned)hi, (unsigned)lo);
        tik = read_all(p, &kl);
        if (!tmd) add(&s, "This title has no title.tmd (a save folder only, or not installed).\n");
    } else if (ends(b, ".wad")) {
        vfs_file f;
        u8 *h = memalign(32, 64);
        if (h && vfs_open_read(&f, path)) {
            if (read_at(&f, 0, h, 0x20) == 0x20) {
                u32 hl = g32(h), cl = g32(h + 8), rl = g32(h + 12);
                kl = g32(h + 16);
                tl = g32(h + 20);
                u32 dl = g32(h + 24);
                add(&s, "WAD:          type \"%c%c\", %u contents bytes, certificates %u, CRL %u\n", h[4], h[5],
                    (unsigned)dl, (unsigned)cl, (unsigned)rl);
                u64 off = ((u64)hl + 63) & ~63ull;
                off += (cl + 63) & ~63u;
                off += (rl + 63) & ~63u;
                if (kl && kl < 0x10000 && (tik = memalign(32, kl + 32)) && read_at(&f, off, tik, kl) != (s32)kl) {
                    free(tik);
                    tik = NULL;
                }
                off += (kl + 63) & ~63u;
                if (tl && tl < 0x100000 && (tmd = memalign(32, tl + 32)) && read_at(&f, off, tmd, tl) != (s32)tl) {
                    free(tmd);
                    tmd = NULL;
                }
            }
            vfs_close(&f);
        }
        free(h);
    } else if (ends(b, ".tik") || !strcasecmp(b, "cetk")) {
        tik = read_all(path, &kl);
    } else {
        tmd = read_all(path, &tl);
    }
    if (tmd) tmd_text(&s, tmd, tl);
    if (tik) tik_text(&s, tik, kl);
    if (!tmd && !tik && !s.n) add(&s, "There is no TMD or ticket here that could be read.\n");
    show("TITLE INFO", &s);
    free(tmd);
    free(tik);
    free(s.t);
}

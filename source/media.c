/*
 * Viewer dispatch and the BMG message viewer. See media.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "pointer.h"
#include "archive.h"
#include "mc_player.h"
#include "mii.h"

static bool media_open_type_(const char *path, ftype_t t);

/* Video and layouts draw straight into the framebuffer: no arrow over them. */
bool media_open_type(const char *path, ftype_t t)
{
    bool own = t == FT_VIDEO || t == FT_LAYOUT || t == FT_ANIM;
    if (own) ptr_overlay(false);
    bool ok = media_open_type_(path, t);
    if (own) ptr_overlay(true);
    return ok;
}

static bool media_open_type_(const char *path, ftype_t t)
{
    switch (t) {
    case FT_STREAM: case FT_AUDIO: case FT_MIDI: case FT_SEQ:
        if (audio_play(path)) return true;
        ui_message("AUDIO", C_YELLOW,
                   "This file couldn't be played: its format or layout isn't supported.");
        return true;
    case FT_VIDEO:
        if (video_play(path)) return true;
        ui_message("VIDEO", C_YELLOW,
                   "This file couldn't be played: it isn't a video this player understands.");
        return true;
    case FT_MODEL:                              /* a BRRES with no model may still hold textures */
        return model_view(path) || image_view(path);
    case FT_IMAGE:  return image_view(path);
    case FT_BMG:    return bmg_view(path);
    case FT_BREFF:  return breff_view(path);
    case FT_MSBT:   return msbt_view(path);
    case FT_LAYOUT:
    case FT_ANIM:   return lyt_view(path) || layout_view(path);
    case FT_KMP:    return kmp_view(path);
    case FT_MII:    return mii_file_view(path);
    default:        return false;
    }
}

bool media_open(const char *path)
{
    return media_open_type(path, ftype_detect(path));
}

/* ---------------------------------------------------------------- BMG */

static u8 *load(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > 8 * 1024 * 1024 || f.size < 0x20) {
        vfs_close(&f);
        return NULL;
    }
    u32 n = (u32)f.size, got = 0;
    u8 *b = memalign(32, (n + 31) & ~31u);
    while (b && got < n) {
        s32 r = vfs_read(&f, b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    vfs_close(&f);
    if (b && got != n) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

typedef struct {
    char *p;
    u32 n, cap;
} sbuf;

static void put(sbuf *s, const char *t, u32 n)
{
    if (s->n + n + 1 > s->cap) {
        u32 nc = (s->cap + n + 1) * 2;
        char *np = realloc(s->p, nc);
        if (!np) return;
        s->p = np;
        s->cap = nc;
    }
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void put_cp(sbuf *s, u32 c)
{
    char o[4];
    if (c < 0x80) {
        o[0] = (char)c;
        put(s, o, 1);
    } else if (c < 0x800) {
        o[0] = 0xC0 | c >> 6;
        o[1] = 0x80 | (c & 63);
        put(s, o, 2);
    } else {
        o[0] = 0xE0 | c >> 12;
        o[1] = 0x80 | ((c >> 6) & 63);
        o[2] = 0x80 | (c & 63);
        put(s, o, 3);
    }
}

/* "MESGbmg1": INF1 (entry table: string offset + attribute bytes), DAT1 (strings), MID1 (message
 * ids), FLW1 / FLI1 (flow: which message follows, branches, events); encoding at 0x10:
 * 1 = CP1252, 2 = UTF-16BE, 3 = Shift-JIS (shown as raw bytes), 4 = UTF-8.
 * A control code is 0x1A, its total size in bytes, a group byte, a 16-bit type and parameters:
 * shown as {group.type params}; parameters that are UTF-16 text (ruby, plural choices) as "text". */
static void bmg_escape(sbuf *s, const u8 *e, u32 size, bool utf16)
{
    char line[64];
    u32 hdr = utf16 ? 2 : 1;                         /* the 0x1A itself */
    if (size < hdr + 4) {
        put(s, "{?}", 3);
        return;
    }
    const u8 *q = e + hdr + 1;                       /* after the size byte */
    snprintf(line, sizeof(line), "{%u.%u", (unsigned)q[0], (unsigned)be16(q + 1));
    put(s, line, strlen(line));
    q += 3;
    u32 plen = size - (hdr + 4);
    if (plen) {
        /* text if every UTF-16 unit after the first byte is printable or a NUL separator */
        bool text = utf16 && plen >= 4 && (plen & 1) == 0;
        for (u32 i = 1; text && i + 1 < plen; i += 2) {
            u32 c = be16(q + i);
            if (c && (c < 0x20 || c > 0xFFEF)) text = false;
        }
        if (text) {
            snprintf(line, sizeof(line), " %u \"", (unsigned)q[0]);
            put(s, line, strlen(line));
            for (u32 i = 1; i + 1 < plen; i += 2) {
                u32 c = be16(q + i);
                if (!c) put(s, "|", 1);
                else put_cp(s, c);
            }
            put(s, "\"", 1);
        } else {
            for (u32 i = 0; i < plen; i++) {
                snprintf(line, sizeof(line), " %02X", q[i]);
                put(s, line, 3);
            }
        }
    }
    put(s, "}", 1);
}

bool bmg_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    if (memcmp(b, "MESGbmg1", 8)) {
        free(b);
        return false;
    }

    int enc = b[0x10] ? b[0x10] : 1;
    u32 nsec = be32(b + 0x0C), pos = 0x20, inf = 0, dat = 0, mid = 0, flw = 0, fli = 0;
    char names[48] = "";
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (!memcmp(b + pos, "INF1", 4)) inf = pos;
        if (!memcmp(b + pos, "DAT1", 4)) dat = pos;
        if (!memcmp(b + pos, "MID1", 4)) mid = pos;
        if (!memcmp(b + pos, "FLW1", 4)) flw = pos;
        if (!memcmp(b + pos, "FLI1", 4)) fli = pos;
        if (strlen(names) + 6 < sizeof(names)) {
            strncat(names, (const char *)b + pos, 4);
            strcat(names, " ");
        }
        if (sz < 8 || pos + sz > len) break;
        pos += sz;
    }
    if (!inf || !dat) {
        free(b);
        return false;
    }

    u32 count = be16(b + inf + 8), esz = be16(b + inf + 10);
    u32 nids = 0;
    if (mid && mid + 16 <= len) nids = be16(b + mid + 8);
    sbuf s = { 0 };
    char line[160];
    snprintf(line, sizeof(line), "%u messages, %s, sections: %s\n\n", (unsigned)count,
             enc == 2 ? "UTF-16" : enc == 3 ? "Shift-JIS" : enc == 4 ? "UTF-8" : "CP1252", names);
    put(&s, line, strlen(line));

    for (u32 i = 0; i < count && esz >= 4 && inf + 0x10 + (i + 1) * esz <= len; i++) {
        u32 off = dat + 8 + be32(b + inf + 0x10 + i * esz);
        if (mid && i < nids && mid + 16 + (i + 1) * 4 <= len)
            snprintf(line, sizeof(line), "#%u [id %u]", (unsigned)i,
                     (unsigned)be32(b + mid + 16 + i * 4));
        else
            snprintf(line, sizeof(line), "#%u", (unsigned)i);
        put(&s, line, strlen(line));
        /* the attribute bytes after the string offset, when any is set */
        bool attr = false;
        for (u32 k = 4; k < esz; k++) attr |= b[inf + 0x10 + i * esz + k] != 0;
        if (attr) {
            put(&s, " (attr", 6);
            for (u32 k = 4; k < esz; k++) {
                snprintf(line, sizeof(line), " %02X", b[inf + 0x10 + i * esz + k]);
                put(&s, line, 3);
            }
            put(&s, ")", 1);
        }
        put(&s, ": ", 2);

        u32 p = off;
        while (p < len) {
            u32 c;
            u32 start = p;
            if (enc == 2) {
                if (p + 2 > len) break;
                c = be16(b + p);
                p += 2;
            } else {
                c = b[p++];
            }
            if (!c) break;
            if (c == 0x1A) {                /* control code */
                u32 size = p < len ? b[p] : 0;
                if (size < (enc == 2 ? 2u : 1u) + 1 || start + size > len) {
                    put(&s, "{?}", 3);
                    continue;
                }
                bmg_escape(&s, b + start, size, enc == 2);
                p = start + size;
                continue;
            }

            if (c == '\n') {
                put(&s, "\n    ", 5);
                continue;
            }
            if (enc == 2 || enc == 1) put_cp(&s, c);
            else {
                char ch = (char)c;
                put(&s, &ch, 1);
            }
        }
        put(&s, "\n", 1);
    }

    /* FLW1: u16 node count, u16 label count, 4 reserved; 8 byte nodes, then the u16 labels.
     * FLI1: u16 count, u16 entry size, 4 reserved; entries of u32 id, u16 pad, u16 node. */
    if (flw && flw + 16 <= len) {
        u32 nn = be16(b + flw + 8), nl = be16(b + flw + 10);
        snprintf(line, sizeof(line), "\nFlow: %u nodes, %u labels\n", (unsigned)nn, (unsigned)nl);
        put(&s, line, strlen(line));
        for (u32 i = 0; i < nn && flw + 16 + (i + 1) * 8 <= len; i++) {
            const u8 *n = b + flw + 16 + i * 8;
            const char *t = n[0] == 1 ? "message" : n[0] == 2 ? "branch" : n[0] == 3 ? "event" : "node";
            snprintf(line, sizeof(line), "  %3u %-7s sub %u  %04X %04X %04X\n", (unsigned)i, t,
                     (unsigned)n[1], (unsigned)be16(n + 2), (unsigned)be16(n + 4),
                     (unsigned)be16(n + 6));
            put(&s, line, strlen(line));
        }
        u32 lab = flw + 16 + nn * 8;
        for (u32 i = 0; i < nl && lab + (i + 1) * 2 <= len; i++) {
            snprintf(line, sizeof(line), "  label %u -> node %u\n", (unsigned)i,
                     (unsigned)be16(b + lab + i * 2));
            put(&s, line, strlen(line));
        }
    }
    if (fli && fli + 16 <= len) {
        u32 nf = be16(b + fli + 8), fes = be16(b + fli + 10);
        snprintf(line, sizeof(line), "\nFlow entries: %u\n", (unsigned)nf);
        put(&s, line, strlen(line));
        for (u32 i = 0; fes >= 8 && i < nf && fli + 16 + (i + 1) * fes <= len; i++) {
            const u8 *e = b + fli + 16 + i * fes;
            snprintf(line, sizeof(line), "  id %u -> node %u\n", (unsigned)be32(e),
                     (unsigned)be16(e + 6));
            put(&s, line, strlen(line));
        }
    }
    free(b);

    if (!s.p) return false;
    u8 *buf = memalign(32, s.n + 32);
    if (!buf) {
        free(s.p);
        return false;
    }
    memcpy(buf, s.p, s.n);
    free(s.p);
    text_show(vfs_basename(path), buf, s.n, s.n, false);
    return true;
}

/* ---------------------------------------------------------------- BREFF */

/* NW4R effect set: "REFF" header, then a REFF section holding the project
 * name and a table of emitters (name, offset, size; offsets from the table).
 * The particle textures live in the matching .breft (the image viewer). */
bool breff_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    if (memcmp(b, "REFF", 4) || len < 0x40 || memcmp(b + 0x10, "REFF", 4)) {
        free(b);
        return false;
    }

    u32 nlen = be16(b + 0x24);
    u32 tab = (0x28 + nlen + 3) & ~3u;
    if (nlen > 256 || tab + 8 > len) {
        free(b);
        return false;
    }

    char proj[260];
    snprintf(proj, sizeof(proj), "%.*s", (int)nlen, (const char *)b + 0x28);
    u32 tsz = be32(b + tab), cnt = be16(b + tab + 4);

    sbuf s = { 0 };
    char line[320];
    snprintf(line, sizeof(line), "Effect set \"%s\": %u emitters\n\n", proj, (unsigned)cnt);
    put(&s, line, strlen(line));

    u32 p = tab + 8;
    for (u32 i = 0; i < cnt && p + 2 <= len && p < tab + tsz; i++) {
        u32 l = be16(b + p);
        if (p + 2 + l + 8 > len) break;
        u32 off = be32(b + p + 2 + l), sz = be32(b + p + 2 + l + 4);
        snprintf(line, sizeof(line), "%3u  %-40.*s  %6u bytes\n", (unsigned)i, (int)(l ? l - 1 : 0),
                 (const char *)b + p + 2, (unsigned)sz);
        put(&s, line, strlen(line));
        (void)off;
        p += 2 + l + 8;
    }

    static const char tail[] = "\nTextures for these emitters are in the matching .breft file.\n";
    put(&s, tail, sizeof(tail) - 1);
    free(b);

    if (!s.p) return false;
    u8 *buf = memalign(32, s.n + 32);
    if (!buf) {
        free(s.p);
        return false;
    }
    memcpy(buf, s.p, s.n);
    free(s.p);
    text_show(vfs_basename(path), buf, s.n, s.n, false);
    return true;
}

/* ---------------------------------------------------------------- shared */

static void putf(sbuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(sbuf *s, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) put(s, line, n < (int)sizeof(line) ? (u32)n : sizeof(line) - 1);
}

static float bef(const u8 *p)
{
    u32 v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* UTF-16BE up to a NUL (or `max` units); control codes 0x0E (MSBT) shown as {..} */
static void put_u16s(sbuf *s, const u8 *p, const u8 *end, u32 max)
{
    for (u32 i = 0; i < max && p + 2 <= end; i++) {
        u32 c = be16(p);
        p += 2;
        if (!c) break;
        /* tag: group, type, parameter size, parameters */
        if (c == 0x0E && p + 6 <= end) {
            u32 ps = be16(p + 4);
            putf(s, "{%u.%u}", (unsigned)be16(p), (unsigned)be16(p + 2));
            p += 6 + ps;
            continue;
        }
        if (c == '\n') {
            put(s, "\n    ", 5);
            continue;
        }
        if (c >= 0xD800 && c < 0xDC00 && p + 2 <= end) {
            c = 0x10000 + ((c - 0xD800) << 10) + (be16(p) - 0xDC00);
            p += 2;
        }
        if (c >= 0x10000) {
            put(s, "?", 1);
            continue;
        }
        put_cp(s, c);
    }
}

static bool show_sbuf(const char *path, sbuf *s)
{
    if (!s->p) return false;
    u8 *buf = memalign(32, s->n + 32);
    if (!buf) {
        free(s->p);
        return false;
    }
    memcpy(buf, s->p, s->n);
    free(s->p);
    text_show(vfs_basename(path), buf, s->n, s->n, false);
    return true;
}

/* ---------------------------------------------------------------- MSBT */

/* "MsgStdBn": LBL1 (hashed label groups), ATR1, TXT2 (string offsets, UTF-16).
 * Big endian on Wii; 16-byte aligned sections. Labels are matched to strings
 * by the index stored with each label. */
bool msbt_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    if (memcmp(b, "MsgStdBn", 8) || be16(b + 8) != 0xFEFF) {
        free(b);
        return false;
    }

    u32 nsec = be16(b + 0x0E), pos = 0x20, lbl = 0, lsz = 0, txt = 0, tsz = 0;
    for (u32 i = 0; i < nsec && pos + 16 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (!memcmp(b + pos, "LBL1", 4)) {
            lbl = pos + 16;
            lsz = sz;
        }
        if (!memcmp(b + pos, "TXT2", 4)) {
            txt = pos + 16;
            tsz = sz;
        }
        pos = (pos + 16 + sz + 15) & ~15u;
    }

    if (!txt || txt + tsz > len || tsz < 4) {
        free(b);
        return false;
    }

    u32 count = be32(b + txt);
    if (count > tsz / 4) {
        free(b);
        return false;
    }

    char **labels = calloc(count + 1, sizeof(char *));
    if (labels && lbl && lbl + lsz <= len && lsz >= 4) {
        u32 groups = be32(b + lbl);
        for (u32 g = 0; g < groups && lbl + 4 + g * 8 + 8 <= lbl + lsz; g++) {
            u32 n = be32(b + lbl + 4 + g * 8), o = lbl + be32(b + lbl + 8 + g * 8);
            for (u32 k = 0; k < n && o < lbl + lsz; k++) {
                u32 l = b[o];
                if (o + 1 + l + 4 > lbl + lsz) break;
                u32 idx = be32(b + o + 1 + l);
                if (idx < count && !labels[idx] && (labels[idx] = malloc(l + 1))) {
                    memcpy(labels[idx], b + o + 1, l);
                    labels[idx][l] = 0;
                }
                o += 1 + l + 4;
            }
        }
    }

    sbuf s = { 0 };
    putf(&s, "%u messages\n\n", (unsigned)count);
    for (u32 i = 0; i < count; i++) {
        u32 o = txt + be32(b + txt + 4 + i * 4);
        putf(&s, "%s: ", labels && labels[i] ? labels[i] : "");
        if (o < txt + tsz) put_u16s(&s, b + o, b + txt + tsz, 1u << 16);
        put(&s, "\n", 1);
    }

    if (labels) for (u32 i = 0; i < count; i++) free(labels[i]);
    free(labels);
    free(b);
    return show_sbuf(path, &s);
}

/* ---------------------------------------------------------------- BRLYT / BRLAN */

static void str16(char *out, const u8 *p, u32 n)
{
    u32 i = 0;
    for (; i < n && p[i]; i++) out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
    out[i] = 0;
}

/* Offset table (count u16 + pad, then count x {u32 offset, u32 pad}) of names. */
static void name_list(sbuf *s, const u8 *b, u32 len, u32 sec, const char *what)
{
    u32 n = be16(b + sec + 8), tab = sec + 12;
    putf(s, "%s (%u):\n", what, (unsigned)n);
    for (u32 i = 0; i < n && tab + i * 8 + 4 <= len; i++) {
        u32 o = tab + be32(b + tab + i * 8);
        if (o < len && memchr(b + o, 0, len - o)) putf(s, "  %s\n", (const char *)b + o);
    }
}

/* NW4R layout (RLYT): canvas, textures, fonts, materials and the pane tree
 * (pan1 / pic1 / txt1 / wnd1 / bnd1 nested with pas1 / pae1), with each
 * pane's size and position and the text of text boxes. Animations (RLAN):
 * length, loop, and which panes / materials are animated and how. */
bool layout_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    bool lyt = !memcmp(b, "RLYT", 4), lan = !memcmp(b, "RLAN", 4);
    if ((!lyt && !lan) || be16(b + 4) != 0xFEFF) {
        free(b);
        return false;
    }
    sbuf s = { 0 };

    u32 nsec = be16(b + 0x0E), pos = be16(b + 0x0C), depth = 0;
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (sz < 8 || pos + sz > len) break;

        const u8 *m = b + pos;
        char ind[40];
        memset(ind, ' ', sizeof(ind));
        ind[depth * 2 < sizeof(ind) - 1 ? depth * 2 : sizeof(ind) - 1] = 0;

        if (!memcmp(m, "lyt1", 4) && sz >= 0x14)
            putf(&s, "Layout %.0f x %.0f%s\n\n", bef(m + 0x0C), bef(m + 0x10),
                 m[8] ? ", centered" : "");
        else if (!memcmp(m, "txl1", 4)) {
            name_list(&s, b, len, pos, "Textures");
            put(&s, "\n", 1);
        } else if (!memcmp(m, "fnl1", 4)) {
            name_list(&s, b, len, pos, "Fonts");
            put(&s, "\n", 1);
        } else if (!memcmp(m, "mat1", 4)) {
            u32 n = be16(m + 8);
            putf(&s, "Materials (%u):", (unsigned)n);
            for (u32 k = 0; k < n && pos + 12 + k * 4 + 4 <= len; k++) {
                u32 o = pos + be32(m + 12 + k * 4);
                char nm[24];
                if (o + 20 <= len) {
                    str16(nm, b + o, 20);
                    putf(&s, "%s %s", k ? "," : "", nm);
                }
            }
            put(&s, "\n\nPanes:\n", 9);
        } else if (!memcmp(m, "pas1", 4) || !memcmp(m, "grs1", 4))
            depth++;
        else if (!memcmp(m, "pae1", 4) || !memcmp(m, "gre1", 4)) {
            if (depth) depth--;
        } else if ((!memcmp(m, "pan1", 4) || !memcmp(m, "pic1", 4) || !memcmp(m, "txt1", 4) ||
                    !memcmp(m, "wnd1", 4) || !memcmp(m, "bnd1", 4)) &&
                   sz >= 0x4C) {
            char nm[20];
            str16(nm, m + 0x0C, 16);
            putf(&s, "%s%.3s %-16s %4.0f x %-4.0f at %.0f, %.0f%s\n", ind, (const char *)m, nm,
                 bef(m + 0x44), bef(m + 0x48), bef(m + 0x24) + 0.0f, bef(m + 0x28) + 0.0f,
                 (m[8] & 1) ? "" : "  (hidden)");
            if (!memcmp(m, "txt1", 4) && sz >= 0x5C) {
                u32 so = be32(m + 0x54), sl = be16(m + 0x4E);
                if (so < sz && sl) {
                    putf(&s, "%s    \"", ind);
                    put_u16s(&s, m + so, m + sz, sl / 2);
                    put(&s, "\"\n", 2);
                }
            }
        } else if (!memcmp(m, "grp1", 4) && sz >= 0x1C) {
            char nm[20];
            str16(nm, m + 8, 16);
            putf(&s, "%sgroup %s (%u panes)\n", ind, nm, (unsigned)be16(m + 0x18));
        } else if (!memcmp(m, "pat1", 4) && sz >= 0x1C) {
            putf(&s, "Animation tag, order %u\n", (unsigned)be16(m + 8));
        } else if (!memcmp(m, "pai1", 4) && sz >= 0x14) {
            u32 nent = be16(m + 0x0E), eo = be32(m + 0x10);
            putf(&s, "%u frames%s, %u textures, %u animated panes / materials\n\n",
                 (unsigned)be16(m + 8), m[0x0A] ? ", looping" : "", (unsigned)be16(m + 0x0C),
                 (unsigned)nent);
            for (u32 k = 0; k < nent && eo + k * 4 + 4 <= sz; k++) {
                u32 e = be32(m + eo + k * 4);
                if (e + 0x18 > sz) continue;
                char nm[24];
                str16(nm, m + e, 20);
                u32 nt = m[e + 0x14];
                putf(&s, "  %-20s", nm);
                for (u32 t = 0; t < nt && e + 0x18 + t * 4 + 4 <= sz; t++) {
                    u32 to = e + be32(m + e + 0x18 + t * 4);
                    if (to + 4 <= sz) putf(&s, " %.4s", (const char *)m + to);
                }
                put(&s, "\n", 1);
            }
        }

        pos += sz;
    }
    free(b);
    return show_sbuf(path, &s);
}

/* ---------------------------------------------------------------- KMP */

/* Mario Kart Wii course data (RKMD): one line per section with its entry count. */
bool kmp_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    if (memcmp(b, "RKMD", 4) || len < 0x10) {
        free(b);
        return false;
    }

    static const char *const what[][2] = {
        { "KTPT", "start positions" },
        { "ENPT", "enemy route points" },
        { "ENPH", "enemy route groups" },
        { "ITPT", "item route points" },
        { "ITPH", "item route groups" },
        { "CKPT", "checkpoints" },
        { "CKPH", "checkpoint groups" },
        { "GOBJ", "objects" },
        { "POTI", "routes" },
        { "AREA", "areas" },
        { "CAME", "cameras" },
        { "JGPT", "respawn points" },
        { "CNPT", "cannon points" },
        { "MSPT", "battle end positions" },
        { "STGI", "stage info" },
    };

    u32 n = be16(b + 8), hl = be16(b + 10);
    sbuf s = { 0 };
    putf(&s, "Mario Kart Wii course data, version %u, %u sections\n\n", (unsigned)be32(b + 12),
         (unsigned)n);

    for (u32 i = 0; i < n && 0x10 + i * 4 + 4 <= len; i++) {
        u32 o = hl + be32(b + 0x10 + i * 4);
        if (o + 8 > len) continue;
        const char *d = "";
        for (size_t k = 0; k < sizeof(what) / sizeof(what[0]); k++)
            if (!memcmp(b + o, what[k][0], 4)) d = what[k][1];
        putf(&s, "%.4s  %5u  %s\n", (const char *)b + o, (unsigned)be16(b + o + 4), d);
        if (!memcmp(b + o, "STGI", 4) && o + 8 + 0x0C <= len && be16(b + o + 4))
            putf(&s, "      %u laps, pole position on the %s, %s start, lens flare %s\n",
                 (unsigned)b[o + 8], b[o + 9] ? "right" : "left", b[o + 0x0A] ? "narrow" : "normal",
                 b[o + 0x0B] ? "on" : "off");
    }
    free(b);
    return show_sbuf(path, &s);
}

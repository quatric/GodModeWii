/*
 * Viewer dispatch and the BMG message viewer. See media.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "media.h"
#include "vfs.h"
#include "ui.h"
#include "archive.h"

bool media_is_name(const char *name)
{
    return audio_is_name(name) || image_is_name(name) || model_is_name(name) || arc_name_has_ext(name, ".bmg");
}

bool media_open(const char *path)
{
    const char *name = vfs_basename(path);
    if (audio_is_name(name)) {
        if (audio_play(path)) return true;
        ui_message("AUDIO", C_YELLOW, "This file couldn't be played: its format or layout isn't supported.");
        return true;
    }
    if (model_is_name(name) && model_view(path)) return true;
    if (image_is_name(name) && image_view(path)) return true;
    if (arc_name_has_ext(name, ".bmg") && bmg_view(path)) return true;
    return false;
}

/* ---------------------------------------------------------------- BMG */

static u8 *load(const char *path, u32 *len)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return NULL;
    if (f.size > 8 * 1024 * 1024 || f.size < 0x20) { vfs_close(&f); return NULL; }
    u32 n = (u32)f.size, got = 0;
    u8 *b = memalign(32, (n + 31) & ~31u);
    while (b && got < n) {
        s32 r = vfs_read(&f, b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    vfs_close(&f);
    if (b && got != n) { free(b); return NULL; }
    *len = n;
    return b;
}

typedef struct { char *p; u32 n, cap; } sbuf;

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
    if (c < 0x80) { o[0] = (char)c; put(s, o, 1); }
    else if (c < 0x800) { o[0] = 0xC0 | c >> 6; o[1] = 0x80 | (c & 63); put(s, o, 2); }
    else { o[0] = 0xE0 | c >> 12; o[1] = 0x80 | ((c >> 6) & 63); o[2] = 0x80 | (c & 63); put(s, o, 3); }
}

/* "MESGbmg1": INF1 (entry table) + DAT1 (strings); encoding at 0x10:
 * 1 = CP1252, 2 = UTF-16BE, 3 = Shift-JIS (shown as raw bytes), 4 = UTF-8.
 * Control codes (0x1A) are shown as {..}. */
bool bmg_view(const char *path)
{
    u32 len;
    u8 *b = load(path, &len);
    if (!b) return false;
    if (memcmp(b, "MESGbmg1", 8)) { free(b); return false; }
    int enc = b[0x10] ? b[0x10] : 1;
    u32 nsec = be32(b + 0x0C), pos = 0x20, inf = 0, dat = 0;
    for (u32 i = 0; i < nsec && pos + 8 <= len; i++) {
        u32 sz = be32(b + pos + 4);
        if (!memcmp(b + pos, "INF1", 4)) inf = pos;
        if (!memcmp(b + pos, "DAT1", 4)) dat = pos;
        if (sz < 8) break;
        pos += sz;
    }
    if (!inf || !dat) { free(b); return false; }
    u32 count = be16(b + inf + 8), esz = be16(b + inf + 10);
    sbuf s = { 0 };
    char line[96];
    snprintf(line, sizeof(line), "%u messages, %s\n\n", (unsigned)count,
             enc == 2 ? "UTF-16" : enc == 3 ? "Shift-JIS" : enc == 4 ? "UTF-8" : "CP1252");
    put(&s, line, strlen(line));
    for (u32 i = 0; i < count && esz >= 4 && inf + 0x10 + (i + 1) * esz <= len; i++) {
        u32 off = dat + 8 + be32(b + inf + 0x10 + i * esz);
        snprintf(line, sizeof(line), "#%u: ", (unsigned)i);
        put(&s, line, strlen(line));
        u32 p = off;
        while (p < len) {
            u32 c;
            if (enc == 2) { if (p + 2 > len) break; c = be16(b + p); p += 2; }
            else { c = b[p++]; }
            if (!c) break;
            if (c == 0x1A) {                /* control code: length byte follows */
                u32 l = p < len ? b[p] : 0;
                u32 skip = enc == 2 ? (l > 2 ? l - 2 : 0) : (l > 1 ? l - 1 : 0);
                put(&s, "{...}", 5);
                p += skip;
                continue;
            }
            if (c == '\n') { put(&s, "\n    ", 5); continue; }
            if (enc == 2 || enc == 1) put_cp(&s, c);
            else { char ch = (char)c; put(&s, &ch, 1); }
        }
        put(&s, "\n", 1);
    }
    free(b);
    if (!s.p) return false;
    u8 *buf = memalign(32, s.n + 32);
    if (!buf) { free(s.p); return false; }
    memcpy(buf, s.p, s.n);
    free(s.p);
    text_show(vfs_basename(path), buf, s.n, s.n, false);
    return true;
}

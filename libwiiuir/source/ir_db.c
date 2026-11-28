/*
 * irdb.xml: WiiIR's device database schema, read one device at a time.
 * Shared, unchanged, between libwiiir and libwiiuir.
 *
 * Based on WiiIR by Dakota Thorpe, used with permission.
 * Copyright 2025 Dakota Thorpe
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ir_platform.h"

#define CHUNK 65536
#define DEVICE_MAX (16 << 20) /* a sane size for one <DeviceEntry> */

/* A window over a file (or a buffer, f == NULL) that tags are read from. */
typedef struct {
    FILE *f;
    char *buf;
    size_t len, pos;
    long base; /* file offset of buf[0] */
} reader;

/* At least `need` unread bytes, reading more as needed; false at the end. */
static bool avail(reader *r, size_t need)
{
    while (r->len - r->pos < need) {
        if (!r->f)
            return false;
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->base += r->pos;
        r->len -= r->pos;
        r->pos = 0;
        size_t got = fread(r->buf + r->len, 1, CHUNK - r->len, r->f);
        if (!got)
            return false;
        r->len += got;
    }
    return true;
}

/* The next tag's contents (between < and >, cut to `max`), skipping
 * comments; `end` is the offset just past it. False at the end. */
static bool next_tag(reader *r, char *tag, size_t max, long *start, long *end)
{
    for (;;) {
        for (;;) {
            if (!avail(r, 1))
                return false;
            char *lt = memchr(r->buf + r->pos, '<', r->len - r->pos);
            if (lt) {
                r->pos = lt - r->buf;
                break;
            }
            r->pos = r->len;
        }
        *start = r->base + r->pos;
        if (avail(r, 4) && !memcmp(r->buf + r->pos, "<!--", 4)) {
            r->pos += 4;
            for (;;) {
                if (!avail(r, 3))
                    return false;
                if (!memcmp(r->buf + r->pos, "-->", 3))
                    break;
                r->pos++;
            }
            r->pos += 3;
            continue;
        }
        r->pos++;
        size_t n = 0;
        char quote = 0;
        for (;;) {
            if (!avail(r, 1))
                return false;
            char c = r->buf[r->pos++];
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            if (n + 1 < max)
                tag[n++] = c;
        }
        tag[n] = 0;
        *end = r->base + r->pos;
        return true;
    }
}

static bool is(const char *tag, const char *name)
{
    size_t n = strlen(name);
    return !strncmp(tag, name, n) && (!tag[n] || isspace((unsigned char)tag[n]) || tag[n] == '/');
}

/* XML text with its entities resolved, into out (cut to size). */
static void decode(const char *s, size_t len, char *out, size_t size)
{
    static const struct { const char *name; char c; } ent[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''},
    };
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < size;) {
        if (s[i] != '&') {
            out[n++] = s[i++];
            continue;
        }
        bool done = false;
        for (size_t e = 0; e < sizeof ent / sizeof ent[0] && !done; e++) {
            size_t el = strlen(ent[e].name);
            if (i + 1 + el <= len && !strncmp(s + i + 1, ent[e].name, el)) {
                out[n++] = ent[e].c;
                i += 1 + el;
                done = true;
            }
        }
        if (!done && i + 2 < len && s[i + 1] == '#') {
            char *endp;
            bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
            unsigned long cp = strtoul(s + i + (hex ? 3 : 2), &endp, hex ? 16 : 10);
            if (endp < s + len && *endp == ';' && cp && cp < 0x110000 && n + 5 < size) {
                if (cp < 0x80) {
                    out[n++] = cp;
                } else if (cp < 0x800) {
                    out[n++] = 0xC0 | cp >> 6;
                    out[n++] = 0x80 | (cp & 0x3F);
                } else if (cp < 0x10000) {
                    out[n++] = 0xE0 | cp >> 12;
                    out[n++] = 0x80 | (cp >> 6 & 0x3F);
                    out[n++] = 0x80 | (cp & 0x3F);
                } else {
                    out[n++] = 0xF0 | cp >> 18;
                    out[n++] = 0x80 | (cp >> 12 & 0x3F);
                    out[n++] = 0x80 | (cp >> 6 & 0x3F);
                    out[n++] = 0x80 | (cp & 0x3F);
                }
                i = endp + 1 - s;
                done = true;
            }
        }
        if (!done)
            out[n++] = s[i++];
    }
    out[n] = 0;
}

/* An attribute's value; false when the tag has none. */
static bool attr(const char *tag, const char *key, char *out, size_t size)
{
    const char *p = tag;
    while (*p && !isspace((unsigned char)*p))
        p++;
    for (;;) {
        while (isspace((unsigned char)*p))
            p++;
        const char *name = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p) && *p != '/')
            p++;
        size_t nlen = p - name;
        while (isspace((unsigned char)*p))
            p++;
        if (*p != '=')
            return false;
        p++;
        while (isspace((unsigned char)*p))
            p++;
        char q = *p;
        if (q != '"' && q != '\'')
            return false;
        const char *v = ++p;
        while (*p && *p != q)
            p++;
        if (!*p)
            return false;
        if (nlen == strlen(key) && !strncmp(name, key, nlen)) {
            decode(v, p - v, out, size);
            return true;
        }
        p++;
    }
}

static char *own(ir_db *db, const char *s)
{
    if ((db->owned_count & (db->owned_count - 1)) == 0) { /* grow at powers of two */
        char **o = realloc(db->owned, (db->owned_count ? db->owned_count * 2 : 16) * sizeof *o);
        if (!o)
            return NULL;
        db->owned = o;
    }
    char *copy = strdup(s);
    if (copy)
        db->owned[db->owned_count++] = copy;
    return copy;
}

int IR_DbAdd(ir_db *db, const char *path)
{
    if (!db || !path || db->file_count >= IR_DB_MAX_FILES)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    reader r = {f, malloc(CHUNK), 0, 0, 0};
    if (!r.buf) {
        fclose(f);
        return -1;
    }

    /* categories are few: reuse the strings of the ones already seen */
    const char *cats[256];
    size_t ncats = 0;
    for (size_t i = 0; i < db->count; i++) {
        size_t c = 0;
        while (c < ncats && cats[c] != db->devices[i].category)
            c++;
        if (c == ncats && ncats < 256)
            cats[ncats++] = db->devices[i].category;
    }

    char tag[1024], val[256];
    long start, end;
    const char *maker = NULL;
    ir_db_device cur;
    bool in_device = false, err = false;
    size_t before = db->count;
    while (!err && next_tag(&r, tag, sizeof tag, &start, &end)) {
        if (is(tag, "Manufacturer")) {
            maker = own(db, attr(tag, "name", val, sizeof val) ? val : "Unknown");
            err = !maker;
        } else if (is(tag, "DeviceEntry") && tag[strlen(tag) - 1] != '/') {
            memset(&cur, 0, sizeof cur);
            cur.manufacturer = maker ? maker : (maker = own(db, "Unknown"));
            cur.name = own(db, attr(tag, "name", val, sizeof val) ? val : "Unnamed");
            if (!attr(tag, "category", val, sizeof val) || !val[0])
                strcpy(val, "Other");
            size_t c = 0;
            while (c < ncats && strcasecmp(cats[c], val))
                c++;
            cur.category = c < ncats ? cats[c] : own(db, val);
            if (c == ncats && ncats < 256 && cur.category)
                cats[ncats++] = cur.category;
            cur.file = db->file_count;
            cur.start = start;
            err = !cur.manufacturer || !cur.name || !cur.category;
            in_device = true;
        } else if (in_device && is(tag, "ButtonEntry")) {
            cur.buttons++;
        } else if (in_device && is(tag, "/DeviceEntry")) {
            in_device = false;
            cur.end = end;
            if ((db->count & (db->count - 1)) == 0) {
                ir_db_device *d = realloc(db->devices, (db->count ? db->count * 2 : 64) * sizeof *d);
                if (!d) {
                    err = true;
                    break;
                }
                db->devices = d;
            }
            db->devices[db->count++] = cur;
        }
    }
    free(r.buf);
    fclose(f);

    if (err || db->count == before || !(db->files[db->file_count] = strdup(path))) {
        db->count = before;
        return -1;
    }
    db->file_count++;
    return 0;
}

void IR_DbFree(ir_db *db)
{
    if (!db)
        return;
    for (size_t i = 0; i < db->owned_count; i++)
        free(db->owned[i]);
    for (int i = 0; i < db->file_count; i++)
        free(db->files[i]);
    free(db->owned);
    free(db->devices);
    memset(db, 0, sizeof *db);
}

int IR_DbFind(const ir_db *db, const char *category, const char *manufacturer, const char *name)
{
    for (size_t i = 0; db && name && i < db->count; i++) {
        const ir_db_device *d = &db->devices[i];
        if (!strcasecmp(d->name, name) && (!manufacturer || !strcasecmp(d->manufacturer, manufacturer)) &&
            (!category || !strcasecmp(d->category, category)))
            return (int)i;
    }
    return -1;
}

/* Whitespace-separated numbers in `base` (0: decimal or 0x hex); the count, or -1. */
static long numbers(const char *s, int base, uint32_t **out)
{
    long n = 0;
    for (const char *p = s; *p;) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        n++;
        while (*p && !isspace((unsigned char)*p))
            p++;
    }
    *out = n ? malloc(n * sizeof **out) : NULL;
    if (n && !*out)
        return -1;
    char *end;
    for (long i = 0; i < n; i++, s = end)
        (*out)[i] = strtoul(s, &end, base);
    return n;
}

/* <Data> into a signal: Flipper "Proto:address,command", Pronto "RAW:...",
 * or microsecond timings with a frequency attribute. */
static bool parse_data(const char *tag, const char *text, ir_signal *s)
{
    char v[32];
    while (isspace((unsigned char)*text))
        text++;
    if (attr(tag, "frequency", v, sizeof v)) {
        s->protocol = IR_PROTO_RAW;
        s->frequency = strtoul(v, NULL, 10);
        s->duty_cycle = attr(tag, "duty", v, sizeof v) ? strtof(v, NULL) : 0.33f;
        long n = numbers(text, 10, &s->timings);
        s->count = n > 0 ? n : 0;
        return n > 0;
    }
    if (!strncasecmp(text, "RAW:", 4)) {
        uint32_t *words;
        long n = numbers(text + 4, 16, &words);
        if (n < 4)
            return free(words), false;
        uint16_t *pronto = malloc(n * sizeof *pronto);
        s->timings = malloc(n * sizeof *s->timings);
        for (long i = 0; pronto && i < n; i++)
            pronto[i] = words[i];
        s->count = pronto && s->timings ? IR_ProntoToTimings(pronto, n, s->timings, n, &s->frequency) : 0;
        free(words);
        free(pronto);
        s->protocol = IR_PROTO_RAW;
        s->duty_cycle = 0.33f;
        return s->count > 0;
    }
    const char *colon = strchr(text, ':');
    if (!colon || colon - text >= 16)
        return false;
    char name[16];
    memcpy(name, text, colon - text);
    name[colon - text] = 0;
    int proto = IR_ProtocolFromName(name);
    char *end;
    s->address = strtoul(colon + 1, &end, 0);
    while (isspace((unsigned char)*end))
        end++;
    if (proto < 0 || proto == IR_PROTO_RAW || *end != ',')
        return false;
    s->command = strtoul(end + 1, NULL, 0);
    s->protocol = (ir_protocol)proto;
    return true;
}

int IR_DbLoad(const ir_db *db, size_t index, ir_remote *remote)
{
    if (!remote)
        return -1;
    memset(remote, 0, sizeof *remote);
    if (!db || index >= db->count)
        return -1;
    const ir_db_device *d = &db->devices[index];
    long size = d->end - d->start;
    if (size <= 0 || size > DEVICE_MAX)
        return -1;
    FILE *f = fopen(db->files[d->file], "rb");
    char *data = f ? malloc(size) : NULL;
    bool ok = data && !fseek(f, d->start, SEEK_SET) && fread(data, 1, size, f) == (size_t)size;
    if (f)
        fclose(f);
    if (!ok) {
        free(data);
        return -1;
    }

    reader r = {NULL, data, size, 0, d->start};
    char tag[1024], *text = NULL;
    long start, end;
    ir_signal cur;
    bool in_button = false, have = false, err = false;
    while (!err && next_tag(&r, tag, sizeof tag, &start, &end)) {
        if (is(tag, "ButtonEntry")) {
            if (have)
                free(cur.timings);
            memset(&cur, 0, sizeof cur);
            attr(tag, "name", cur.name, sizeof cur.name);
            in_button = true;
            have = false;
        } else if (in_button && is(tag, "Data") && tag[strlen(tag) - 1] != '/') {
            char *lt = memchr(r.buf + r.pos, '<', r.len - r.pos);
            size_t len = lt ? (size_t)(lt - (r.buf + r.pos)) : r.len - r.pos;
            free(text);
            text = malloc(len + 1);
            if (!text) {
                err = true;
                break;
            }
            decode(r.buf + r.pos, len, text, len + 1);
            if (have)
                free(cur.timings);
            cur.timings = NULL;
            have = parse_data(tag, text, &cur);
            if (!have) {
                free(cur.timings);
                cur.timings = NULL;
            }
        } else if (in_button && is(tag, "/ButtonEntry")) {
            in_button = false;
            if (!have || !cur.name[0]) {
                free(cur.timings);
                have = false;
                continue;
            }
            ir_signal *list = realloc(remote->signals, (remote->count + 1) * sizeof *list);
            if (!list) {
                err = true;
                break;
            }
            remote->signals = list;
            remote->signals[remote->count++] = cur;
            have = false;
        }
    }
    if (have)
        free(cur.timings);
    free(text);
    free(data);
    if (err || !remote->count) {
        IR_FreeRemote(remote);
        return -1;
    }
    return 0;
}

/*
 * MIME mail as text: the Wii Message Board's messages (WiiConnect24 mail, multipart with UTF-16 text in base64),
 * .eml files and the like. The main headers, then each text part decoded (base64 or quoted-printable; UTF-8,
 * UTF-16 or Latin-1), HTML with its tags taken out when there is no plain text, and the other parts listed.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mime.h"

typedef struct {
    char *p;
    size_t len, cap;
    bool bad;
} out_t;

static void put(out_t *o, const char *s, size_t n)
{
    if (o->bad) return;
    if (o->len + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 4096;
        while (o->len + n + 1 > c) c *= 2;
        char *q = realloc(o->p, c);
        if (!q) { o->bad = true; return; }
        o->p = q;
        o->cap = c;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
}
static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

static void put_utf8(out_t *o, unsigned c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); n = 4; }
    put(o, b, n);
}

/* text in `charset` to UTF-8 */
static void put_charset(out_t *o, const unsigned char *s, size_t n, const char *charset)
{
    bool le = !strncasecmp(charset, "utf-16le", 8), be = !le && !strncasecmp(charset, "utf-16", 6);
    if (!be && !le && n >= 2 && ((s[0] == 0xFE && s[1] == 0xFF) || (s[0] == 0xFF && s[1] == 0xFE))) be = true;
    if (be || le) {
        size_t i = 0;
        if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { le = true; be = false; i = 2; }
        else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { le = false; i = 2; }
        for (; i + 1 < n; i += 2) {
            unsigned c = le ? (unsigned)(s[i] | s[i + 1] << 8) : (unsigned)(s[i] << 8 | s[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                unsigned d = le ? (unsigned)(s[i + 2] | s[i + 3] << 8) : (unsigned)(s[i + 2] << 8 | s[i + 3]);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
            }
            if (c != '\r') put_utf8(o, c);
        }
        return;
    }
    bool latin = !strncasecmp(charset, "iso-8859", 8) || !strncasecmp(charset, "windows-125", 11) ||
                 !strncasecmp(charset, "latin", 5);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') continue;
        if (latin && s[i] >= 0x80) put_utf8(o, s[i]);
        else put(o, (const char *)s + i, 1);
    }
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* decodes in place; returns the new length */
static size_t unbase64(unsigned char *s, size_t n)
{
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v(s[i]);
        if (v < 0) continue;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; s[o++] = (unsigned char)(acc >> bits); }
    }
    return o;
}

static size_t unqp(unsigned char *s, size_t n, bool header)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {      /* soft line break */
            i++;
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
        } else if (s[i] == '=' && i + 2 < n && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            s[o++] = (unsigned char)(hexv(s[i + 1]) << 4 | hexv(s[i + 2]));
            i += 2;
        } else {
            s[o++] = header && s[i] == '_' ? ' ' : s[i];
        }
    }
    return o;
}

/* ---------------------------------------------------------------- headers */

typedef struct {
    const char *start, *end;   /* the header block, without the blank line */
    const char *body, *bend;
} entity_t;

static const char *line_end(const char *p, const char *end)
{
    const char *e = memchr(p, '\n', (size_t)(end - p));
    return e ? e : end;
}

/* splits headers from body */
static void split(const char *p, const char *end, entity_t *e)
{
    e->start = p;
    const char *q = p;
    while (q < end) {
        const char *le = line_end(q, end);
        size_t n = (size_t)(le - q);
        if (n == 0 || (n == 1 && q[0] == '\r')) {
            e->end = q;
            e->body = le < end ? le + 1 : end;
            e->bend = end;
            return;
        }
        q = le < end ? le + 1 : end;
    }
    e->end = end;
    e->body = e->bend = end;
}

/* a header's value, continuation lines joined; false if absent */
static bool header(const entity_t *e, const char *name, char *out, size_t sz)
{
    size_t nl = strlen(name);
    for (const char *q = e->start; q < e->end;) {
        const char *le = line_end(q, e->end);
        if ((size_t)(le - q) > nl && !strncasecmp(q, name, nl) && q[nl] == ':') {
            size_t o = 0;
            const char *v = q + nl + 1;
            for (;;) {
                while (v < le && (*v == ' ' || *v == '\t')) v++;
                while (v < le && *v != '\r' && o + 1 < sz) out[o++] = *v++;
                const char *nx = le < e->end ? le + 1 : e->end;
                if (nx < e->end && (*nx == ' ' || *nx == '\t')) {
                    if (o + 1 < sz) out[o++] = ' ';
                    v = nx;
                    le = line_end(nx, e->end);
                    continue;
                }
                break;
            }
            out[o] = 0;
            return true;
        }
        q = le < e->end ? le + 1 : e->end;
    }
    out[0] = 0;
    return false;
}

/* a parameter of a header value: charset=, boundary=, name= ... */
static bool param(const char *v, const char *name, char *out, size_t sz)
{
    size_t nl = strlen(name);
    for (const char *p = strchr(v, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (strncasecmp(q, name, nl) || q[nl] != '=') continue;
        q += nl + 1;
        size_t o = 0;
        if (*q == '"') {
            q++;
            while (*q && *q != '"' && o + 1 < sz) out[o++] = *q++;
        } else {
            while (*q && *q != ';' && *q != ' ' && o + 1 < sz) out[o++] = *q++;
        }
        out[o] = 0;
        return true;
    }
    out[0] = 0;
    return false;
}

/* RFC 2047 words (=?charset?B?...?= / ?Q?) in a header, to UTF-8 */
static void put_header_text(out_t *o, const char *v)
{
    bool after_word = false;
    while (*v) {
        const char *w = strstr(v, "=?");
        if (!w) { puts_(o, v); return; }
        if (w > v && !isspace((unsigned char)w[-1])) after_word = false;
        const char *q1 = strchr(w + 2, '?'), *q2 = q1 ? strchr(q1 + 1, '?') : NULL, *e = q2 ? strstr(q2 + 1, "?=") : NULL;
        if (!e || q2 - q1 != 2) { put(o, v, (size_t)(w - v) + 2); v = w + 2; continue; }
        const char *gap = v;                                   /* spaces between two encoded words go */
        while (gap < w && isspace((unsigned char)*gap)) gap++;
        if (gap != w || !after_word) put(o, v, (size_t)(w - v));
        char cs[32];
        size_t cl = (size_t)(q1 - w - 2) < sizeof(cs) - 1 ? (size_t)(q1 - w - 2) : sizeof(cs) - 1;
        memcpy(cs, w + 2, cl);
        cs[cl] = 0;
        size_t n = (size_t)(e - q2 - 1);
        unsigned char *t = malloc(n + 1);
        if (t) {
            memcpy(t, q2 + 1, n);
            n = toupper((unsigned char)q1[1]) == 'B' ? unbase64(t, n) : unqp(t, n, true);
            put_charset(o, t, n, cs);
            free(t);
        }
        v = e + 2;
        after_word = true;
    }
}

/* ---------------------------------------------------------------- bodies */

static void strip_html(out_t *o, const char *s, size_t n)
{
    bool tag = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '<') {
            tag = true;
            if (!strncasecmp(s + i, "<br", 3) || !strncasecmp(s + i, "<p", 2) || !strncasecmp(s + i, "</p", 3) ||
                !strncasecmp(s + i, "<div", 4))
                puts_(o, "\n");
        } else if (s[i] == '>') {
            tag = false;
        } else if (!tag) {
            if (s[i] == '&') {
                static const struct { const char *e; const char *c; } ent[] = {
                    { "&nbsp;", " " }, { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" },
                };
                size_t k = 0;
                while (k < 6 && strncasecmp(s + i, ent[k].e, strlen(ent[k].e))) k++;
                if (k < 6) { puts_(o, ent[k].c); i += strlen(ent[k].e) - 1; continue; }
            }
            if (s[i] != '\r') put(o, s + i, 1);
        }
    }
}

static void entity(out_t *o, const char *p, const char *end, int depth, bool *shown_text);

static void multipart(out_t *o, const entity_t *e, const char *type, const char *boundary, int depth, bool *shown)
{
    char delim[96];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t dl = strlen(delim);
    const char *parts[64], *ends[64];
    int np = 0;
    const char *q = e->body;
    const char *cur = NULL;
    while (q < e->bend) {
        const char *le = line_end(q, e->bend);
        if ((size_t)(le - q) >= dl && !memcmp(q, delim, dl)) {
            if (cur && np < 64) { parts[np] = cur; ends[np++] = q; }
            if ((size_t)(le - q) >= dl + 2 && q[dl] == '-' && q[dl + 1] == '-') { cur = NULL; break; }
            cur = le < e->bend ? le + 1 : e->bend;
        }
        q = le < e->bend ? le + 1 : e->bend;
    }
    if (cur && np < 64) { parts[np] = cur; ends[np++] = e->bend; }
    bool alt = !strcasecmp(type, "multipart/alternative");
    if (alt) {                                  /* one of them: plain text if there is some, else the first */
        int pick = 0;
        for (int i = 0; i < np; i++) {
            entity_t s;
            char ct[128];
            split(parts[i], ends[i], &s);
            header(&s, "Content-Type", ct, sizeof(ct));
            if (!ct[0] || !strncasecmp(ct, "text/plain", 10)) { pick = i; break; }
        }
        if (np) entity(o, parts[pick], ends[pick], depth + 1, shown);
        return;
    }
    for (int i = 0; i < np; i++) entity(o, parts[i], ends[i], depth + 1, shown);
}

static void entity(out_t *o, const char *p, const char *end, int depth, bool *shown)
{
    if (depth > 8) return;
    entity_t e;
    split(p, end, &e);
    char ct[256], cte[64], type[96], charset[40], name[128], disp[256];
    if (!header(&e, "Content-Type", ct, sizeof(ct))) snprintf(ct, sizeof(ct), "text/plain");
    header(&e, "Content-Transfer-Encoding", cte, sizeof(cte));
    header(&e, "Content-Disposition", disp, sizeof(disp));
    size_t tl = strcspn(ct, "; ");
    snprintf(type, sizeof(type), "%.*s", (int)(tl < sizeof(type) - 1 ? tl : sizeof(type) - 1), ct);
    if (!param(ct, "charset", charset, sizeof(charset))) snprintf(charset, sizeof(charset), "utf-8");
    if (!param(disp, "filename", name, sizeof(name))) param(ct, "name", name, sizeof(name));

    if (!strncasecmp(type, "multipart/", 10)) {
        char b[80];
        if (param(ct, "boundary", b, sizeof(b))) { multipart(o, &e, type, b, depth, shown); return; }
    }
    if (!strncasecmp(type, "message/rfc822", 14)) {
        puts_(o, "\n--- Forwarded message ---\n");
        entity(o, e.body, e.bend, depth + 1, shown);
        return;
    }
    size_t n = (size_t)(e.bend - e.body);
    unsigned char *raw = malloc(n + 1);
    if (!raw) { o->bad = true; return; }
    memcpy(raw, e.body, n);
    if (!strncasecmp(cte, "base64", 6)) n = unbase64(raw, n);
    else if (!strncasecmp(cte, "quoted-printable", 16)) n = unqp(raw, n, false);
    bool attach = !strncasecmp(disp, "attachment", 10) || name[0];
    if (!strncasecmp(type, "text/", 5) && !attach) {
        if (*shown) puts_(o, "\n\n");
        *shown = true;
        if (!strcasecmp(type, "text/html")) {
            out_t t = { 0 };
            put_charset(&t, raw, n, charset);
            if (t.p) strip_html(o, t.p, t.len);
            free(t.p);
        } else {
            put_charset(o, raw, n, charset);
        }
    } else {
        char line[300];
        snprintf(line, sizeof(line), "\n[Attachment: %s, %s, %u bytes]\n", name[0] ? name : "(no name)", type, (unsigned)n);
        puts_(o, line);
    }
    free(raw);
}

/* ---------------------------------------------------------------- the message */

bool mime_is(const unsigned char *buf, size_t len)
{
    entity_t e;
    size_t n = len < 16384 ? len : 16384;
    split((const char *)buf, (const char *)buf + n, &e);
    if (e.body == e.end + 0 && e.end == (const char *)buf + n) return false;     /* no blank line: no headers */
    char v[128];
    bool mime = header(&e, "MIME-Version", v, sizeof(v)), type = header(&e, "Content-Type", v, sizeof(v));
    int mailish = header(&e, "From", v, sizeof(v)) + header(&e, "Subject", v, sizeof(v)) + header(&e, "Date", v, sizeof(v)) +
                  header(&e, "To", v, sizeof(v));
    return (mime && (type || mailish)) || (type && mailish >= 2);
}

char *mime_render(const unsigned char *buf, size_t len, size_t *outlen)
{
    out_t o = { 0 };
    entity_t e;
    split((const char *)buf, (const char *)buf + len, &e);
    static const char *const show[] = { "From", "To", "Cc", "Date", "Subject" };
    for (int i = 0; i < 5; i++) {
        char v[1024];
        if (!header(&e, show[i], v, sizeof(v)) || !v[0]) continue;
        puts_(&o, show[i]);
        puts_(&o, ": ");
        put_header_text(&o, v);
        puts_(&o, "\n");
    }
    puts_(&o, "----------------------------------------\n");
    bool shown = false;
    entity(&o, (const char *)buf, (const char *)buf + len, 0, &shown);
    puts_(&o, "\n");
    if (o.bad) { free(o.p); return NULL; }
    *outlen = o.len;
    return o.p;
}

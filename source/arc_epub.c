/*
 * EPUB (2 and 3), read-only, opened like an archive: each chapter's text in reading order
 * ("chapter_001.txt"), the whole book in one file ("text.txt", with its title and author first), and
 * its pictures ("images/..."; JPEG, PNG and WebP open in the image viewer). Pages aren't laid out:
 * the XHTML becomes plain text, a line break at each block (paragraph, heading, list item, ...) and a
 * blank line after each paragraph and heading.
 *
 * Layout (EPUB Open Container Format 3.2): a zip; META-INF/container.xml names the package document
 * (<rootfile full-path>), whose <manifest> lists every item {id, href, media-type} and whose <spine>
 * gives the reading order (<itemref idref>); <dc:title> and <dc:creator> in its <metadata>.
 * Encrypted (DRM) books can't be read: their chapters don't unpack to text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "archive.h"

#define EPUB_MAX_FILE  (32u * 1024 * 1024)
#define EPUB_MAX_ENTRY (16u * 1024 * 1024)
#define EPUB_MAX_OUT   (40u * 1024 * 1024)

/* ---------------------------------------------------------------- zip */

typedef struct {
    char *name;
    uint32_t method, csize, usize, local;
} zent_t;

typedef struct {
    const uint8_t *b;
    uint32_t len, n;
    zent_t *e;
} zip_t;

static uint16_t l16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool zip_open(zip_t *z, const uint8_t *b, uint32_t len)
{
    memset(z, 0, sizeof(*z));
    z->b = b;
    z->len = len;
    if (len < 22) return false;
    uint32_t eocd = 0;
    bool found = false;
    for (uint32_t i = len - 22 + 1; i-- > 0 && len - i <= 22 + 65535;)
        if (le32(b + i) == 0x06054B50u) {
            eocd = i;
            found = true;
            break;
        }
    if (!found) return false;
    uint32_t cnt = l16(b + eocd + 10), cdsz = le32(b + eocd + 12), cd = le32(b + eocd + 16);
    if ((uint64_t)cd + cdsz > len || !cnt) return false;
    z->e = calloc(cnt, sizeof(zent_t));
    if (!z->e) return false;
    uint32_t p = cd;
    for (uint32_t i = 0; i < cnt && p + 46 <= len && le32(b + p) == 0x02014B50u; i++) {
        uint32_t nl = l16(b + p + 28), xl = l16(b + p + 30), cl = l16(b + p + 32);
        if ((uint64_t)p + 46 + nl > len) break;
        zent_t *e = &z->e[z->n];
        e->method = l16(b + p + 10);
        e->csize = le32(b + p + 20);
        e->usize = le32(b + p + 24);
        e->local = le32(b + p + 42);
        e->name = malloc(nl + 1);
        if (!e->name) break;
        memcpy(e->name, b + p + 46, nl);
        e->name[nl] = 0;
        z->n++;
        p += 46 + nl + xl + cl;
    }
    return z->n > 0;
}

static void zip_close(zip_t *z)
{
    for (uint32_t i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
}

static zent_t *zip_find(zip_t *z, const char *name)
{
    for (uint32_t i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return &z->e[i];
    for (uint32_t i = 0; i < z->n; i++)                  /* some books get the case wrong */
        if (!strcasecmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}

/* An entry's bytes, malloc'd with a NUL after them (so text can be scanned as a string). */
static uint8_t *zip_read(zip_t *z, const zent_t *e, uint32_t *out_len)
{
    if (!e || e->usize > EPUB_MAX_ENTRY || (uint64_t)e->local + 30 > z->len) return NULL;
    const uint8_t *lh = z->b + e->local;
    if (le32(lh) != 0x04034B50u) return NULL;
    uint64_t data = (uint64_t)e->local + 30 + l16(lh + 26) + l16(lh + 28);
    if (data + e->csize > z->len) return NULL;
    uint8_t *out = malloc((size_t)e->usize + 1);
    if (!out) return NULL;
    bool ok = false;
    if (e->method == 0 && e->csize == e->usize) {
        memcpy(out, z->b + data, e->usize);
        ok = true;
    } else if (e->method == 8) {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, -15) == Z_OK) {
            s.next_in = (Bytef *)(z->b + data);
            s.avail_in = e->csize;
            s.next_out = out;
            s.avail_out = e->usize;
            int rc = inflate(&s, Z_FINISH);
            ok = (rc == Z_STREAM_END || rc == Z_BUF_ERROR) && s.total_out == e->usize;
            inflateEnd(&s);
        }
    }
    if (!ok) {
        free(out);
        return NULL;
    }
    out[e->usize] = 0;
    *out_len = e->usize;
    return out;
}

/* ---------------------------------------------------------------- output */

typedef struct {
    uint8_t *p;
    uint32_t n, cap;
    bool bad;
} sb_t;

static void sb_put(sb_t *b, const void *s, uint32_t n)
{
    if (b->bad || !n) return;
    if ((uint64_t)b->n + n > EPUB_MAX_OUT) {
        b->bad = true;
        return;
    }
    if (b->n + n > b->cap) {
        uint64_t nc = b->cap ? b->cap : 4096;
        while (nc < (uint64_t)b->n + n) nc *= 2;
        if (nc > EPUB_MAX_OUT) nc = EPUB_MAX_OUT;
        uint8_t *np = realloc(b->p, (size_t)nc);
        if (!np) {
            b->bad = true;
            return;
        }
        b->p = np;
        b->cap = (uint32_t)nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void sb_c(sb_t *b, char c) { sb_put(b, &c, 1); }
static void sb_s(sb_t *b, const char *s) { sb_put(b, s, (uint32_t)strlen(s)); }

static void sb_utf8(sb_t *b, uint32_t u)
{
    char t[4];
    if (u < 0x80) t[0] = (char)u, sb_put(b, t, 1);
    else if (u < 0x800) t[0] = (char)(0xC0 | u >> 6), t[1] = (char)(0x80 | (u & 63)), sb_put(b, t, 2);
    else if (u < 0x10000)
        t[0] = (char)(0xE0 | u >> 12), t[1] = (char)(0x80 | ((u >> 6) & 63)), t[2] = (char)(0x80 | (u & 63)), sb_put(b, t, 3);
    else if (u < 0x110000)
        t[0] = (char)(0xF0 | u >> 18), t[1] = (char)(0x80 | ((u >> 12) & 63)), t[2] = (char)(0x80 | ((u >> 6) & 63)),
        t[3] = (char)(0x80 | (u & 63)), sb_put(b, t, 4);
}

/* ---------------------------------------------------------------- XML / XHTML */

static const struct { const char *n; uint16_t u; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "copy", 0xA9 },
    { "reg", 0xAE }, { "trade", 0x2122 }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "agrave", 0xE0 }, { "aacute", 0xE1 },
    { "acirc", 0xE2 }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "szlig", 0xDF },
    { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA },
    { "times", 0xD7 }, { "shy", 0 }, { "zwnj", 0 }, { "zwj", 0 }, { "thinsp", ' ' }, { "ensp", ' ' }, { "emsp", ' ' },
};

/* An entity at s ("&...;"): its code point and length; 0 length when it isn't one. */
static uint32_t entity(const char *s, uint32_t *cp)
{
    const char *semi = strchr(s, ';');
    if (!semi || semi - s > 12) return 0;
    uint32_t len = (uint32_t)(semi - s) + 1;
    if (s[1] == '#') {
        *cp = (uint32_t)(s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10));
        return len;
    }
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++)
        if (strlen(ents[i].n) == len - 2 && !strncmp(s + 1, ents[i].n, len - 2)) {
            *cp = ents[i].u;
            return len;
        }
    return 0;
}

/* The local name of a tag starting at t (after '<' and an optional '/'), without its prefix, lower case. */
static void tag_name(const char *t, char *out, size_t n)
{
    if (*t == '/') t++;
    size_t k = 0;
    const char *start = t;
    while (*t && *t != '>' && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\r' && *t != '/') {
        if (*t == ':') start = t + 1;
        t++;
    }
    for (const char *p = start; p < t && k + 1 < n; p++) out[k++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    out[k] = 0;
}

/* An attribute's value inside the tag [t, end), entities decoded, into out. */
static bool attr(const char *t, const char *end, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = t; p + nl + 1 < end; p++) {
        if (strncasecmp(p, name, nl) || (p > t && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r' && p[-1] != ':'))
            continue;
        const char *q = p + nl;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != '=') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        sb_t v = { 0 };
        while (q < end && *q != quote) {
            uint32_t cp, l;
            if (*q == '&' && (l = entity(q, &cp))) {
                sb_utf8(&v, cp);
                q += l;
            } else sb_c(&v, *q++);
        }
        uint32_t k = v.n < n - 1 ? v.n : (uint32_t)n - 1;
        if (k) memcpy(out, v.p, k);
        out[k] = 0;
        free(v.p);
        return true;
    }
    return false;
}

static bool in_list(const char *n, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(n, list[i])) return true;
    return false;
}

/* The trailing whitespace state of the output: so spaces and line breaks don't pile up. */
static void ws_space(sb_t *o)
{
    if (o->n && o->p[o->n - 1] != ' ' && o->p[o->n - 1] != '\n') sb_c(o, ' ');
}
static void ws_lines(sb_t *o, int want)       /* end the line, with want - 1 blank lines after it */
{
    if (!o->n) return;
    while (o->n && o->p[o->n - 1] == ' ') o->n--;
    int have = 0;
    for (uint32_t i = o->n; i > 0 && o->p[i - 1] == '\n'; i--) have++;
    for (; have < want; have++) sb_c(o, '\n');
}

/* "%20" and friends in an href */
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            char h[3] = { r[1], r[2], 0 };
            char *e;
            long v = strtol(h, &e, 16);
            if (!*e) {
                *w++ = (char)v;
                r += 2;
                continue;
            }
        }
        *w++ = *r;
    }
    *w = 0;
}

/* XHTML to plain text: tags dropped, block tags end lines, <head>, <script> and <style> skipped,
 * whitespace collapsed (kept in <pre>), entities decoded; an image becomes [image: alt]. */
static void xhtml_text(const char *s, sb_t *o, char *title, size_t tn)
{
    static const char *const para[] = { "p", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "table",
                                        "ul", "ol", "dl", "figure", "hr", "section", "article", "header", "footer",
                                        "aside", "nav", NULL };
    static const char *const line[] = { "br", "div", "li", "tr", "dt", "dd", "figcaption", "caption", "td", "th", NULL };
    int skip = 0, pre = 0;
    for (const char *p = s; *p;) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : p + strlen(p);
                continue;
            }
            if (!strncmp(p, "<![CDATA[", 9)) {
                const char *e = strstr(p + 9, "]]>");
                if (!skip) sb_put(o, p + 9, e ? (uint32_t)(e - p - 9) : 0);
                p = e ? e + 3 : p + strlen(p);
                continue;
            }
            const char *end = strchr(p, '>');
            if (!end) break;
            bool close = p[1] == '/', selfc = end > p && end[-1] == '/';
            char n[24];
            tag_name(p + 1, n, sizeof(n));
            if (!strcmp(n, "head") || !strcmp(n, "script") || !strcmp(n, "style")) {
                if (!strcmp(n, "head") && !close) {           /* the chapter title is in <head> */
                    const char *ts = strstr(p, "<title"), *he = strstr(p, "</head");
                    if (ts && (!he || ts < he) && title && !title[0]) {
                        const char *tb = strchr(ts, '>'), *te = tb ? strstr(tb, "</title") : NULL;
                        if (tb && te) {
                            sb_t tt = { 0 };
                            for (const char *q = tb + 1; q < te; q++) sb_c(&tt, (*q == '\n' || *q == '\t') ? ' ' : *q);
                            uint32_t k = tt.n < tn - 1 ? tt.n : (uint32_t)tn - 1;
                            if (k) memcpy(title, tt.p, k);
                            title[k] = 0;
                            free(tt.p);
                        }
                    }
                }
                if (!selfc) skip += close ? (skip > 0 ? -1 : 0) : 1;
            } else if (!skip) {
                if (!strcmp(n, "pre")) pre += close ? (pre > 0 ? -1 : 0) : 1;
                if (in_list(n, para)) ws_lines(o, 2);
                else if (in_list(n, line)) {
                    if (!strcmp(n, "td") || !strcmp(n, "th")) ws_space(o);
                    else {
                        ws_lines(o, 1);
                        if (!close && !strcmp(n, "li")) sb_s(o, "\xE2\x80\xA2 ");
                    }
                } else if (!strcmp(n, "img") || !strcmp(n, "image")) {
                    char alt[128], src[256];
                    ws_space(o);
                    if (attr(p, end, "alt", alt, sizeof(alt)) && alt[0]) sb_s(o, "[image: "), sb_s(o, alt), sb_s(o, "]");
                    else if (attr(p, end, "src", src, sizeof(src)) || attr(p, end, "href", src, sizeof(src))) {
                        char *h = strchr(src, '#'), *bn = strrchr(src, '/');
                        if (h) *h = 0;
                        url_decode(src);
                        sb_s(o, "[image: "), sb_s(o, bn ? bn + 1 : src), sb_s(o, "]");
                    } else sb_s(o, "[image]");
                    ws_space(o);
                }
            }
            p = end + 1;
            continue;
        }
        if (skip) {
            p++;
            continue;
        }
        uint32_t cp, l;
        if (*p == '&' && (l = entity(p, &cp))) {
            if (cp == 0xA0) cp = ' ';
            if (cp == ' ' && !pre) ws_space(o);
            else sb_utf8(o, cp);
            p += l;
            continue;
        }
        char c = *p++;
        if (!pre && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) ws_space(o);
        else if (pre && c == '\r') continue;
        else sb_c(o, c);
    }
    ws_lines(o, 1);
}

/* base directory + href, with "./" and "../" folded */
static void join(const char *dir, const char *href, char *out, size_t n)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s%s", href[0] == '/' ? "" : dir, href[0] == '/' ? href + 1 : href);
    char *h = strchr(tmp, '#');
    if (h) *h = 0;
    url_decode(tmp);
    char *seg[64];
    int ns = 0;
    for (char *t = strtok(tmp, "/"); t && ns < 64; t = strtok(NULL, "/")) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) {
            if (ns) ns--;
            continue;
        }
        seg[ns++] = t;
    }
    out[0] = 0;
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", i ? "/" : "", seg[i]);
    }
}

/* ---------------------------------------------------------------- the archive */

typedef struct {
    char id[96], href[400], type[64];
} item_t;

typedef struct {
    archive_t *a;
    sb_t blob;
} out_t;

static void add_file(out_t *o, const char *name, const uint8_t *p, uint32_t n)
{
    uint32_t at = o->blob.n;
    sb_put(&o->blob, p, n);
    if (o->blob.bad) return;
    tree_add_path(&o->a->tree, name, false, n, at, 0, 0);
}

static int32_t mem_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    return a->src.read(a->src.ctx, nd->a + off, buf, len);
}

/* the text of the first <tag>...</tag> (any prefix) in s */
static void elem_text(const char *s, const char *tag, char *out, size_t n)
{
    out[0] = 0;
    for (const char *p = s; (p = strchr(p, '<'));) {
        char nm[32];
        tag_name(p + 1, nm, sizeof(nm));
        const char *end = strchr(p, '>');
        if (!end) return;
        if (p[1] != '/' && !strcmp(nm, tag)) {
            const char *q = end + 1, *c = NULL;
            for (const char *r = q; (r = strstr(r, "</")); r += 2) {
                char cn[32];
                tag_name(r + 1, cn, sizeof(cn));
                if (!strcmp(cn, tag)) {
                    c = r;
                    break;
                }
            }
            if (!c) return;
            sb_t t = { 0 };
            for (const char *r = q; r < c;) {
                uint32_t cp, l;
                if (*r == '<') {
                    const char *e = strchr(r, '>');
                    r = e && e < c ? e + 1 : c;
                } else if (*r == '&' && (l = entity(r, &cp))) sb_utf8(&t, cp), r += l;
                else if (*r == '\n' || *r == '\t' || *r == '\r') sb_c(&t, ' '), r++;
                else sb_c(&t, *r++);
            }
            uint32_t k = t.n < n - 1 ? t.n : (uint32_t)n - 1;
            if (k) memcpy(out, t.p, k);
            out[k] = 0;
            free(t.p);
            return;
        }
        p = end + 1;
    }
}

bool arc_epub_probe(const src_t *f, const char *name)
{
    uint8_t h[58];
    if (f->size < 58 || !src_read_full(f, 0, h, sizeof(h)) || le32(h) != 0x04034B50u) return false;
    /* the "mimetype" entry, stored first, as the format asks; or a zip named .epub */
    return (!memcmp(h + 30, "mimetype", 8) && !memcmp(h + 38, "application/epub+zip", 20)) ||
           arc_name_has_ext(name, ".epub");
}

archive_t *arc_epub_open(const src_t *f, const char *name)
{
    (void)name;
    if (f->size > EPUB_MAX_FILE) {
        g_archive_err = "This EPUB is too big to open";
        return NULL;
    }
    uint32_t len = (uint32_t)f->size;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf || !src_read_full(f, 0, buf, len)) {
        free(buf);
        g_archive_err = "Read error";
        return NULL;
    }
    zip_t z;
    if (!zip_open(&z, buf, len)) {
        zip_close(&z);
        free(buf);
        g_archive_err = "Not a readable zip";
        return NULL;
    }

    archive_t *a = NULL;
    item_t *items = NULL;
    uint8_t *opf = NULL, *cont = NULL;
    uint32_t nl;
    char opf_path[400] = "", opf_dir[400] = "";

    cont = zip_read(&z, zip_find(&z, "META-INF/container.xml"), &nl);
    if (cont) {
        const char *r = strstr((char *)cont, "rootfile");
        while (r && !opf_path[0]) {
            const char *e = strchr(r, '>');
            if (!e) break;
            attr(r, e, "full-path", opf_path, sizeof(opf_path));
            r = strstr(e, "rootfile");
        }
    }
    if (!opf_path[0])                                   /* no container: the first .opf */
        for (uint32_t i = 0; i < z.n; i++)
            if (arc_name_has_ext(z.e[i].name, ".opf")) {
                snprintf(opf_path, sizeof(opf_path), "%s", z.e[i].name);
                break;
            }
    opf = zip_read(&z, zip_find(&z, opf_path), &nl);
    if (!opf) {
        g_archive_err = zip_find(&z, "META-INF/encryption.xml") ? "This EPUB is encrypted (DRM)"
                                                               : "No package document (.opf) in this EPUB";
        goto out;
    }
    snprintf(opf_dir, sizeof(opf_dir), "%s", opf_path);
    char *sl = strrchr(opf_dir, '/');
    if (sl) sl[1] = 0;
    else opf_dir[0] = 0;

    /* manifest */
    uint32_t ni = 0, capi = 0;
    for (const char *p = (char *)opf; (p = strchr(p, '<'));) {
        const char *e = strchr(p, '>');
        if (!e) break;
        char nm[24];
        tag_name(p + 1, nm, sizeof(nm));
        if (p[1] != '/' && !strcmp(nm, "item")) {
            if (ni == capi) {
                capi = capi ? capi * 2 : 64;
                item_t *nx = realloc(items, capi * sizeof(item_t));
                if (!nx) break;
                items = nx;
            }
            item_t *it = &items[ni];
            memset(it, 0, sizeof(*it));
            char href[400] = "";
            attr(p, e, "id", it->id, sizeof(it->id));
            attr(p, e, "href", href, sizeof(href));
            attr(p, e, "media-type", it->type, sizeof(it->type));
            join(opf_dir, href, it->href, sizeof(it->href));
            if (it->href[0]) ni++;
        }
        p = e + 1;
    }

    a = archive_alloc("EPUB");
    if (!a) goto out;
    a->read = mem_read;
    out_t o = { a, { 0 } };

    char btitle[256], author[256];
    elem_text((char *)opf, "title", btitle, sizeof(btitle));
    elem_text((char *)opf, "creator", author, sizeof(author));
    sb_t all = { 0 };
    if (btitle[0]) sb_s(&all, btitle), sb_c(&all, '\n');
    if (author[0]) sb_s(&all, "by "), sb_s(&all, author), sb_c(&all, '\n');
    if (all.n) sb_c(&all, '\n');

    /* spine, in order */
    unsigned ch = 0;
    for (const char *p = (char *)opf; (p = strchr(p, '<')) && !o.blob.bad;) {
        const char *e = strchr(p, '>');
        if (!e) break;
        char nm[24], idref[96] = "";
        tag_name(p + 1, nm, sizeof(nm));
        const char *tag = p;
        p = e + 1;
        if (tag[1] == '/' || strcmp(nm, "itemref") || !attr(tag, e, "idref", idref, sizeof(idref))) continue;
        item_t *it = NULL;
        for (uint32_t i = 0; i < ni; i++)
            if (!strcmp(items[i].id, idref)) it = &items[i];
        if (!it) continue;
        uint32_t dl;
        uint8_t *d = zip_read(&z, zip_find(&z, it->href), &dl);
        if (!d) continue;
        sb_t text = { 0 };
        char ctitle[160] = "";
        xhtml_text((char *)d, &text, ctitle, sizeof(ctitle));
        free(d);
        while (text.n && (text.p[0] == '\n' || text.p[0] == ' ')) memmove(text.p, text.p + 1, --text.n);
        if (!text.n) {                                  /* a cover page or an empty divider */
            free(text.p);
            continue;
        }
        char fn[48];
        snprintf(fn, sizeof(fn), "chapter_%03u.txt", ++ch);
        add_file(&o, fn, text.p, text.n);
        char hd[220];
        snprintf(hd, sizeof(hd), "---- %u%s%s ----\n", ch, ctitle[0] ? ": " : "", ctitle);
        sb_s(&all, hd);
        sb_put(&all, text.p, text.n);
        sb_c(&all, '\n');
        free(text.p);
    }
    if (!o.blob.bad && all.n) add_file(&o, "text.txt", all.p, all.n);
    free(all.p);

    /* pictures, as they are */
    for (uint32_t i = 0; i < ni && !o.blob.bad; i++) {
        if (strncmp(items[i].type, "image/", 6)) continue;
        uint32_t dl;
        uint8_t *d = zip_read(&z, zip_find(&z, items[i].href), &dl);
        if (!d) continue;
        const char *bn = strrchr(items[i].href, '/');
        char fn[300];
        snprintf(fn, sizeof(fn), "images/%s", bn ? bn + 1 : items[i].href);
        add_file(&o, fn, d, dl);
        free(d);
    }

    if (!ch) {
        g_archive_err = zip_find(&z, "META-INF/encryption.xml") ? "This EPUB is encrypted (DRM)" : "No chapters found in this EPUB";
        archive_close(a);
        free(o.blob.p);
        a = NULL;
        goto out;
    }
    a->base = *f;
    a->src = src_from_memory(o.blob.p, o.blob.n);
    if (!a->src.ctx) {
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        a = NULL;
    }
out:
    free(items);
    free(opf);
    free(cont);
    zip_close(&z);
    free(buf);
    return a;
}

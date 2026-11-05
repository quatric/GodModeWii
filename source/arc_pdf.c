/*
 * PDF, read-only, opened like an archive: each page's text ("page_001.txt"), all of it in one file
 * ("text.txt", with the title and author when the document has them), and every embedded image
 * ("page_001_img01.jpg" for JPEG data as stored, ".png" for the rest, converted to 8-bit RGB / gray).
 * Pages are not rendered: the text comes out in the order it is drawn, with line breaks where the
 * text position moves down, which reads well for most documents but not for multi-column layouts.
 *
 * What it reads (written from the PDF 1.7 specification, ISO 32000-1):
 *   objects    found by scanning for "n g obj" (no xref needed, so damaged files still open), and
 *              those inside object streams (/Type /ObjStm); a later definition wins
 *   filters    FlateDecode (with PNG / TIFF predictors), LZWDecode, ASCIIHexDecode, ASCII85Decode,
 *              RunLengthDecode; DCTDecode is kept as the JPEG it is. JPX, JBIG2 and CCITT images are
 *              skipped, and encrypted documents are refused.
 *   text       Tj TJ ' " Td TD T* Tm, inside forms too; codes through the font's ToUnicode CMap,
 *              else its /Differences glyph names, else WinAnsi / Latin-1. A composite font with no
 *              ToUnicode can't be mapped and is marked as such.
 *   images     Gray, RGB, CMYK, ICCBased, Indexed and image masks, 1 / 2 / 4 / 8 bits per component.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "archive.h"

#define PDF_MAX_FILE   (32u * 1024 * 1024)
#define PDF_MAX_OUT    (40u * 1024 * 1024)        /* everything the archive holds */
#define PDF_MAX_STREAM (24u * 1024 * 1024)
#define PDF_MAX_PAGES  5000
#define PDF_MAX_OBJ    4000000

/* ---------------------------------------------------------------- values */

enum { V_NULL, V_BOOL, V_NUM, V_STR, V_NAME, V_ARR, V_DICT, V_REF, V_OP };

typedef struct pv pv;
struct pv {
    uint8_t t;
    double num;                 /* V_NUM, V_BOOL, V_REF (object number) */
    char *s;                    /* V_STR / V_NAME / V_OP bytes (NUL-terminated too) */
    uint32_t n;                 /* string length, or item count */
    pv **v;                     /* V_ARR items, V_DICT values */
    char **k;                   /* V_DICT keys */
    const uint8_t *stm;         /* a stream's raw bytes (V_DICT) */
    uint32_t stmlen;
};

typedef struct chunk {
    struct chunk *next;
    size_t used, cap;
    uint8_t data[];
} chunk;

typedef struct {
    uint8_t type;               /* 0 none, 1 at an offset in the file, 2 inside an object stream */
    uint32_t off;               /* file offset (1) / offset in the object stream's data (2) */
    uint32_t stm;               /* (2) which decoded object stream */
} oref;

typedef struct {
    uint8_t *buf;
    uint32_t len;
    chunk *arena;
    oref *idx;
    pv **cache;
    uint8_t *busy;
    uint32_t nobj;
    uint8_t **ostm;             /* decoded object streams */
    uint32_t *ostmlen, nostm;
} pdf_t;

static void *amalloc(pdf_t *d, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (!d->arena || d->arena->used + n > d->arena->cap) {
        size_t cap = n > 65536 ? n : 65536;
        chunk *c = malloc(sizeof(chunk) + cap);
        if (!c) return NULL;
        c->next = d->arena;
        c->used = 0;
        c->cap = cap;
        d->arena = c;
    }
    void *p = d->arena->data + d->arena->used;
    d->arena->used += n;
    return p;
}

static pv *newv(pdf_t *d, uint8_t t)
{
    pv *v = amalloc(d, sizeof(pv));
    if (v) memset(v, 0, sizeof(*v)), v->t = t;
    return v;
}

static pv NULLV = { V_NULL, 0, NULL, 0, NULL, NULL, NULL, 0 };

/* ---------------------------------------------------------------- lexer / parser */

typedef struct {
    pdf_t *d;
    const uint8_t *b;
    uint32_t len, pos;
} lex_t;

static bool is_ws(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static bool is_delim(uint8_t c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

static void skip_ws(lex_t *l)
{
    while (l->pos < l->len) {
        uint8_t c = l->b[l->pos];
        if (is_ws(c)) l->pos++;
        else if (c == '%')
            while (l->pos < l->len && l->b[l->pos] != '\n' && l->b[l->pos] != '\r') l->pos++;
        else break;
    }
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static pv *mkstr(pdf_t *d, uint8_t t, const uint8_t *s, uint32_t n)
{
    pv *v = newv(d, t);
    if (!v || !(v->s = amalloc(d, n + 1))) return NULL;
    memcpy(v->s, s, n);
    v->s[n] = 0;
    v->n = n;
    return v;
}

static bool read_int(lex_t *l, uint32_t *at, long *out)
{
    uint32_t p = *at;
    long v = 0;
    if (p >= l->len || l->b[p] < '0' || l->b[p] > '9') return false;
    while (p < l->len && l->b[p] >= '0' && l->b[p] <= '9') v = v * 10 + (l->b[p++] - '0');
    *out = v;
    *at = p;
    return true;
}

static pv *parse(lex_t *l, int depth);

/* the next token; NULL at the end. ']' and '>>' come back as V_OP so callers can stop on them */
static pv *parse(lex_t *l, int depth)
{
    pdf_t *d = l->d;
    skip_ws(l);
    if (l->pos >= l->len || depth > 64) return NULL;
    uint8_t c = l->b[l->pos];

    if (c == '[') {
        l->pos++;
        pv *a = newv(d, V_ARR);
        uint32_t cap = 0;
        pv **items = NULL;
        for (;;) {
            pv *x = parse(l, depth + 1);
            if (!x || (x->t == V_OP && !strcmp(x->s, "]"))) break;
            if (a->n == cap) {
                uint32_t nc = cap ? cap * 2 : 8;
                pv **ni = amalloc(d, nc * sizeof(pv *));
                if (!ni) break;
                if (cap) memcpy(ni, items, cap * sizeof(pv *));
                items = ni;
                cap = nc;
            }
            items[a->n++] = x;
        }
        a->v = items;
        return a;
    }
    if (c == '<' && l->pos + 1 < l->len && l->b[l->pos + 1] == '<') {
        l->pos += 2;
        pv *o = newv(d, V_DICT);
        uint32_t cap = 0;
        for (;;) {
            pv *key = parse(l, depth + 1);
            if (!key || (key->t == V_OP && !strcmp(key->s, ">>"))) break;
            if (key->t != V_NAME) continue;
            pv *val = parse(l, depth + 1);
            if (!val) break;
            if (val->t == V_OP && !strcmp(val->s, ">>")) break;
            if (o->n == cap) {
                uint32_t nc = cap ? cap * 2 : 8;
                pv **nv = amalloc(d, nc * sizeof(pv *));
                char **nk = amalloc(d, nc * sizeof(char *));
                if (!nv || !nk) break;
                if (cap) memcpy(nv, o->v, cap * sizeof(pv *)), memcpy(nk, o->k, cap * sizeof(char *));
                o->v = nv;
                o->k = nk;
                cap = nc;
            }
            o->k[o->n] = key->s;
            o->v[o->n++] = val;
        }
        return o;
    }
    if (c == '>' && l->pos + 1 < l->len && l->b[l->pos + 1] == '>') {
        l->pos += 2;
        return mkstr(d, V_OP, (const uint8_t *)">>", 2);
    }
    if (c == ']') {
        l->pos++;
        return mkstr(d, V_OP, (const uint8_t *)"]", 1);
    }
    if (c == '<') {                                   /* hex string */
        l->pos++;
        uint32_t start = l->pos;
        while (l->pos < l->len && l->b[l->pos] != '>') l->pos++;
        uint32_t end = l->pos;
        if (l->pos < l->len) l->pos++;
        uint8_t *tmp = amalloc(d, (end - start) / 2 + 2);
        if (!tmp) return NULL;
        uint32_t n = 0;
        int hi = -1;
        for (uint32_t i = start; i < end; i++) {
            int h = hexv(l->b[i]);
            if (h < 0) continue;
            if (hi < 0) hi = h;
            else tmp[n++] = (uint8_t)(hi << 4 | h), hi = -1;
        }
        if (hi >= 0) tmp[n++] = (uint8_t)(hi << 4);
        pv *v = newv(d, V_STR);
        if (v) v->s = (char *)tmp, v->n = n;
        return v;
    }
    if (c == '(') {                                   /* literal string */
        l->pos++;
        uint32_t start = l->pos, cap = 0;
        int nest = 1;
        while (l->pos < l->len && nest) {             /* its raw length bounds the decoded one */
            uint8_t x = l->b[l->pos++];
            if (x == '\\') l->pos++;
            else if (x == '(') nest++;
            else if (x == ')') nest--;
        }
        cap = l->pos - start + 1;
        uint8_t *o = amalloc(d, cap);
        if (!o) return NULL;
        uint32_t n = 0, p = start, end = nest ? l->len : l->pos - 1;
        while (p < end) {
            uint8_t x = l->b[p++];
            if (x != '\\') {
                if (x == '\r') {                          /* any EOL reads as \n */
                    if (p < end && l->b[p] == '\n') p++;
                    x = '\n';
                }
                o[n++] = x;
                continue;
            }
            if (p >= end) break;
            x = l->b[p++];
            switch (x) {
            case 'n': o[n++] = '\n'; break;
            case 'r': o[n++] = '\r'; break;
            case 't': o[n++] = '\t'; break;
            case 'b': o[n++] = '\b'; break;
            case 'f': o[n++] = '\f'; break;
            case '\r': if (p < end && l->b[p] == '\n') p++; break;
            case '\n': break;
            default:
                if (x >= '0' && x <= '7') {
                    int v = x - '0';
                    for (int k = 0; k < 2 && p < end && l->b[p] >= '0' && l->b[p] <= '7'; k++) v = v * 8 + (l->b[p++] - '0');
                    o[n++] = (uint8_t)v;
                } else o[n++] = x;
            }
        }
        pv *v = newv(d, V_STR);
        if (v) v->s = (char *)o, v->n = n;
        return v;
    }
    if (c == '/') {                                   /* name, #xx escapes */
        l->pos++;
        uint32_t start = l->pos;
        while (l->pos < l->len && !is_ws(l->b[l->pos]) && !is_delim(l->b[l->pos])) l->pos++;
        uint8_t *o = amalloc(d, l->pos - start + 1);
        if (!o) return NULL;
        uint32_t n = 0;
        for (uint32_t i = start; i < l->pos; i++) {
            if (l->b[i] == '#' && i + 2 < l->pos && hexv(l->b[i + 1]) >= 0 && hexv(l->b[i + 2]) >= 0) {
                o[n++] = (uint8_t)(hexv(l->b[i + 1]) << 4 | hexv(l->b[i + 2]));
                i += 2;
            } else o[n++] = l->b[i];
        }
        o[n] = 0;
        pv *v = newv(d, V_NAME);
        if (v) v->s = (char *)o, v->n = n;
        return v;
    }
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
        uint32_t start = l->pos;
        long a, g;
        uint32_t p = l->pos;
        /* "n g R" */
        if (read_int(l, &p, &a)) {
            uint32_t q = p;
            while (q < l->len && is_ws(l->b[q])) q++;
            if (q > p && read_int(l, &q, &g)) {
                uint32_t r = q;
                while (r < l->len && is_ws(l->b[r])) r++;
                if (r > q && r < l->len && l->b[r] == 'R' && (r + 1 >= l->len || is_ws(l->b[r + 1]) || is_delim(l->b[r + 1]))) {
                    l->pos = r + 1;
                    pv *v = newv(d, V_REF);
                    if (v) v->num = (double)a;
                    return v;
                }
            }
        }
        l->pos++;
        while (l->pos < l->len && ((l->b[l->pos] >= '0' && l->b[l->pos] <= '9') || l->b[l->pos] == '.' ||
                                   l->b[l->pos] == '-'))
            l->pos++;
        char tmp[40];
        uint32_t n = l->pos - start < sizeof(tmp) - 1 ? l->pos - start : sizeof(tmp) - 1;
        memcpy(tmp, l->b + start, n);
        tmp[n] = 0;
        pv *v = newv(d, V_NUM);
        if (v) v->num = strtod(tmp, NULL);
        return v;
    }
    /* keyword / operator */
    uint32_t start = l->pos;
    while (l->pos < l->len && !is_ws(l->b[l->pos]) && !is_delim(l->b[l->pos])) l->pos++;
    if (l->pos == start) {                            /* a stray delimiter: ) { } */
        l->pos++;
        return mkstr(d, V_OP, l->b + start, 1);
    }
    uint32_t n = l->pos - start;
    if (n == 4 && !memcmp(l->b + start, "true", 4)) { pv *v = newv(d, V_BOOL); if (v) v->num = 1; return v; }
    if (n == 5 && !memcmp(l->b + start, "false", 5)) return newv(d, V_BOOL);
    if (n == 4 && !memcmp(l->b + start, "null", 4)) return newv(d, V_NULL);
    return mkstr(d, V_OP, l->b + start, n);
}

/* ---------------------------------------------------------------- objects */

static pv *obj(pdf_t *d, uint32_t num);

static pv *res(pdf_t *d, pv *v)
{
    for (int i = 0; v && v->t == V_REF && i < 8; i++) v = obj(d, (uint32_t)v->num);
    return v ? v : &NULLV;
}

static pv *dget(pdf_t *d, pv *dict, const char *key)
{
    dict = res(d, dict);
    if (dict->t != V_DICT) return &NULLV;
    for (uint32_t i = 0; i < dict->n; i++)
        if (!strcmp(dict->k[i], key)) return res(d, dict->v[i]);
    return &NULLV;
}

static bool is_name(pv *v, const char *s) { return v->t == V_NAME && !strcmp(v->s, s); }
static double num(pv *v, double def) { return v->t == V_NUM ? v->num : def; }

static bool find_kw(const uint8_t *b, uint32_t len, uint32_t from, const char *kw, uint32_t *at)
{
    size_t kl = strlen(kw);
    for (uint32_t i = from; i + kl <= len; i++)
        if (b[i] == kw[0] && !memcmp(b + i, kw, kl)) {
            *at = i;
            return true;
        }
    return false;
}

/* After a dictionary: "stream" EOL data "endstream" */
static void attach_stream(pdf_t *d, lex_t *l, pv *dict)
{
    uint32_t p = l->pos;
    while (p < l->len && is_ws(l->b[p])) p++;
    if (p + 6 > l->len || memcmp(l->b + p, "stream", 6)) return;
    p += 6;
    if (p < l->len && l->b[p] == '\r') p++;
    if (p < l->len && l->b[p] == '\n') p++;
    double want = num(dget(d, dict, "Length"), -1);
    uint32_t n = 0, e;
    if (want >= 0 && p + want <= l->len) {
        uint32_t q = p + (uint32_t)want;
        while (q < l->len && is_ws(l->b[q]) && q < p + (uint32_t)want + 4) q++;
        if (q + 9 <= l->len && !memcmp(l->b + q, "endstream", 9)) n = (uint32_t)want;
        else if (find_kw(l->b, l->len, p, "endstream", &e)) n = e - p;
    } else if (find_kw(l->b, l->len, p, "endstream", &e))
        n = e - p;
    else n = l->len - p;
    if (!want || n) {
        dict->stm = l->b + p;
        dict->stmlen = n;
    }
}

static pv *obj(pdf_t *d, uint32_t n)
{
    if (n >= d->nobj || !d->idx[n].type) return &NULLV;
    if (d->cache[n]) return d->cache[n];
    if (d->busy[n]) return &NULLV;
    d->busy[n] = 1;
    oref *r = &d->idx[n];
    lex_t l = { d, d->buf, d->len, r->off };
    if (r->type == 2) l.b = d->ostm[r->stm], l.len = d->ostmlen[r->stm];
    pv *v = parse(&l, 0);
    if (!v) v = &NULLV;
    if (v->t == V_DICT && r->type == 1) attach_stream(d, &l, v);
    d->cache[n] = v;
    d->busy[n] = 0;
    return v;
}

/* ---------------------------------------------------------------- filters */

typedef struct {
    uint8_t *p;
    uint32_t n, cap;
    bool bad;
} buf_t;

static void bput(buf_t *b, const void *src, uint32_t n)
{
    if (b->bad) return;
    if (b->n + n > b->cap) {
        uint64_t nc = (uint64_t)(b->cap ? b->cap : 4096);
        while (nc < (uint64_t)b->n + n) nc *= 2;
        if (nc > PDF_MAX_STREAM + 65536u) {
            if ((uint64_t)b->n + n > PDF_MAX_STREAM) { b->bad = true; return; }
            nc = PDF_MAX_STREAM;
        }
        uint8_t *np = realloc(b->p, (size_t)nc);
        if (!np) { b->bad = true; return; }
        b->p = np;
        b->cap = (uint32_t)nc;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static void bputc(buf_t *b, uint8_t c) { bput(b, &c, 1); }

static bool f_flate(const uint8_t *s, uint32_t n, buf_t *o)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 15 + 32) != Z_OK) return false;
    uint8_t tmp[16384];
    z.next_in = (Bytef *)s;
    z.avail_in = n;
    int rc;
    do {
        z.next_out = tmp;
        z.avail_out = sizeof(tmp);
        rc = inflate(&z, Z_NO_FLUSH);
        bput(o, tmp, sizeof(tmp) - z.avail_out);
        if (o->bad) break;
    } while (rc == Z_OK && (z.avail_in || !z.avail_out));
    inflateEnd(&z);
    /* a stream cut short still gives what it held */
    return !o->bad && (rc == Z_STREAM_END || o->n > 0);
}

static bool f_lzw(const uint8_t *s, uint32_t n, buf_t *o, int early)
{
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], first[4096];
    uint8_t stack[4096];
    uint32_t bitpos = 0, next = 258, width = 9;
    int prev = -1;
    for (int i = 0; i < 256; i++) suffix[i] = first[i] = (uint8_t)i;
    while ((bitpos + width) <= n * 8u) {
        uint32_t code = 0;
        for (uint32_t k = 0; k < width; k++, bitpos++) code = code << 1 | ((s[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
        if (code == 256) { next = 258; width = 9; prev = -1; continue; }
        if (code == 257) break;
        uint32_t c = code, sp = 0;
        if (c >= next) {                              /* KwKwK */
            if (prev < 0 || c != next) return o->n > 0;
            stack[sp++] = first[prev];
            c = (uint32_t)prev;
        }
        while (c > 255 && sp < sizeof(stack)) stack[sp++] = suffix[c], c = prefix[c];
        if (sp < sizeof(stack)) stack[sp++] = (uint8_t)c;
        uint8_t fc = (uint8_t)c;
        while (sp) bputc(o, stack[--sp]);
        if (prev >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = fc;
            first[next] = first[prev];
            next++;
        }
        prev = (int)code;
        if (next + early >= (1u << width) && width < 12) width++;
    }
    return !o->bad;
}

static bool f_hex(const uint8_t *s, uint32_t n, buf_t *o)
{
    int hi = -1;
    for (uint32_t i = 0; i < n && s[i] != '>'; i++) {
        int h = hexv(s[i]);
        if (h < 0) continue;
        if (hi < 0) hi = h;
        else bputc(o, (uint8_t)(hi << 4 | h)), hi = -1;
    }
    if (hi >= 0) bputc(o, (uint8_t)(hi << 4));
    return !o->bad;
}

static bool f_a85(const uint8_t *s, uint32_t n, buf_t *o)
{
    uint32_t v = 0, k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '~') break;
        if (is_ws(c)) continue;
        if (c == 'z' && !k) { bput(o, "\0\0\0\0", 4); continue; }
        if (c < '!' || c > 'u') return false;
        v = v * 85 + (c - '!');
        if (++k == 5) {
            uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            bput(o, b, 4);
            v = 0;
            k = 0;
        }
    }
    if (k > 1) {
        for (uint32_t j = k; j < 5; j++) v = v * 85 + 84;
        uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        bput(o, b, k - 1);
    }
    return !o->bad;
}

static bool f_rle(const uint8_t *s, uint32_t n, buf_t *o)
{
    for (uint32_t i = 0; i < n;) {
        uint8_t c = s[i++];
        if (c == 128) break;
        if (c < 128) {
            uint32_t k = c + 1u;
            if (i + k > n) k = n - i;
            bput(o, s + i, k);
            i += k;
        } else if (i < n) {
            for (int k = 0; k < 257 - c; k++) bputc(o, s[i]);
            i++;
        }
    }
    return !o->bad;
}

/* PNG (10-15) and TIFF (2) predictors, in place */
static bool predict(pdf_t *d, pv *parms, buf_t *b)
{
    int pred = (int)num(dget(d, parms, "Predictor"), 1);
    if (pred < 2) return true;
    int colors = (int)num(dget(d, parms, "Colors"), 1), bpc = (int)num(dget(d, parms, "BitsPerComponent"), 8);
    int cols = (int)num(dget(d, parms, "Columns"), 1);
    if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > 1000000) return false;
    uint32_t bpp = (uint32_t)(colors * bpc + 7) / 8, row = (uint32_t)((uint64_t)colors * bpc * cols + 7) / 8;
    if (pred == 2) {
        if (bpc != 8) return true;
        for (uint32_t r = 0; (r + 1) * row <= b->n; r++)
            for (uint32_t i = bpp; i < row; i++) b->p[r * row + i] += b->p[r * row + i - bpp];
        return true;
    }
    uint32_t rows = b->n / (row + 1);
    uint8_t *out = malloc((size_t)rows * row + 1);
    if (!out) return false;
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *in = b->p + r * (row + 1) + 1;
        uint8_t ft = b->p[r * (row + 1)], *cur = out + r * row, *up = r ? cur - row : NULL;
        for (uint32_t i = 0; i < row; i++) {
            uint8_t a = i >= bpp ? cur[i - bpp] : 0, u = up ? up[i] : 0, c = (i >= bpp && up) ? up[i - bpp] : 0;
            uint8_t v = in[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += u; break;
            case 3: v += (uint8_t)((a + u) / 2); break;
            case 4: {
                int p = a + u - c, pa = abs(p - a), pb = abs(p - u), pc = abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? u : c;
                break;
            }
            }
            cur[i] = v;
        }
    }
    free(b->p);
    b->p = out;
    b->n = rows * row;
    b->cap = b->n + 1;
    return true;
}

/* A stream decoded through its filters. *jpeg: it stopped at DCTDecode (the bytes are a JPEG). False
 * for filters it can't undo. The result is malloc'd (o->p). */
static bool decode(pdf_t *d, pv *dict, buf_t *o, bool *jpeg)
{
    memset(o, 0, sizeof(*o));
    if (jpeg) *jpeg = false;
    if (!dict->stm) return false;
    pv *f = dget(d, dict, "Filter"), *dp = dget(d, dict, "DecodeParms");
    uint32_t nf = f->t == V_ARR ? f->n : f->t == V_NAME ? 1 : 0;
    buf_t cur = { 0 };
    bput(&cur, dict->stm, dict->stmlen);
    for (uint32_t i = 0; i < nf && !cur.bad; i++) {
        pv *fi = f->t == V_ARR ? res(d, f->v[i]) : f, *pi = dp->t == V_ARR ? (i < dp->n ? res(d, dp->v[i]) : &NULLV) : dp;
        if (fi->t != V_NAME) break;
        const char *fn = fi->s;
        buf_t nx = { 0 };
        bool ok;
        if (!strcmp(fn, "FlateDecode") || !strcmp(fn, "Fl")) ok = f_flate(cur.p, cur.n, &nx) && predict(d, pi, &nx);
        else if (!strcmp(fn, "LZWDecode") || !strcmp(fn, "LZW"))
            ok = f_lzw(cur.p, cur.n, &nx, (int)num(dget(d, pi, "EarlyChange"), 1)) && predict(d, pi, &nx);
        else if (!strcmp(fn, "ASCIIHexDecode") || !strcmp(fn, "AHx")) ok = f_hex(cur.p, cur.n, &nx);
        else if (!strcmp(fn, "ASCII85Decode") || !strcmp(fn, "A85")) ok = f_a85(cur.p, cur.n, &nx);
        else if (!strcmp(fn, "RunLengthDecode") || !strcmp(fn, "RL")) ok = f_rle(cur.p, cur.n, &nx);
        else if (!strcmp(fn, "DCTDecode") || !strcmp(fn, "DCT")) {
            if (jpeg) *jpeg = true;
            *o = cur;
            return i + 1 == nf;
        } else ok = false;
        free(cur.p);
        if (!ok) {
            free(nx.p);
            return false;
        }
        cur = nx;
    }
    if (cur.bad) {
        free(cur.p);
        return false;
    }
    *o = cur;
    return true;
}

/* ---------------------------------------------------------------- indexing */

static bool index_objects(pdf_t *d)
{
    uint32_t maxn = 0;
    const uint8_t *b = d->buf;
    /* pass 1: the highest object number, so the index can be sized */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i + 5 < d->len; i++) {
            if (b[i] < '0' || b[i] > '9' || (i && !is_ws(b[i - 1]) && !is_delim(b[i - 1]))) continue;
            uint32_t p = i;
            long on = 0, gen = 0;
            lex_t l = { d, b, d->len, 0 };
            if (!read_int(&l, &p, &on) || p >= d->len || !is_ws(b[p])) continue;
            while (p < d->len && is_ws(b[p])) p++;
            if (!read_int(&l, &p, &gen) || p >= d->len || !is_ws(b[p])) continue;
            while (p < d->len && is_ws(b[p])) p++;
            if (p + 3 > d->len || memcmp(b + p, "obj", 3) || (p + 3 < d->len && !is_ws(b[p + 3]) && !is_delim(b[p + 3])))
                continue;
            if (on <= 0 || on >= PDF_MAX_OBJ) continue;
            if (!pass) {
                if ((uint32_t)on > maxn) maxn = (uint32_t)on;
            } else {
                d->idx[on].type = 1;
                d->idx[on].off = p + 3;
            }
            i = p + 2;
        }
        if (!pass) {
            d->nobj = maxn + 1;
            d->idx = calloc(d->nobj, sizeof(oref));
            d->cache = calloc(d->nobj, sizeof(pv *));
            d->busy = calloc(d->nobj, 1);
            if (!d->idx || !d->cache || !d->busy) return false;
        }
    }

    /* object streams: their objects fill the slots nothing else defines */
    uint32_t n0 = d->nobj;
    for (uint32_t i = 1; i < n0; i++) {
        if (d->idx[i].type != 1) continue;
        pv *o = obj(d, i);
        if (o->t != V_DICT || !o->stm || !is_name(dget(d, o, "Type"), "ObjStm")) continue;
        buf_t s;
        if (!decode(d, o, &s, NULL)) {
            free(s.p);
            continue;
        }
        uint32_t cnt = (uint32_t)num(dget(d, o, "N"), 0), first = (uint32_t)num(dget(d, o, "First"), 0);
        uint8_t **ns = realloc(d->ostm, (d->nostm + 1) * sizeof(uint8_t *));
        uint32_t *nl = realloc(d->ostmlen, (d->nostm + 1) * sizeof(uint32_t));
        if (ns) d->ostm = ns;
        if (nl) d->ostmlen = nl;
        if (!ns || !nl || first >= s.n) {
            free(s.p);
            continue;
        }
        uint32_t si = d->nostm++;
        d->ostm[si] = s.p;
        d->ostmlen[si] = s.n;
        lex_t l = { d, s.p, first, 0 };
        for (uint32_t k = 0; k < cnt && k < 1000000; k++) {
            long on, off;
            skip_ws(&l);
            if (!read_int(&l, &l.pos, &on)) break;
            skip_ws(&l);
            if (!read_int(&l, &l.pos, &off)) break;
            if (on > 0 && (uint32_t)on < d->nobj && !d->idx[on].type && first + (uint64_t)off < s.n) {
                d->idx[on].type = 2;
                d->idx[on].off = first + (uint32_t)off;
                d->idx[on].stm = si;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------- text */

typedef struct {
    uint32_t code, cp[3];
    uint8_t n;
} umap_t;
typedef struct {
    uint32_t lo, hi, base;
} urange_t;

typedef struct {
    pv *dict;
    bool two;                   /* 2-byte codes (Type0) */
    bool unmapped;              /* Type0 without ToUnicode */
    uint32_t simple[256];       /* 1-byte codes -> Unicode */
    umap_t *map;
    uint32_t nmap;
    urange_t *rng;
    uint32_t nrng;
} font_t;

static const uint16_t win1252[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static const uint16_t macroman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

static const struct { const char *n; uint16_t u; } glyphs[] = {
    { "space", ' ' }, { "exclam", '!' }, { "quotedbl", '"' }, { "numbersign", '#' }, { "dollar", '$' },
    { "percent", '%' }, { "ampersand", '&' }, { "quoteright", 0x2019 }, { "quotesingle", '\'' },
    { "parenleft", '(' }, { "parenright", ')' }, { "asterisk", '*' }, { "plus", '+' }, { "comma", ',' },
    { "hyphen", '-' }, { "minus", '-' }, { "period", '.' }, { "slash", '/' }, { "zero", '0' }, { "one", '1' },
    { "two", '2' }, { "three", '3' }, { "four", '4' }, { "five", '5' }, { "six", '6' }, { "seven", '7' },
    { "eight", '8' }, { "nine", '9' }, { "colon", ':' }, { "semicolon", ';' }, { "less", '<' }, { "equal", '=' },
    { "greater", '>' }, { "question", '?' }, { "at", '@' }, { "bracketleft", '[' }, { "backslash", '\\' },
    { "bracketright", ']' }, { "asciicircum", '^' }, { "underscore", '_' }, { "quoteleft", 0x2018 },
    { "grave", '`' }, { "braceleft", '{' }, { "bar", '|' }, { "braceright", '}' }, { "asciitilde", '~' },
    { "bullet", 0x2022 }, { "endash", 0x2013 }, { "emdash", 0x2014 }, { "quotedblleft", 0x201C },
    { "quotedblright", 0x201D }, { "quotesinglbase", 0x201A }, { "quotedblbase", 0x201E }, { "ellipsis", 0x2026 },
    { "trademark", 0x2122 }, { "copyright", 0xA9 }, { "registered", 0xAE }, { "degree", 0xB0 }, { "fi", 0xFB01 },
    { "fl", 0xFB02 }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 }, { "section", 0xA7 }, { "paragraph", 0xB6 },
    { "periodcentered", 0xB7 }, { "multiply", 0xD7 }, { "divide", 0xF7 }, { "nbspace", 0xA0 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 }, { "ccedilla", 0xE7 }, { "udieresis", 0xFC },
    { "odieresis", 0xF6 }, { "adieresis", 0xE4 }, { "germandbls", 0xDF }, { "Eacute", 0xC9 }, { "ntilde", 0xF1 },
};

static uint32_t glyph_cp(const char *n)
{
    if (n[0] && !n[1]) return (uint8_t)n[0];
    if (!strncmp(n, "uni", 3) && strlen(n) >= 7) return (uint32_t)strtoul(n + 3, NULL, 16) & 0xFFFF;
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (!strcmp(glyphs[i].n, n)) return glyphs[i].u;
    return 0;
}

static uint32_t utf16_first(const uint8_t *s, uint32_t n, uint32_t *cps, uint8_t *count)
{
    uint8_t k = 0;
    for (uint32_t i = 0; i + 1 < n && k < 3; i += 2) {
        uint32_t u = (uint32_t)s[i] << 8 | s[i + 1];
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t lo = (uint32_t)s[i + 2] << 8 | s[i + 3];
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        cps[k++] = u;
    }
    *count = k;
    return k ? cps[0] : 0;
}

static uint32_t code_of(const pv *s)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < s->n && i < 4; i++) c = c << 8 | (uint8_t)s->s[i];
    return c;
}

static void parse_cmap(pdf_t *d, font_t *f, pv *stm)
{
    buf_t b;
    if (!decode(d, stm, &b, NULL)) {
        free(b.p);
        return;
    }
    lex_t l = { d, b.p, b.n, 0 };
    uint32_t capm = 0;
    for (;;) {
        pv *t = parse(&l, 0);
        if (!t) break;
        if (t->t == V_OP && !strcmp(t->s, "beginbfchar")) {
            for (;;) {
                pv *src = parse(&l, 0);
                if (!src || src->t != V_STR) break;
                pv *dst = parse(&l, 0);
                if (!dst) break;
                if (dst->t != V_STR) continue;
                if (f->nmap == capm) {
                    capm = capm ? capm * 2 : 256;
                    umap_t *nm = realloc(f->map, capm * sizeof(umap_t));
                    if (!nm) break;
                    f->map = nm;
                }
                umap_t *m = &f->map[f->nmap++];
                m->code = code_of(src);
                utf16_first((const uint8_t *)dst->s, dst->n, m->cp, &m->n);
            }
        } else if (t->t == V_OP && !strcmp(t->s, "beginbfrange")) {
            for (;;) {
                pv *lo = parse(&l, 0);
                if (!lo || lo->t != V_STR) break;
                pv *hi = parse(&l, 0), *dst = parse(&l, 0);
                if (!hi || !dst) break;
                uint32_t a = code_of(lo), z = code_of(hi);
                if (z < a || z - a > 65535) continue;
                if (dst->t == V_STR) {
                    if (f->nrng % 64 == 0) {
                        urange_t *nr = realloc(f->rng, (f->nrng + 64) * sizeof(urange_t));
                        if (!nr) break;
                        f->rng = nr;
                    }
                    uint32_t cps[3];
                    uint8_t k;
                    f->rng[f->nrng++] = (urange_t){ a, z, utf16_first((const uint8_t *)dst->s, dst->n, cps, &k) };
                } else if (dst->t == V_ARR) {
                    for (uint32_t i = 0; i < dst->n && a + i <= z; i++) {
                        pv *e = dst->v[i];
                        if (e->t != V_STR) continue;
                        if (f->nmap == capm) {
                            capm = capm ? capm * 2 : 256;
                            umap_t *nm = realloc(f->map, capm * sizeof(umap_t));
                            if (!nm) break;
                            f->map = nm;
                        }
                        umap_t *m = &f->map[f->nmap++];
                        m->code = a + i;
                        utf16_first((const uint8_t *)e->s, e->n, m->cp, &m->n);
                    }
                }
            }
        }
    }
    free(b.p);
}

static void font_init(pdf_t *d, font_t *f, pv *fd)
{
    memset(f, 0, sizeof(*f));
    f->dict = fd;
    pv *sub = dget(d, fd, "Subtype");
    f->two = is_name(sub, "Type0");
    pv *enc = dget(d, fd, "Encoding");
    pv *benc = enc->t == V_DICT ? dget(d, enc, "BaseEncoding") : enc;
    bool mac = is_name(benc, "MacRomanEncoding"), win = is_name(benc, "WinAnsiEncoding");
    for (int i = 0; i < 256; i++)
        f->simple[i] = i >= 0x80 && mac ? macroman[i - 0x80] : i >= 0x80 && i < 0xA0 && win ? win1252[i - 0x80] : (uint32_t)i;
    if (enc->t == V_DICT) {
        pv *diff = dget(d, enc, "Differences");
        uint32_t code = 0;
        for (uint32_t i = 0; diff->t == V_ARR && i < diff->n; i++) {
            pv *e = res(d, diff->v[i]);
            if (e->t == V_NUM) code = (uint32_t)e->num;
            else if (e->t == V_NAME && code < 256) {
                uint32_t u = glyph_cp(e->s);
                if (u) f->simple[code] = u;
                code++;
            }
        }
    }
    pv *tu = dget(d, fd, "ToUnicode");
    if (tu->t == V_DICT && tu->stm) parse_cmap(d, f, tu);
    if (f->two && !f->nmap && !f->nrng) f->unmapped = true;
}

static void font_free(font_t *f)
{
    free(f->map);
    free(f->rng);
}

static void put_utf8(buf_t *o, uint32_t u)
{
    if (!u || u == 0xFEFF) return;
    if (u < 0x80) bputc(o, (uint8_t)u);
    else if (u < 0x800) bputc(o, (uint8_t)(0xC0 | u >> 6)), bputc(o, (uint8_t)(0x80 | (u & 63)));
    else if (u < 0x10000)
        bputc(o, (uint8_t)(0xE0 | u >> 12)), bputc(o, (uint8_t)(0x80 | ((u >> 6) & 63))), bputc(o, (uint8_t)(0x80 | (u & 63)));
    else
        bputc(o, (uint8_t)(0xF0 | u >> 18)), bputc(o, (uint8_t)(0x80 | ((u >> 12) & 63))),
            bputc(o, (uint8_t)(0x80 | ((u >> 6) & 63))), bputc(o, (uint8_t)(0x80 | (u & 63)));
}

static bool map_code(const font_t *f, uint32_t c, buf_t *o)
{
    for (uint32_t i = 0; i < f->nmap; i++)
        if (f->map[i].code == c) {
            for (uint8_t k = 0; k < f->map[i].n; k++) put_utf8(o, f->map[i].cp[k]);
            return true;
        }
    for (uint32_t i = 0; i < f->nrng; i++)
        if (c >= f->rng[i].lo && c <= f->rng[i].hi) {
            put_utf8(o, f->rng[i].base + (c - f->rng[i].lo));
            return true;
        }
    return false;
}

typedef struct {
    pdf_t *d;
    buf_t *o;
    font_t *fonts;
    uint32_t nfonts, capfonts;
    bool unmapped_note;
    int images;                 /* images drawn so far on this page */
    bool have_y;
    double last_y;              /* where the last text went, on the page */
} txt_t;

static font_t *font_for(txt_t *t, pv *fd)
{
    for (uint32_t i = 0; i < t->nfonts; i++)
        if (t->fonts[i].dict == fd) return &t->fonts[i];
    if (t->nfonts == t->capfonts) {
        uint32_t nc = t->capfonts ? t->capfonts * 2 : 16;
        font_t *nf = realloc(t->fonts, nc * sizeof(font_t));
        if (!nf) return NULL;
        t->fonts = nf;
        t->capfonts = nc;
    }
    font_t *f = &t->fonts[t->nfonts++];
    font_init(t->d, f, fd);
    return f;
}

static char last_char(buf_t *o) { return o->n ? (char)o->p[o->n - 1] : '\n'; }
static void soft(buf_t *o, char c)
{
    char l = last_char(o);
    if (c == ' ' && (l == ' ' || l == '\n')) return;
    if (c == '\n' && l == '\n') return;
    bputc(o, (uint8_t)c);
}

static void show(txt_t *t, font_t *f, pv *s)
{
    if (s->t != V_STR) return;
    if (!f) {
        for (uint32_t i = 0; i < s->n; i++) put_utf8(t->o, (uint8_t)s->s[i]);
        return;
    }
    if (f->unmapped) {
        if (!t->unmapped_note) bput(t->o, "[text in a font without a Unicode map]", 38), t->unmapped_note = true;
        return;
    }
    const uint8_t *b = (const uint8_t *)s->s;
    for (uint32_t i = 0; i < s->n;) {
        uint32_t c;
        if (f->two && i + 1 < s->n) c = (uint32_t)b[i] << 8 | b[i + 1], i += 2;
        else c = b[i++];
        if (map_code(f, c, t->o)) continue;
        if (!f->two) put_utf8(t->o, f->simple[c & 0xFF]);
    }
}

static void run_content(txt_t *t, const uint8_t *b, uint32_t n, pv *resources, const double *ctm0, int depth);

/* m = a x b for PDF's [a b c d e f] matrices */
static void mmul(double *m, const double *a, const double *b)
{
    double r[6] = { a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
                    a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5] };
    memcpy(m, r, sizeof(r));
}

static void do_xobject(txt_t *t, pv *resources, pv *name, const double *ctm, int depth)
{
    pdf_t *d = t->d;
    if (name->t != V_NAME || depth > 4) return;
    pv *x = dget(d, dget(d, resources, "XObject"), name->s);
    pv *st = dget(d, x, "Subtype");
    if (is_name(st, "Form") && x->stm) {
        double m[6] = { 1, 0, 0, 1, 0, 0 }, c[6];
        pv *mx = dget(d, x, "Matrix");
        for (uint32_t i = 0; mx->t == V_ARR && i < 6 && i < mx->n; i++) m[i] = num(res(d, mx->v[i]), m[i]);
        mmul(c, m, ctm);
        buf_t cs;
        if (decode(d, x, &cs, NULL)) {
            pv *r = dget(d, x, "Resources");
            run_content(t, cs.p, cs.n, r->t == V_DICT ? r : resources, c, depth + 1);
        }
        free(cs.p);
    } else if (is_name(st, "Image"))
        t->images++;
}

/* Text in drawing order; a line break wherever the next piece lands on another line of the page
 * (text matrix x graphics matrix), so it works whichever of the two a generator moves. */
static void run_content(txt_t *t, const uint8_t *b, uint32_t n, pv *resources, const double *ctm0, int depth)
{
    pdf_t *d = t->d;
    lex_t l = { d, b, n, 0 };
    pv *ops[16];
    int nops = 0, sp = 0;
    font_t *font = NULL;
    double stack[32][6], ctm[6], tm[6] = { 1, 0, 0, 1, 0, 0 }, tlm[6] = { 1, 0, 0, 1, 0, 0 }, tfs = 1, tl = 0;
    memcpy(ctm, ctm0, sizeof(ctm));
    for (;;) {
        pv *v = parse(&l, 0);
        if (!v) break;
        if (v->t != V_OP) {
            if (nops == 16) memmove(ops, ops + 1, 15 * sizeof(pv *)), nops--;
            ops[nops++] = v;
            continue;
        }
        const char *op = v->s;
        bool shows = !strcmp(op, "Tj") || !strcmp(op, "TJ") || !strcmp(op, "'") || !strcmp(op, "\"");
        if (!strcmp(op, "q") && sp < 32) memcpy(stack[sp++], ctm, sizeof(ctm));
        else if (!strcmp(op, "Q") && sp > 0) memcpy(ctm, stack[--sp], sizeof(ctm));
        else if (!strcmp(op, "cm") && nops >= 6) {
            double m[6];
            for (int i = 0; i < 6; i++) m[i] = num(ops[nops - 6 + i], 0);
            mmul(ctm, m, ctm);
        } else if (!strcmp(op, "BT")) {
            double id[6] = { 1, 0, 0, 1, 0, 0 };
            memcpy(tm, id, sizeof(tm));
            memcpy(tlm, id, sizeof(tlm));
        } else if (!strcmp(op, "Tf") && nops >= 2 && ops[nops - 2]->t == V_NAME) {
            pv *fd = dget(d, dget(d, resources, "Font"), ops[nops - 2]->s);
            font = fd->t == V_DICT ? font_for(t, fd) : NULL;
            tfs = num(ops[nops - 1], 1);
        } else if (!strcmp(op, "TL") && nops >= 1) tl = num(ops[nops - 1], 0);
        else if (!strcmp(op, "Tm") && nops >= 6) {
            for (int i = 0; i < 6; i++) tlm[i] = num(ops[nops - 6 + i], i == 0 || i == 3);
            memcpy(tm, tlm, sizeof(tm));
        } else if ((!strcmp(op, "Td") || !strcmp(op, "TD") || !strcmp(op, "T*")) ) {
            double tx = 0, ty = -tl;
            if (op[1] != '*' && nops >= 2) tx = num(ops[nops - 2], 0), ty = num(ops[nops - 1], 0);
            if (op[1] == 'D') tl = -ty;
            double m[6] = { 1, 0, 0, 1, tx, ty };
            mmul(tlm, m, tlm);
            memcpy(tm, tlm, sizeof(tm));
        } else if (!strcmp(op, "Do") && nops >= 1) do_xobject(t, resources, ops[nops - 1], ctm, depth);
        else if (!strcmp(op, "BI")) {                  /* inline image: skip to EI */
            uint32_t e = l.pos;
            while (find_kw(b, n, e, "EI", &e)) {
                if (e > 0 && is_ws(b[e - 1]) && (e + 2 >= n || is_ws(b[e + 2]))) break;
                e += 2;
            }
            l.pos = e + 2 <= n ? e + 2 : n;
        }
        if (shows && nops >= 1) {
            if (op[0] == '\'' || op[0] == '"') {           /* next line first */
                double m[6] = { 1, 0, 0, 1, 0, -tl };
                mmul(tlm, m, tlm);
                memcpy(tm, tlm, sizeof(tm));
            }
            double dev[6];
            mmul(dev, tm, ctm);
            double y = dev[5], hy = tfs * dev[3], hx = tfs * dev[2];
            double h = hx * hx + hy * hy > 0 ? __builtin_sqrt(hx * hx + hy * hy) : 1;
            double gap = h * 0.5 > 0.5 ? h * 0.5 : 0.5;
            if (t->have_y && (y - t->last_y > gap || t->last_y - y > gap)) soft(t->o, '\n');
            t->last_y = y;
            t->have_y = true;
            pv *s = ops[nops - 1];
            if (op[1] == 'J' && s->t == V_ARR) {
                for (uint32_t i = 0; i < s->n; i++) {
                    pv *e = s->v[i];
                    if (e->t == V_STR) show(t, font, e);
                    else if (e->t == V_NUM && e->num < -250) soft(t->o, ' ');
                }
            } else show(t, font, s);
        }
        nops = 0;
        if (t->o->bad) break;
    }
}

/* ---------------------------------------------------------------- images */

static void be32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static void png_chunk(buf_t *o, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t h[8];
    be32w(h, n);
    memcpy(h + 4, type, 4);
    bput(o, h, 8);
    bput(o, data, n);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, n);              /* crc32(c, NULL, 0) would return 0, not c */
    be32w(h, (uint32_t)crc);
    bput(o, h, 4);
}

/* 8-bit gray (ch 1) or RGB (ch 3) pixels as a PNG */
static bool png_write(buf_t *o, const uint8_t *px, uint32_t w, uint32_t h, int ch)
{
    uint64_t raw = (uint64_t)(w * ch + 1) * h;
    uint8_t *r = malloc((size_t)raw);
    if (!r) return false;
    for (uint32_t y = 0; y < h; y++) {
        r[y * (w * ch + 1)] = 0;
        memcpy(r + y * (w * ch + 1) + 1, px + (size_t)y * w * ch, (size_t)w * ch);
    }
    uLongf zl = compressBound((uLong)raw);
    uint8_t *z = malloc(zl);
    if (!z || compress2(z, &zl, r, (uLong)raw, 6) != Z_OK) {
        free(r);
        free(z);
        return false;
    }
    free(r);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    bput(o, sig, 8);
    uint8_t ih[13];
    be32w(ih, w);
    be32w(ih + 4, h);
    ih[8] = 8;
    ih[9] = ch == 3 ? 2 : 0;
    ih[10] = ih[11] = ih[12] = 0;
    png_chunk(o, "IHDR", ih, 13);
    png_chunk(o, "IDAT", z, (uint32_t)zl);
    png_chunk(o, "IEND", NULL, 0);
    free(z);
    return !o->bad;
}

static uint32_t sample(const uint8_t *row, uint32_t i, int bpc)
{
    if (bpc == 8) return row[i];
    if (bpc == 16) return row[i * 2];
    uint32_t bit = i * bpc;
    return (row[bit >> 3] >> (8 - bpc - (bit & 7))) & ((1u << bpc) - 1);
}

/* The color space's component count and, for Indexed, its base count and lookup table. */
static int cs_info(pdf_t *d, pv *cs, int *base, buf_t *lut, uint32_t *hival)
{
    cs = res(d, cs);
    if (cs->t == V_NAME) {
        if (!strcmp(cs->s, "DeviceGray") || !strcmp(cs->s, "G") || !strcmp(cs->s, "CalGray")) return 1;
        if (!strcmp(cs->s, "DeviceRGB") || !strcmp(cs->s, "RGB") || !strcmp(cs->s, "CalRGB")) return 3;
        if (!strcmp(cs->s, "DeviceCMYK") || !strcmp(cs->s, "CMYK")) return 4;
        return 0;
    }
    if (cs->t != V_ARR || !cs->n) return 0;
    pv *fam = res(d, cs->v[0]);
    if (is_name(fam, "ICCBased") && cs->n > 1) {
        int nn = (int)num(dget(d, cs->v[1], "N"), 0);
        return nn == 1 || nn == 3 || nn == 4 ? nn : 0;
    }
    if (is_name(fam, "CalRGB") || is_name(fam, "Lab")) return 3;
    if (is_name(fam, "CalGray")) return 1;
    if ((is_name(fam, "Indexed") || is_name(fam, "I")) && cs->n >= 4) {
        int b2, dummy;
        buf_t inner = { 0 };
        uint32_t hv;
        b2 = cs_info(d, cs->v[1], &dummy, &inner, &hv);
        free(inner.p);
        if (!b2 || b2 == -1) return 0;
        *base = b2;
        *hival = (uint32_t)num(res(d, cs->v[2]), 0);
        pv *lk = res(d, cs->v[3]);
        if (lk->t == V_STR) bput(lut, lk->s, lk->n);
        else if (lk->t == V_DICT && lk->stm) {
            buf_t t;
            if (decode(d, lk, &t, NULL)) bput(lut, t.p, t.n);
            free(t.p);
        }
        return -1;                                     /* indexed */
    }
    return 0;
}

static void to_rgb(const uint32_t *c, int nc, uint8_t *o)
{
    if (nc == 1) o[0] = o[1] = o[2] = (uint8_t)c[0];
    else if (nc == 3) o[0] = (uint8_t)c[0], o[1] = (uint8_t)c[1], o[2] = (uint8_t)c[2];
    else {
        uint32_t k = c[3];
        for (int i = 0; i < 3; i++) o[i] = (uint8_t)((255 - c[i]) * (255 - k) / 255);
    }
}

/* An image XObject as a file: the JPEG as stored, or a PNG. *ext: "jpg" / "png". */
static bool image_file(pdf_t *d, pv *x, buf_t *out, const char **ext)
{
    memset(out, 0, sizeof(*out));
    bool jpeg;
    buf_t raw;
    if (!decode(d, x, &raw, &jpeg)) {
        free(raw.p);
        return false;
    }
    if (jpeg) {
        *ext = "jpg";
        *out = raw;
        return raw.n > 0;
    }
    uint32_t w = (uint32_t)num(dget(d, x, "Width"), 0), h = (uint32_t)num(dget(d, x, "Height"), 0);
    bool mask = dget(d, x, "ImageMask")->t == V_BOOL && dget(d, x, "ImageMask")->num;
    int bpc = mask ? 1 : (int)num(dget(d, x, "BitsPerComponent"), 8);
    int base = 0;
    uint32_t hival = 0;
    buf_t lut = { 0 };
    int nc = mask ? 1 : cs_info(d, dget(d, x, "ColorSpace"), &base, &lut, &hival);
    bool ok = false;
    if (w && h && w <= 8192 && h <= 8192 && (uint64_t)w * h * 3 <= 24u * 1024 * 1024 && nc &&
        (bpc == 1 || bpc == 2 || bpc == 4 || bpc == 8 || bpc == 16)) {
        int comps = nc == -1 ? 1 : nc;
        uint32_t row = (uint32_t)(((uint64_t)w * comps * bpc + 7) / 8);
        if ((uint64_t)row * h <= raw.n) {
            int och = (nc == 1 || (nc == -1 && base == 1)) ? 1 : 3;
            uint8_t *px = malloc((size_t)w * h * och);
            if (px) {
                uint32_t maxv = (1u << (bpc > 8 ? 8 : bpc)) - 1;
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *r = raw.p + (size_t)y * row;
                    for (uint32_t xx = 0; xx < w; xx++) {
                        uint8_t *o = px + ((size_t)y * w + xx) * och;
                        if (nc == -1) {
                            uint32_t idx = sample(r, xx, bpc), c[4] = { 0 };
                            if (idx > hival) idx = hival;
                            for (int k = 0; k < base; k++)
                                c[k] = (uint64_t)idx * base + k < lut.n ? lut.p[idx * base + k] : 0;
                            if (och == 1) o[0] = (uint8_t)c[0];
                            else to_rgb(c, base, o);
                        } else {
                            uint32_t c[4];
                            for (int k = 0; k < comps; k++) c[k] = sample(r, xx * comps + k, bpc) * 255 / maxv;
                            if (mask) c[0] = 255 - c[0];
                            if (och == 1) o[0] = (uint8_t)c[0];
                            else to_rgb(c, comps, o);
                        }
                    }
                }
                ok = png_write(out, px, w, h, och);
                free(px);
                *ext = "png";
            }
        }
    }
    free(lut.p);
    free(raw.p);
    if (!ok) free(out->p), memset(out, 0, sizeof(*out));
    return ok;
}

/* ---------------------------------------------------------------- the archive */

typedef struct {
    archive_t *a;
    buf_t blob;
    bool full;
} out_t;

static void add_file(out_t *o, const char *name, const uint8_t *p, uint32_t n)
{
    if (o->full || (uint64_t)o->blob.n + n > PDF_MAX_OUT) {
        o->full = true;
        return;
    }
    uint32_t at = o->blob.n;
    if (n) {
        /* bput caps at PDF_MAX_STREAM; the blob grows by hand */
        if (at + n > o->blob.cap) {
            uint64_t nc = o->blob.cap ? o->blob.cap : 65536;
            while (nc < (uint64_t)at + n) nc *= 2;
            if (nc > PDF_MAX_OUT) nc = PDF_MAX_OUT;
            uint8_t *np = realloc(o->blob.p, (size_t)nc);
            if (!np) {
                o->full = true;
                return;
            }
            o->blob.p = np;
            o->blob.cap = (uint32_t)nc;
        }
        memcpy(o->blob.p + at, p, n);
        o->blob.n += n;
    }
    tree_add_path(&o->a->tree, name, false, n, at, 0, 0);
}

static int32_t mem_read(archive_t *a, const tnode *nd, uint64_t off, void *buf, uint32_t len)
{
    if (off >= nd->size) return 0;
    if (len > nd->size - off) len = (uint32_t)(nd->size - off);
    return a->src.read(a->src.ctx, nd->a + off, buf, len);
}

/* pages in order, through the page tree; inherited Resources are passed down */
typedef struct {
    pv **page, **resources;
    uint32_t n, cap;
} pages_t;

static void walk(pdf_t *d, pv *node, pv *inh, pages_t *p, int depth, uint8_t *seen)
{
    if (depth > 64 || p->n >= PDF_MAX_PAGES || node->t != V_DICT) return;
    pv *r = dget(d, node, "Resources");
    if (r->t == V_DICT) inh = r;
    pv *kids = dget(d, node, "Kids");
    if (kids->t == V_ARR && !is_name(dget(d, node, "Type"), "Page")) {
        for (uint32_t i = 0; i < kids->n; i++) {
            pv *k = kids->v[i];
            if (k->t == V_REF) {
                uint32_t on = (uint32_t)k->num;
                if (on < d->nobj) {
                    if (seen[on]) continue;
                    seen[on] = 1;
                }
            }
            walk(d, res(d, k), inh, p, depth + 1, seen);
        }
        return;
    }
    if (p->n == p->cap) {
        uint32_t nc = p->cap ? p->cap * 2 : 64;
        pv **np = realloc(p->page, nc * sizeof(pv *)), **nr = realloc(p->resources, nc * sizeof(pv *));
        if (np) p->page = np;
        if (nr) p->resources = nr;
        if (!np || !nr) return;
        p->cap = nc;
    }
    p->page[p->n] = node;
    p->resources[p->n++] = inh;
}

/* A text string (title, author): UTF-16BE with a BOM, else PDFDocEncoding read as Latin-1. */
static void text_string(buf_t *o, pv *s)
{
    if (s->t != V_STR) return;
    const uint8_t *b = (const uint8_t *)s->s;
    if (s->n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        uint32_t cps[3];
        uint8_t k;
        for (uint32_t i = 2; i + 1 < s->n; i += 2) {
            utf16_first(b + i, s->n - i, cps, &k);
            if (k) put_utf8(o, cps[0]);
            if (cps[0] >= 0x10000) i += 2;
        }
    } else
        for (uint32_t i = 0; i < s->n; i++) put_utf8(o, b[i]);
}

static void images_in(pdf_t *d, out_t *o, pv *resources, unsigned page, unsigned *count, uint8_t *done, int depth)
{
    pv *xo = dget(d, resources, "XObject");
    if (xo->t != V_DICT || depth > 4) return;
    for (uint32_t i = 0; i < xo->n && !o->full; i++) {
        pv *ref = xo->v[i];
        uint32_t on = ref->t == V_REF ? (uint32_t)ref->num : 0;
        if (on && on < d->nobj) {
            if (done[on]) continue;
            done[on] = 1;
        }
        pv *x = res(d, ref);
        pv *st = dget(d, x, "Subtype");
        if (is_name(st, "Form")) {
            pv *r = dget(d, x, "Resources");
            if (r->t == V_DICT) images_in(d, o, r, page, count, done, depth + 1);
            continue;
        }
        if (!is_name(st, "Image") || !x->stm) continue;
        buf_t f;
        const char *ext = "bin";
        if (!image_file(d, x, &f, &ext)) continue;
        char nm[64];
        snprintf(nm, sizeof(nm), "page_%03u_img%02u.%s", page, ++*count, ext);
        add_file(o, nm, f.p, f.n);
        free(f.p);
    }
}

bool arc_pdf_probe(const src_t *f, const char *name)
{
    (void)name;
    uint8_t h[1024];
    uint32_t n = f->size < sizeof(h) ? (uint32_t)f->size : (uint32_t)sizeof(h);
    if (n < 8 || !src_read_full(f, 0, h, n)) return false;
    for (uint32_t i = 0; i + 5 <= n; i++)
        if (!memcmp(h + i, "%PDF-", 5)) return true;
    return false;
}

static void pdf_free(pdf_t *d)
{
    while (d->arena) {
        chunk *c = d->arena->next;
        free(d->arena);
        d->arena = c;
    }
    for (uint32_t i = 0; i < d->nostm; i++) free(d->ostm[i]);
    free(d->ostm);
    free(d->ostmlen);
    free(d->idx);
    free(d->cache);
    free(d->busy);
    free(d->buf);
}

archive_t *arc_pdf_open(const src_t *f, const char *name)
{
    (void)name;
    if (f->size > PDF_MAX_FILE) {
        g_archive_err = "This PDF is too big to open";
        return NULL;
    }
    pdf_t d;
    memset(&d, 0, sizeof(d));
    d.len = (uint32_t)f->size;
    d.buf = malloc(d.len ? d.len : 1);
    if (!d.buf || !src_read_full(f, 0, d.buf, d.len)) {
        g_archive_err = "Read error";
        pdf_free(&d);
        return NULL;
    }
    if (!index_objects(&d)) {
        g_archive_err = "Out of memory";
        pdf_free(&d);
        return NULL;
    }

    /* the catalog and the trailer's /Info and /Encrypt, from a trailer or an xref stream */
    pv *catalog = &NULLV, *info = &NULLV;
    bool encrypted = false;
    for (uint32_t i = 1; i < d.nobj; i++) {
        if (!d.idx[i].type) continue;
        pv *o = obj(&d, i);
        if (o->t != V_DICT) continue;
        pv *t = dget(&d, o, "Type");
        if (is_name(t, "Catalog") && dget(&d, o, "Pages")->t == V_DICT) catalog = o;
        if (is_name(t, "XRef")) {
            if (dget(&d, o, "Encrypt")->t != V_NULL) encrypted = true;
            if (dget(&d, o, "Info")->t == V_DICT) info = dget(&d, o, "Info");
        }
    }
    for (uint32_t p = 0; find_kw(d.buf, d.len, p, "trailer", &p); p += 7) {
        lex_t l = { &d, d.buf, d.len, p + 7 };
        pv *t = parse(&l, 0);
        if (!t || t->t != V_DICT) continue;
        if (dget(&d, t, "Encrypt")->t != V_NULL) encrypted = true;
        if (dget(&d, t, "Info")->t == V_DICT) info = dget(&d, t, "Info");
        pv *root = dget(&d, t, "Root");
        if (root->t == V_DICT && dget(&d, root, "Pages")->t == V_DICT) catalog = root;
    }
    if (encrypted) {
        g_archive_err = "Encrypted PDFs aren't supported";
        pdf_free(&d);
        return NULL;
    }

    pages_t pg = { 0 };
    uint8_t *seen = calloc(d.nobj, 1);
    if (seen && catalog->t == V_DICT) walk(&d, dget(&d, catalog, "Pages"), &NULLV, &pg, 0, seen);
    if (!pg.n)                                         /* no usable page tree: every /Type /Page */
        for (uint32_t i = 1; i < d.nobj && pg.n < PDF_MAX_PAGES; i++) {
            if (!d.idx[i].type) continue;
            pv *o = obj(&d, i);
            if (o->t == V_DICT && is_name(dget(&d, o, "Type"), "Page")) walk(&d, o, &NULLV, &pg, 0, seen);
        }

    out_t o = { 0 };
    o.a = archive_alloc("PDF");
    if (!o.a || !seen) {
        free(seen);
        free(pg.page);
        free(pg.resources);
        if (o.a) archive_close(o.a);
        pdf_free(&d);
        return NULL;
    }
    o.a->read = mem_read;

    buf_t all = { 0 };
    pv *title = dget(&d, info, "Title"), *author = dget(&d, info, "Author");
    if (title->t == V_STR) bput(&all, "Title: ", 7), text_string(&all, title), bputc(&all, '\n');
    if (author->t == V_STR) bput(&all, "Author: ", 8), text_string(&all, author), bputc(&all, '\n');
    char line[96];
    int ln = snprintf(line, sizeof(line), "%u page%s\n\n", (unsigned)pg.n, pg.n == 1 ? "" : "s");
    bput(&all, line, (uint32_t)ln);

    txt_t t = { &d, NULL, NULL, 0, 0, false, 0, false, 0 };
    memset(seen, 0, d.nobj);
    for (uint32_t i = 0; i < pg.n && !o.full; i++) {
        buf_t text = { 0 };
        t.o = &text;
        t.unmapped_note = false;
        t.images = 0;
        t.have_y = false;
        pv *pc = dget(&d, pg.page[i], "Contents");
        uint32_t nc = pc->t == V_ARR ? pc->n : 1;
        for (uint32_t k = 0; k < nc; k++) {
            pv *c = pc->t == V_ARR ? res(&d, pc->v[k]) : pc;
            buf_t cs;
            static const double id[6] = { 1, 0, 0, 1, 0, 0 };
            if (c->t == V_DICT && decode(&d, c, &cs, NULL)) run_content(&t, cs.p, cs.n, pg.resources[i], id, 0);
            free(cs.p);
        }
        /* tidy: drop spaces before a line break */
        uint32_t w = 0;
        for (uint32_t r = 0; r < text.n; r++) {
            if (text.p[r] == '\n')
                while (w && text.p[w - 1] == ' ') w--;
            text.p[w++] = text.p[r];
        }
        text.n = w;
        while (text.n && (text.p[text.n - 1] == ' ' || text.p[text.n - 1] == '\n')) text.n--;
        if (t.images) {
            ln = snprintf(line, sizeof(line), "%s[%d image%s]", text.n ? "\n\n" : "", t.images, t.images == 1 ? "" : "s");
            bput(&text, line, (uint32_t)ln);
        }
        bputc(&text, '\n');

        ln = snprintf(line, sizeof(line), "---- page %u ----\n", (unsigned)i + 1);
        bput(&all, line, (uint32_t)ln);
        bput(&all, text.p, text.n);
        bputc(&all, '\n');

        char nm[32];
        snprintf(nm, sizeof(nm), "page_%03u.txt", (unsigned)i + 1);
        add_file(&o, nm, text.p, text.n);
        free(text.p);

        unsigned count = 0;
        images_in(&d, &o, pg.resources[i], i + 1, &count, seen, 0);
        pv *pr = dget(&d, pg.page[i], "Resources");
        if (pr != pg.resources[i] && pr->t == V_DICT) images_in(&d, &o, pr, i + 1, &count, seen, 0);
    }
    if (o.full) bput(&all, "\n(Stopped early: the rest doesn't fit in memory.)\n", 50);
    if (!all.bad) add_file(&o, "text.txt", all.p, all.n);
    o.full = false;
    free(all.p);

    for (uint32_t i = 0; i < t.nfonts; i++) font_free(&t.fonts[i]);
    free(t.fonts);
    free(seen);
    free(pg.page);
    free(pg.resources);
    pdf_free(&d);

    archive_t *a = o.a;
    if (a->tree.count <= 1 || !o.blob.p) {
        g_archive_err = "No pages found in this PDF";
        archive_close(a);
        free(o.blob.p);
        return NULL;
    }
    a->base = *f;
    a->src = src_from_memory(o.blob.p, o.blob.n);
    if (!a->src.ctx) {
        memset(&a->base, 0, sizeof(a->base));
        archive_close(a);
        return NULL;
    }
    return a;
}

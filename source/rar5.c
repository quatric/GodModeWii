#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "rar5.h"

#define NC   306
#define DC   64
#define LDC  16
#define RC   44
#define BC   20
#define TABLE_SIZE (NC + DC + LDC + RC)
#define FAST_BITS 10
#define MAX_FILTER_LEN 0x400000u
#define FLUSH_AT (128 * 1024)
#define IN_BUF (64 * 1024)
#define MAX_WINDOW (256u << 20)

typedef struct {
    uint16_t fast[1 << FAST_BITS];  /* (len << 9) | sym, 0 = use the slow path */
    uint16_t count[16];
    uint16_t sorted[NC];
    int nsym;
} huff_t;

typedef struct { uint64_t start, len; int type, channels; } filter_t;

struct rar5 {
    ar_stream *st;
    char err[96];
    bool solid_archive;
    bool eof;

    /* current file */
    uint64_t data_pos;       /* where its packed data starts */
    uint64_t packed;
    uint64_t usize;
    int method, dict_log;
    unsigned dict_ver;
    bool solid, has_crc, skip;
    uint32_t crc;
    bool prepared;

    /* bit reader over the packed data */
    uint8_t *in;
    size_t in_len, in_pos;
    uint64_t in_left;        /* packed bytes not yet pulled from the stream */
    uint64_t acc;
    int nacc;
    uint64_t consumed;       /* bits consumed in the current block */

    /* decoder state (kept across files of a solid archive) */
    uint8_t *win;
    size_t wsize;            /* power of two */
    uint64_t pos;            /* total bytes produced into the window */
    uint64_t wr;             /* total bytes flushed */
    uint64_t fstart;         /* `pos` at the start of the current file */
    uint32_t dist[4];
    uint32_t last_len;
    bool have_tables;
    huff_t hmain, hdist, hldist, hlen;
    filter_t *flt;
    int nflt, capflt;
    uint8_t *tmp;
    size_t tmp_cap;

    /* output */
    rar5_sink sink;
    void *ud;
    uint32_t ocrc;
    uint64_t emitted;
    bool sink_fail;
};

static bool fail(rar5_t *r, const char *m) { snprintf(r->err, sizeof(r->err), "%s", m); return false; }
const char *rar5_error(const rar5_t *r) { return r->err; }

/* ---------------------------------------------------------------- container */

static bool rd(rar5_t *r, void *b, size_t n) { return ar_read(r->st, b, n) == n; }

static bool rd_vint(rar5_t *r, uint64_t *v, uint8_t *raw, int *nraw)
{
    uint64_t x = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t b;
        if (!rd(r, &b, 1)) return false;
        if (raw) raw[(*nraw)++] = b;
        x |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *v = x; return true; }
    }
    return false;
}

static bool buf_vint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t x = 0;
    for (int i = 0; i < 10 && *p < end; i++) {
        uint8_t b = *(*p)++;
        x |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *v = x; return true; }
    }
    return false;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

rar5_t *rar5_open(ar_stream *st)
{
    uint8_t sig[8];
    if (!ar_seek(st, 0, SEEK_SET) || ar_read(st, sig, 8) != 8 || memcmp(sig, "Rar!\x1a\x07\x01\x00", 8))
        return NULL;
    rar5_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->st = st;
    return r;
}

bool rar5_rewind(rar5_t *r)
{
    r->eof = false;
    r->prepared = false;
    r->have_tables = false;
    r->nflt = 0;
    r->pos = r->wr = 0;
    return ar_seek(r->st, 8, SEEK_SET);
}

void rar5_close(rar5_t *r)
{
    if (!r) return;
    free(r->win); free(r->in); free(r->flt); free(r->tmp);
    free(r);
}

int rar5_next(rar5_t *r, rar5_entry *e)
{
    if (r->eof) return 0;
    if (r->prepared) {      /* previous file was neither extracted nor skipped */
        if (!ar_seek(r->st, (off64_t)(r->data_pos + r->packed), SEEK_SET)) return -1;
        r->prepared = false;
    }
    if (r->in_left) r->in_left = 0;
    if (r->in_len) r->in_len = r->in_pos = 0;
    for (;;) {
        uint8_t crcb[4], raw[16];
        int nraw = 0;
        uint64_t hsize;
        size_t got = ar_read(r->st, crcb, 4);
        if (got == 0) { r->eof = true; return 0; }
        if (got != 4 || !rd_vint(r, &hsize, raw, &nraw)) { fail(r, "Damaged RAR5 header"); return -1; }
        if (hsize < 2 || hsize > 0x100000) { fail(r, "Damaged RAR5 header"); return -1; }
        uint8_t *hb = malloc(hsize + (size_t)nraw);
        if (!hb) { fail(r, "Out of memory"); return -1; }
        memcpy(hb, raw, (size_t)nraw);
        if (!rd(r, hb + nraw, hsize)) { free(hb); fail(r, "Truncated RAR5 header"); return -1; }
        if (ar_crc32(0, hb, hsize + (size_t)nraw) != le32(crcb)) { free(hb); fail(r, "RAR5 header checksum error"); return -1; }
        const uint8_t *p = hb + nraw, *end = hb + nraw + hsize;
        uint64_t type, flags, extra = 0, dsize = 0;
        if (!buf_vint(&p, end, &type) || !buf_vint(&p, end, &flags)) { free(hb); fail(r, "Damaged RAR5 header"); return -1; }
        if (flags & 1) buf_vint(&p, end, &extra);
        if (flags & 2) buf_vint(&p, end, &dsize);
        if (flags & 0x18) { free(hb); fail(r, "Multi-volume RAR archives aren't supported"); return -1; }
        off64_t after = ar_tell(r->st);
        if (type == 4) { free(hb); fail(r, "Encrypted RAR archives aren't supported"); return -1; }
        if (type == 5) { free(hb); r->eof = true; return 0; }
        if (type == 1) {
            uint64_t af = 0;
            buf_vint(&p, end, &af);
            if (af & 1) { free(hb); fail(r, "Multi-volume RAR archives aren't supported"); return -1; }
            r->solid_archive = (af & 4) != 0;
            free(hb);
            continue;
        }
        if (type != 2) {        /* service blocks (comments, quick open...) and unknown ones */
            free(hb);
            if (!ar_seek(r->st, after + (off64_t)dsize, SEEK_SET)) { fail(r, "Truncated RAR5 archive"); return -1; }
            continue;
        }
        uint64_t ff, usize, attrs, comp, host, nlen, mtime32 = 0;
        uint32_t crc = 0;
        bool ok = buf_vint(&p, end, &ff) && buf_vint(&p, end, &usize) && buf_vint(&p, end, &attrs);
        if (ok && (ff & 2)) { if (end - p < 4) ok = false; else p += 4; }
        (void)mtime32;
        if (ok && (ff & 4)) { if (end - p < 4) ok = false; else { crc = le32(p); p += 4; } }
        ok = ok && buf_vint(&p, end, &comp) && buf_vint(&p, end, &host) && buf_vint(&p, end, &nlen);
        if (!ok || nlen >= sizeof(e->name) || (uint64_t)(end - p) < nlen) { free(hb); fail(r, "Damaged RAR5 file header"); return -1; }
        memcpy(e->name, p, (size_t)nlen);
        e->name[nlen] = '\0';
        p += nlen;
        e->skip = false;
        /* extra area: refuse encryption, ignore links */
        const uint8_t *x = end - extra;
        if (extra && extra <= hsize) {
            while (x < end) {
                uint64_t rs, rt;
                const uint8_t *rec = x;
                if (!buf_vint(&rec, end, &rs) || (uint64_t)(end - rec) < rs) break;
                const uint8_t *next = rec + rs;
                const uint8_t *q = rec;
                if (!buf_vint(&q, next, &rt)) break;
                if (rt == 1) { free(hb); fail(r, "Encrypted RAR archives aren't supported"); return -1; }
                if (rt == 5) e->skip = true;
                x = next;
            }
        }
        free(hb);

        e->dir = (ff & 1) != 0;
        e->size = e->dir ? 0 : usize;
        r->usize = usize;
        r->packed = dsize;
        r->data_pos = (uint64_t)after;
        r->has_crc = (ff & 4) != 0;
        r->crc = crc;
        r->method = (int)((comp >> 7) & 7);
        r->solid = (comp & 0x40) != 0;
        r->dict_ver = (unsigned)(comp & 0x3F);
        r->dict_log = (int)((comp >> 10) & 0x1F);
        if (r->dict_ver == 1) {
            unsigned frac = (unsigned)((comp >> 15) & 0x1F);
            r->dict_log = r->dict_log | (int)(frac << 8);
        }
        r->skip = e->dir || e->skip;
        r->prepared = true;
        if (!ar_seek(r->st, after, SEEK_SET)) { fail(r, "Truncated RAR5 archive"); return -1; }
        return 1;
    }
}

/* ---------------------------------------------------------------- bits */

static void refill(rar5_t *r)
{
    while (r->nacc <= 56) {
        if (r->in_pos == r->in_len) {
            size_t want = r->in_left > IN_BUF ? IN_BUF : (size_t)r->in_left;
            if (want) {
                size_t g = ar_read(r->st, r->in, want);
                r->in_len = g;
                r->in_left -= want;
                if (g < want) r->in_left = 0;
            } else {
                r->in_len = 0;
            }
            r->in_pos = 0;
            if (!r->in_len) { r->nacc += 8; continue; }        /* zero padding past the end */
        }
        r->acc |= (uint64_t)r->in[r->in_pos++] << (56 - r->nacc);
        r->nacc += 8;
    }
}

static inline uint32_t peek(rar5_t *r, int n)
{
    if (r->nacc < 32) refill(r);
    return (uint32_t)(r->acc >> (64 - n));
}
static inline void skip(rar5_t *r, int n) { r->acc <<= n; r->nacc -= n; r->consumed += (uint64_t)n; }
static inline uint32_t getbits(rar5_t *r, int n) { uint32_t v = n ? peek(r, n) : 0; skip(r, n); return v; }

/* ---------------------------------------------------------------- huffman */

static bool build(huff_t *h, const uint8_t *len, int n)
{
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    uint16_t offs[17];
    offs[1] = 0;
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return false;             /* over-subscribed */
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    }
    for (int i = 0; i < n; i++)
        if (len[i]) h->sorted[offs[len[i]]++] = (uint16_t)i;
    h->nsym = n;
    /* fast table */
    uint32_t code = 0;
    int idx = 0;
    for (int l = 1; l <= FAST_BITS; l++) {
        for (int k = 0; k < h->count[l]; k++, idx++, code++) {
            int sym = h->sorted[idx];
            uint32_t first = code << (FAST_BITS - l), cnt = 1u << (FAST_BITS - l);
            for (uint32_t j = 0; j < cnt; j++)
                h->fast[first + j] = (uint16_t)((l << 9) | sym);
        }
        code <<= 1;
    }
    return true;
}

static int decode(rar5_t *r, const huff_t *h)
{
    uint32_t v = peek(r, 16);
    uint16_t f = h->fast[v >> (16 - FAST_BITS)];
    if (f) { skip(r, f >> 9); return f & 0x1FF; }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= (int)((v >> (16 - l)) & 1);
        int c = h->count[l];
        if (code - c < first) { skip(r, l); return h->sorted[index + (code - first)]; }
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static bool read_tables(rar5_t *r)
{
    uint8_t bl[BC];
    for (int i = 0; i < BC;) {
        int v = (int)getbits(r, 4);
        if (v == 15) {
            int c = (int)getbits(r, 4);
            if (c == 0) bl[i++] = 15;
            else for (int j = 0; j < c + 2 && i < BC; j++) bl[i++] = 0;
        } else {
            bl[i++] = (uint8_t)v;
        }
    }
    huff_t bd;
    if (!build(&bd, bl, BC)) return fail(r, "Damaged RAR5 data (code lengths)");
    uint8_t t[TABLE_SIZE];
    for (int i = 0; i < TABLE_SIZE;) {
        int s = decode(r, &bd);
        if (s < 0) return fail(r, "Damaged RAR5 data (code table)");
        if (s < 16) { t[i++] = (uint8_t)s; continue; }
        int n;
        bool zero = s >= 18;
        if ((s & 1) == 0) n = (int)getbits(r, 3) + 3;
        else n = (int)getbits(r, 7) + 11;
        if (!zero && i == 0) return fail(r, "Damaged RAR5 data (code table)");
        uint8_t val = zero ? 0 : t[i - 1];
        while (n-- > 0 && i < TABLE_SIZE) t[i++] = val;
    }
    if (!build(&r->hmain, t, NC) || !build(&r->hdist, t + NC, DC) ||
        !build(&r->hldist, t + NC + DC, LDC) || !build(&r->hlen, t + NC + DC + LDC, RC))
        return fail(r, "Damaged RAR5 data (code table)");
    r->have_tables = true;
    return true;
}

/* ---------------------------------------------------------------- output */

static bool emit(rar5_t *r, const uint8_t *p, size_t n)
{
    if (!n) return true;
    r->ocrc = ar_crc32(r->ocrc, p, n);
    r->emitted += n;
    if (r->sink && !r->sink(r->ud, p, n)) { r->sink_fail = true; return false; }
    return true;
}

static bool emit_window(rar5_t *r, uint64_t from, uint64_t to)
{
    while (from < to) {
        size_t off = (size_t)(from & (r->wsize - 1));
        size_t n = r->wsize - off;
        if (n > to - from) n = (size_t)(to - from);
        if (!emit(r, r->win + off, n)) return false;
        from += n;
    }
    return true;
}

static void apply_filter(const filter_t *f, uint8_t *d, size_t n)
{
    switch (f->type) {
    case 0: {                           /* delta */
        uint8_t *src = malloc(n);
        if (!src) return;
        memcpy(src, d, n);
        size_t sp = 0;
        for (int c = 0; c < f->channels; c++) {
            uint8_t prev = 0;
            for (size_t j = (size_t)c; j < n; j += (size_t)f->channels) {
                prev = (uint8_t)(prev - src[sp++]);
                d[j] = prev;
            }
        }
        free(src);
        break;
    }
    case 1:
    case 2: {                           /* x86 E8 / E8E9 */
        const uint32_t fsz = 0x1000000;
        if (n < 5) break;
        for (size_t i = 0; i + 4 < n + 0;) {
            uint8_t b = d[i++];
            if (b == 0xE8 || (b == 0xE9 && f->type == 2)) {
                uint32_t off = (uint32_t)((i + f->start) % fsz);
                uint32_t addr = le32(d + i);
                if (addr & 0x80000000u) {
                    if (((addr + off) & 0x80000000u) == 0) { uint32_t v = addr + fsz; memcpy(d + i, &(uint8_t[4]){ (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }, 4); }
                } else if (((addr - fsz) & 0x80000000u) != 0) {
                    uint32_t v = addr - off;
                    memcpy(d + i, &(uint8_t[4]){ (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }, 4);
                }
                i += 4;
            }
        }
        break;
    }
    case 3:                             /* ARM */
        for (size_t i = 0; i + 3 < n; i += 4) {
            if (d[i + 3] == 0xEB) {
                uint32_t o = d[i] | d[i + 1] << 8 | (uint32_t)d[i + 2] << 16;
                o -= (uint32_t)((i + f->start) / 4);
                d[i] = (uint8_t)o; d[i + 1] = (uint8_t)(o >> 8); d[i + 2] = (uint8_t)(o >> 16);
            }
        }
        break;
    }
}

/* Writes out everything that is final. `all` also forces filters that end exactly at pos. */
static bool flush(rar5_t *r)
{
    for (;;) {
        if (r->nflt) {
            filter_t *f = &r->flt[0];
            uint64_t fs = r->fstart + f->start;
            if (r->wr < fs) {
                uint64_t to = fs < r->pos ? fs : r->pos;
                if (!emit_window(r, r->wr, to)) return false;
                r->wr = to;
                if (r->wr < fs) return true;
            }
            if (r->wr == fs) {
                if (fs + f->len > r->pos) return true;          /* not decoded yet */
                if (f->len > r->tmp_cap) {
                    uint8_t *nt = realloc(r->tmp, (size_t)f->len);
                    if (!nt) return fail(r, "Out of memory");
                    r->tmp = nt;
                    r->tmp_cap = (size_t)f->len;
                }
                for (uint64_t i = 0; i < f->len; i++)
                    r->tmp[i] = r->win[(fs + i) & (r->wsize - 1)];
                apply_filter(f, r->tmp, (size_t)f->len);
                if (!emit(r, r->tmp, (size_t)f->len)) return false;
                r->wr = fs + f->len;
                memmove(r->flt, r->flt + 1, (size_t)(--r->nflt) * sizeof(filter_t));
                continue;
            }
            /* a filter that starts before what has been flushed: drop it */
            memmove(r->flt, r->flt + 1, (size_t)(--r->nflt) * sizeof(filter_t));
            continue;
        }
        if (!emit_window(r, r->wr, r->pos)) return false;
        r->wr = r->pos;
        return true;
    }
}

/* ---------------------------------------------------------------- decoding */

static uint32_t read_filter_val(rar5_t *r)
{
    int bytes = (int)getbits(r, 2) + 1;
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= getbits(r, 8) << (8 * i);
    return v;
}

static int len_of(rar5_t *r, int slot)
{
    int lbits, len = 2;
    if (slot < 8) { lbits = 0; len += slot; }
    else { lbits = slot / 4 - 1; len += (4 | (slot & 3)) << lbits; }
    if (lbits) len += (int)getbits(r, lbits);
    return len;
}

static bool alloc_window(rar5_t *r)
{
    uint64_t dict;
    if (r->dict_ver == 0) dict = (uint64_t)128 * 1024 << r->dict_log;
    else {
        uint64_t base = (uint64_t)128 * 1024 << (r->dict_log & 0xFF);
        dict = base + base / 32 * (uint64_t)(r->dict_log >> 8);
    }
    uint64_t need = dict + MAX_FILTER_LEN + 2 * FLUSH_AT;
    size_t w = 1u << 20;
    while (w < need && w < (size_t)MAX_WINDOW * 2) w <<= 1;
    if (need > MAX_WINDOW) return fail(r, "RAR dictionary is too large for this console");
    if (r->win && (r->solid || w <= r->wsize)) {
        if (w > r->wsize) return fail(r, "RAR dictionary changed inside a solid archive");
        return true;
    }
    free(r->win);
    r->win = malloc(w);
    if (!r->win) { r->wsize = 0; return fail(r, "Not enough memory for the RAR dictionary"); }
    r->wsize = w;
    return true;
}

static bool decode_file(rar5_t *r)
{
    if (!alloc_window(r)) return false;
    if (!r->solid) {
        r->pos = r->wr = 0;
        memset(r->dist, 0, sizeof(r->dist));
        r->last_len = 0;
        r->have_tables = false;
    }
    r->nflt = 0;
    r->fstart = r->pos;
    r->wr = r->pos;
    uint64_t end = r->pos + r->usize;

    while (r->pos < end) {
        /* block header */
        { int pad = r->nacc % 8; r->acc <<= pad; r->nacc -= pad; }
        uint32_t bf = getbits(r, 8), cks = getbits(r, 8);
        int nb = (int)((bf >> 3) & 7) + 1;
        if (nb > 3) return fail(r, "Damaged RAR5 data (block header)");
        uint32_t bsize = 0, x = 0x5A ^ bf;
        for (int i = 0; i < nb; i++) { uint32_t b = getbits(r, 8); bsize |= b << (8 * i); x ^= b; }
        if ((x & 0xFF) != cks) return fail(r, "Damaged RAR5 data (block checksum)");
        uint64_t total_bits = bsize ? (uint64_t)(bsize - 1) * 8 + (bf & 7) + 1 : 0;
        r->consumed = 0;
        if (bf & 0x80) { if (!read_tables(r)) return false; }
        else if (!r->have_tables) return fail(r, "Damaged RAR5 data (no code table)");

        while (r->consumed < total_bits && r->pos < end) {
            int s = decode(r, &r->hmain);
            if (s < 0) return fail(r, "Damaged RAR5 data (symbol)");
            if (s < 256) {
                r->win[r->pos++ & (r->wsize - 1)] = (uint8_t)s;
            } else if (s == 256) {
                filter_t f;
                uint32_t bs = read_filter_val(r), bl = read_filter_val(r);
                f.type = (int)getbits(r, 3);
                f.channels = f.type == 0 ? (int)getbits(r, 5) + 1 : 0;
                f.start = (r->pos - r->fstart) + bs;
                f.len = bl;
                if (bl > MAX_FILTER_LEN || f.type > 3) return fail(r, "Unsupported RAR5 filter");
                if (r->nflt == r->capflt) {
                    int nc = r->capflt ? r->capflt * 2 : 8;
                    filter_t *nf = realloc(r->flt, (size_t)nc * sizeof(filter_t));
                    if (!nf) return fail(r, "Out of memory");
                    r->flt = nf;
                    r->capflt = nc;
                }
                r->flt[r->nflt++] = f;
            } else {
                uint32_t d;
                int len;
                if (s == 257) {
                    if (!r->last_len) continue;
                    len = (int)r->last_len;
                    d = r->dist[0];
                } else if (s < 262) {
                    int k = s - 258;
                    d = r->dist[k];
                    for (int i = k; i > 0; i--) r->dist[i] = r->dist[i - 1];
                    r->dist[0] = d;
                    int ls = decode(r, &r->hlen);
                    if (ls < 0) return fail(r, "Damaged RAR5 data (length)");
                    len = len_of(r, ls);
                    r->last_len = (uint32_t)len;
                } else {
                    len = len_of(r, s - 262);
                    int ds = decode(r, &r->hdist);
                    if (ds < 0) return fail(r, "Damaged RAR5 data (distance)");
                    int dbits = 0;
                    d = (uint32_t)ds;
                    if (ds >= 4) { dbits = ds / 2 - 1; d = (2u | (ds & 1)) << dbits; }
                    if (dbits > 0) {
                        if (dbits >= 4) {
                            if (dbits > 4) d += getbits(r, dbits - 4) << 4;
                            int lo = decode(r, &r->hldist);
                            if (lo < 0) return fail(r, "Damaged RAR5 data (distance)");
                            d += (uint32_t)lo;
                        } else {
                            d += getbits(r, dbits);
                        }
                    }
                    d++;
                    if (d > 0x100) len++;
                    if (d > 0x2000) len++;
                    if (d > 0x40000) len++;
                    r->dist[3] = r->dist[2]; r->dist[2] = r->dist[1]; r->dist[1] = r->dist[0]; r->dist[0] = d;
                    r->last_len = (uint32_t)len;
                }
                if (d == 0 || d > r->pos || d > r->wsize - 2 * FLUSH_AT)
                    return fail(r, "Damaged RAR5 data (bad distance)");
                uint64_t left = end - r->pos;
                if ((uint64_t)len > left) len = (int)left;
                size_t mask = r->wsize - 1;
                for (int i = 0; i < len; i++, r->pos++)
                    r->win[r->pos & mask] = r->win[(r->pos - d) & mask];
            }
            if (r->pos - r->wr >= FLUSH_AT && !flush(r))
                return false;
        }
        if (bf & 0x40) break;
    }
    if (!flush(r)) return false;
    if (r->pos != end || r->wr != end) return fail(r, "Damaged RAR5 data (short file)");
    return true;
}

bool rar5_extract(rar5_t *r, rar5_sink sink, void *ud)
{
    if (!r->prepared) return fail(r, "No current file");
    r->prepared = false;
    r->sink = sink;
    r->ud = ud;
    r->ocrc = 0;
    r->emitted = 0;
    r->sink_fail = false;
    if (r->skip && !r->usize) return true;
    if (r->skip) {
        if (!ar_seek(r->st, (off64_t)(r->data_pos + r->packed), SEEK_SET)) return fail(r, "Truncated RAR5 archive");
        return true;
    }
    if (!sink && !r->solid_archive) {
        return ar_seek(r->st, (off64_t)(r->data_pos + r->packed), SEEK_SET) || fail(r, "Truncated RAR5 archive");
    }

    bool ok;
    if (!r->in) { r->in = malloc(IN_BUF); if (!r->in) return fail(r, "Out of memory"); }
    r->in_len = r->in_pos = 0;
    r->in_left = r->packed;
    r->acc = 0;
    r->nacc = 0;
    if (r->method == 0) {
        uint64_t left = r->usize < r->packed ? r->usize : r->packed;
        ok = true;
        while (ok && left) {
            size_t n = left > IN_BUF ? IN_BUF : (size_t)left;
            if (ar_read(r->st, r->in, n) != n) { ok = fail(r, "Truncated RAR5 archive"); break; }
            ok = emit(r, r->in, n);
            left -= n;
        }
        r->in_left = 0;
    } else {
        if (r->method > 5) return fail(r, "Unknown RAR5 compression method");
        ok = decode_file(r);
    }
    r->in_len = r->in_pos = 0;
    r->in_left = 0;
    if (!ok) return false;
    if (r->has_crc && r->ocrc != r->crc) return fail(r, "RAR5 checksum mismatch (damaged data)");
    return ar_seek(r->st, (off64_t)(r->data_pos + r->packed), SEEK_SET) || fail(r, "Truncated RAR5 archive");
}

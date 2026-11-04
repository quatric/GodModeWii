/*
 * ECDSA on sect233r1 (NIST B-233), the curve of the Wii's console certificates: y^2 + xy = x^3 + x^2 + b
 * over GF(2^233), reduction polynomial x^233 + x^74 + 1. Plain and small, for signing a content.bin: affine
 * points, field inversion by the extended Euclidean algorithm, scalars mod n by shift-and-add.
 *
 * The nonce is not random: it is SHA-1 of the private key, the hash being signed and a counter, reduced
 * mod n, so it is secret, different for every message and never comes from a weak random source (a
 * guessable nonce gives away the private key).
 *
 * Numbers are eight 32-bit words, least significant first; outside, 30-byte big-endian strings.
 */
#include <string.h>

#include "ecc233.h"
#include "sha1.h"

typedef uint32_t el[8];
typedef struct {
    el x, y;
    int inf;
} pt;

static void zero(el a) { memset(a, 0, sizeof(el)); }
static void cpy(el d, const el s) { memcpy(d, s, sizeof(el)); }
static int is_zero(const el a)
{
    for (int i = 0; i < 8; i++)
        if (a[i]) return 0;
    return 1;
}
static int eq(const el a, const el b) { return !memcmp(a, b, sizeof(el)); }
static int bit(const el a, int i) { return (a[i >> 5] >> (i & 31)) & 1; }
static int deg(const el a)
{
    for (int i = 7; i >= 0; i--)
        if (a[i])
            for (int b = 31; b >= 0; b--)
                if (a[i] >> b & 1) return i * 32 + b;
    return -1;
}

static void from_bytes(el a, const uint8_t *p, int n)    /* n big-endian bytes */
{
    zero(a);
    for (int i = 0; i < n; i++) {
        int k = n - 1 - i;                                  /* byte k counted from the low end */
        a[k >> 2] |= (uint32_t)p[i] << ((k & 3) * 8);
    }
}
static void to_bytes(const el a, uint8_t *p)             /* 30 bytes */
{
    for (int i = 0; i < 30; i++) {
        int k = 29 - i;
        p[i] = (uint8_t)(a[k >> 2] >> ((k & 3) * 8));
    }
}
static void from_hex(el a, const char *h)
{
    uint8_t b[30];
    for (int i = 0; i < 30; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = h[i * 2 + j];
            v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        b[i] = (uint8_t)v;
    }
    from_bytes(a, b, 30);
}

/* ---------------------------------------------------------------- GF(2^233) */

static el F_POLY, B, GX, GY, N;
static int ready;

static void init(void)
{
    if (ready) return;
    zero(F_POLY);
    F_POLY[233 >> 5] |= 1u << (233 & 31);
    F_POLY[74 >> 5] |= 1u << (74 & 31);
    F_POLY[0] |= 1;
    from_hex(B, "0066647EDE6C332C7F8C0923BB58213B333B20E9CE4281FE115F7D8F90AD");
    from_hex(GX, "00FAC9DFCBAC8313BB2139F1BB755FEF65BC391F8B36F8F8EB7371FD558B");
    from_hex(GY, "01006A08A41903350678E58528BEBF8A0BEFF867A7CA36716F7E01F81052");
    from_hex(N, "01000000000000000000000000000013E974E72F8A6922031D2603CFE0D7");
    ready = 1;
}

static void f_add(el r, const el a, const el b)
{
    for (int i = 0; i < 8; i++) r[i] = a[i] ^ b[i];
}

static void shl1(el a)
{
    for (int i = 7; i > 0; i--) a[i] = (a[i] << 1) | (a[i - 1] >> 31);
    a[0] <<= 1;
}

static void f_mul(el r, const el a, const el b)
{
    el t;
    zero(t);
    for (int i = 232; i >= 0; i--) {
        shl1(t);
        if (bit(t, 233)) f_add(t, t, F_POLY);
        if (bit(b, i)) f_add(t, t, a);
    }
    cpy(r, t);
}

/* dst ^= src << j */
static void xor_shl(el dst, const el src, int j)
{
    int w = j >> 5, b = j & 31;
    for (int i = 7; i >= w; i--) {
        uint32_t v = src[i - w] << b;
        if (b && i - w - 1 >= 0) v |= src[i - w - 1] >> (32 - b);
        dst[i] ^= v;
    }
}

static void f_inv(el r, const el a)
{
    el u, v, g1, g2, t;
    cpy(u, a);
    cpy(v, F_POLY);
    zero(g1);
    g1[0] = 1;
    zero(g2);
    if (is_zero(a)) {
        zero(r);
        return;
    }
    while (deg(u) > 0) {
        int j = deg(u) - deg(v);
        if (j < 0) {
            cpy(t, u), cpy(u, v), cpy(v, t);
            cpy(t, g1), cpy(g1, g2), cpy(g2, t);
            j = -j;
        }
        xor_shl(u, v, j);
        xor_shl(g1, g2, j);
    }
    cpy(r, g1);
}

/* ---------------------------------------------------------------- points */

static void p_dbl(pt *r, const pt *p)
{
    if (p->inf || is_zero(p->x)) {
        r->inf = 1;
        return;
    }
    el l, t, x3, y3;
    f_inv(t, p->x);
    f_mul(l, p->y, t);
    f_add(l, l, p->x);                  /* l = x + y/x */
    f_mul(x3, l, l);
    f_add(x3, x3, l);
    x3[0] ^= 1;                         /* + a */
    f_mul(y3, p->x, p->x);
    cpy(t, l);
    t[0] ^= 1;
    f_mul(t, t, x3);
    f_add(y3, y3, t);                   /* y3 = x^2 + (l + 1) x3 */
    cpy(r->x, x3);
    cpy(r->y, y3);
    r->inf = 0;
}

static void p_add(pt *r, const pt *p, const pt *q)
{
    if (p->inf) {
        *r = *q;
        return;
    }
    if (q->inf) {
        *r = *p;
        return;
    }
    if (eq(p->x, q->x)) {
        if (eq(p->y, q->y)) p_dbl(r, p);
        else r->inf = 1;                /* q = -p */
        return;
    }
    el l, t, x3, y3;
    f_add(t, p->x, q->x);
    f_inv(t, t);
    f_add(l, p->y, q->y);
    f_mul(l, l, t);                     /* l = (y1 + y2) / (x1 + x2) */
    f_mul(x3, l, l);
    f_add(x3, x3, l);
    f_add(x3, x3, p->x);
    f_add(x3, x3, q->x);
    x3[0] ^= 1;
    f_add(t, p->x, x3);
    f_mul(y3, l, t);
    f_add(y3, y3, x3);
    f_add(y3, y3, p->y);
    cpy(r->x, x3);
    cpy(r->y, y3);
    r->inf = 0;
}

static void p_mul(pt *r, const el k, const pt *p)
{
    pt acc = { .inf = 1 };
    for (int i = 255; i >= 0; i--) {
        p_dbl(&acc, &acc);
        if (bit(k, i)) p_add(&acc, &acc, p);
    }
    *r = acc;
}

/* ---------------------------------------------------------------- integers mod n */

static int n_cmp(const el a, const el b)
{
    for (int i = 7; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static void n_sub(el r, const el a, const el b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}
static void n_add(el r, const el a, const el b)       /* a, b < n: r = a + b mod n */
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    if (n_cmp(r, N) >= 0) n_sub(r, r, N);
}
static void n_mod(el r, const el a)                   /* any 256-bit a */
{
    el t;
    zero(t);
    for (int i = 255; i >= 0; i--) {
        shl1(t);
        t[0] |= (uint32_t)bit(a, i);
        if (n_cmp(t, N) >= 0) n_sub(t, t, N);
    }
    cpy(r, t);
}
static void n_mul(el r, const el a, const el b)       /* a, b < n */
{
    el t;
    zero(t);
    for (int i = 232; i >= 0; i--) {
        n_add(t, t, t);
        if (bit(b, i)) n_add(t, t, a);
    }
    cpy(r, t);
}
static void n_inv(el r, const el a)                   /* a^(n-2) mod n; n is prime */
{
    el e, t, acc;
    zero(e);
    e[0] = 2;
    n_sub(e, N, e);
    zero(acc);
    acc[0] = 1;
    cpy(t, a);
    for (int i = 0; i < 233; i++) {
        if (bit(e, i)) n_mul(acc, acc, t);
        n_mul(t, t, t);
    }
    cpy(r, acc);
}

/* ---------------------------------------------------------------- ECDSA */

void ecc233_public_key(const uint8_t priv[30], uint8_t pub[60])
{
    init();
    el d;
    from_bytes(d, priv, 30);
    pt g = { .inf = 0 }, q;
    cpy(g.x, GX);
    cpy(g.y, GY);
    p_mul(&q, d, &g);
    to_bytes(q.x, pub);
    to_bytes(q.y, pub + 30);
}

void ecc233_sign(const uint8_t priv[30], const uint8_t hash[20], uint8_t r_out[30], uint8_t s_out[30])
{
    init();
    el d, z, k, r, s, t;
    from_bytes(d, priv, 30);
    n_mod(d, d);
    from_bytes(z, hash, 20);
    pt g = { .inf = 0 }, p;
    cpy(g.x, GX);
    cpy(g.y, GY);
    for (uint8_t ctr = 0;; ctr += 2) {
        uint8_t kb[40];
        for (int h = 0; h < 2; h++) {
            sha1_t c;
            uint8_t n = (uint8_t)(ctr + h);
            sha1_init(&c);
            sha1_update(&c, priv, 30);
            sha1_update(&c, hash, 20);
            sha1_update(&c, &n, 1);
            sha1_final(&c, kb + h * 20);
        }
        from_bytes(k, kb, 30);
        memset(kb, 0, sizeof(kb));
        n_mod(k, k);
        if (is_zero(k)) continue;
        p_mul(&p, k, &g);
        if (p.inf) continue;
        n_mod(r, p.x);
        if (is_zero(r)) continue;
        n_mul(t, r, d);
        n_add(t, t, z);                         /* z < 2^160 < n */
        n_inv(s, k);
        n_mul(s, s, t);
        if (is_zero(s)) continue;
        break;
    }
    to_bytes(r, r_out);
    to_bytes(s, s_out);
    memset(d, 0, sizeof(d));
    memset(k, 0, sizeof(k));
}

bool ecc233_verify(const uint8_t pub[60], const uint8_t hash[20], const uint8_t r_in[30], const uint8_t s_in[30])
{
    init();
    el r, s, z, w, u1, u2, v;
    from_bytes(r, r_in, 30);
    from_bytes(s, s_in, 30);
    from_bytes(z, hash, 20);
    if (is_zero(r) || is_zero(s) || n_cmp(r, N) >= 0 || n_cmp(s, N) >= 0) return false;
    n_inv(w, s);
    n_mul(u1, z, w);
    n_mul(u2, r, w);
    pt g = { .inf = 0 }, q = { .inf = 0 }, a, b, x;
    cpy(g.x, GX);
    cpy(g.y, GY);
    from_bytes(q.x, pub, 30);
    from_bytes(q.y, pub + 30, 30);
    p_mul(&a, u1, &g);
    p_mul(&b, u2, &q);
    p_add(&x, &a, &b);
    if (x.inf) return false;
    n_mod(v, x.x);
    return n_cmp(v, r) == 0;
}

#include <string.h>

#include "aes.h"

static uint8_t sbox[256], isbox[256];
static uint32_t td0[256], td1[256], td2[256], td3[256];
static int tables_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static uint32_t ror8(uint32_t x) { return (x >> 8) | (x << 24); }

static void build_tables(void)
{
    /* S-box from the multiplicative inverse + affine transform */
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);       /* p *= 3 */
        q ^= q << 1; q ^= q << 2; q ^= q << 4;                      /* q /= 3 */
        if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                    (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        isbox[sbox[i]] = (uint8_t)i;

    for (int i = 0; i < 256; i++) {
        uint8_t s = isbox[i];
        uint32_t w = ((uint32_t)gmul(s, 0x0E) << 24) | ((uint32_t)gmul(s, 0x09) << 16) |
                     ((uint32_t)gmul(s, 0x0D) << 8) | gmul(s, 0x0B);
        td0[i] = w;
        td1[i] = ror8(w);
        td2[i] = ror8(td1[i]);
        td3[i] = ror8(td2[i]);
    }
    tables_ready = 1;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

void aes128_init_dec(aes128_t *ctx, const uint8_t key[16])
{
    if (!tables_ready)
        build_tables();

    uint32_t ek[44];
    uint32_t rcon = 1;
    for (int i = 0; i < 4; i++)
        ek[i] = rd32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = ek[i - 1];
        if ((i & 3) == 0) {
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)sbox[t >> 24] << 24) | ((uint32_t)sbox[(t >> 16) & 255] << 16) |
                ((uint32_t)sbox[(t >> 8) & 255] << 8) | sbox[t & 255];
            t ^= rcon << 24;
            rcon = xtime((uint8_t)rcon);
        }
        ek[i] = ek[i - 4] ^ t;
    }

    /* Equivalent inverse cipher: reverse the round keys and run InvMixColumns on the middle ones. */
    for (int r = 0; r <= 10; r++) {
        for (int c = 0; c < 4; c++) {
            uint32_t k = ek[(10 - r) * 4 + c];
            if (r != 0 && r != 10)
                k = td0[sbox[k >> 24]] ^ td1[sbox[(k >> 16) & 255]] ^ td2[sbox[(k >> 8) & 255]] ^ td3[sbox[k & 255]];
            ctx->dk[r * 4 + c] = k;
        }
    }
}

static void dec_block(const aes128_t *ctx, const uint8_t *in, uint8_t *out)
{
    const uint32_t *rk = ctx->dk;
    uint32_t s0 = rd32(in) ^ rk[0], s1 = rd32(in + 4) ^ rk[1], s2 = rd32(in + 8) ^ rk[2], s3 = rd32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    rk += 4;
    for (int r = 1; r < 10; r++, rk += 4) {
        t0 = td0[s0 >> 24] ^ td1[(s3 >> 16) & 255] ^ td2[(s2 >> 8) & 255] ^ td3[s1 & 255] ^ rk[0];
        t1 = td0[s1 >> 24] ^ td1[(s0 >> 16) & 255] ^ td2[(s3 >> 8) & 255] ^ td3[s2 & 255] ^ rk[1];
        t2 = td0[s2 >> 24] ^ td1[(s1 >> 16) & 255] ^ td2[(s0 >> 8) & 255] ^ td3[s3 & 255] ^ rk[2];
        t3 = td0[s3 >> 24] ^ td1[(s2 >> 16) & 255] ^ td2[(s1 >> 8) & 255] ^ td3[s0 & 255] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    /* last round: InvSubBytes + InvShiftRows + AddRoundKey */
    t0 = ((uint32_t)isbox[s0 >> 24] << 24) | ((uint32_t)isbox[(s3 >> 16) & 255] << 16) |
         ((uint32_t)isbox[(s2 >> 8) & 255] << 8) | isbox[s1 & 255];
    t1 = ((uint32_t)isbox[s1 >> 24] << 24) | ((uint32_t)isbox[(s0 >> 16) & 255] << 16) |
         ((uint32_t)isbox[(s3 >> 8) & 255] << 8) | isbox[s2 & 255];
    t2 = ((uint32_t)isbox[s2 >> 24] << 24) | ((uint32_t)isbox[(s1 >> 16) & 255] << 16) |
         ((uint32_t)isbox[(s0 >> 8) & 255] << 8) | isbox[s3 & 255];
    t3 = ((uint32_t)isbox[s3 >> 24] << 24) | ((uint32_t)isbox[(s2 >> 16) & 255] << 16) |
         ((uint32_t)isbox[(s1 >> 8) & 255] << 8) | isbox[s0 & 255];
    wr32(out, t0 ^ rk[0]);
    wr32(out + 4, t1 ^ rk[1]);
    wr32(out + 8, t2 ^ rk[2]);
    wr32(out + 12, t3 ^ rk[3]);
}

void aes128_cbc_decrypt(const aes128_t *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t prev[16], cur[16], tmp[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, in + off, 16);          /* keep the ciphertext: in == out is allowed */
        dec_block(ctx, cur, tmp);
        for (int i = 0; i < 16; i++)
            out[off + i] = tmp[i] ^ prev[i];
        memcpy(prev, cur, 16);
    }
}

void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (!tables_ready)
        build_tables();
    uint8_t rk[11][16];
    memcpy(rk[0], key, 16);
    uint8_t rcon = 1;
    for (int r = 1; r <= 10; r++) {
        const uint8_t *p = rk[r - 1];
        uint8_t *n = rk[r];
        n[0] = p[0] ^ sbox[p[13]] ^ rcon;
        n[1] = p[1] ^ sbox[p[14]];
        n[2] = p[2] ^ sbox[p[15]];
        n[3] = p[3] ^ sbox[p[12]];
        for (int i = 4; i < 16; i++)
            n[i] = p[i] ^ n[i - 4];
        rcon = xtime(rcon);
    }
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[0][i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)
            t[i] = sbox[s[i]];
        /* ShiftRows: column-major state, row i shifts left by i */
        uint8_t u[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
                u[c * 4 + row] = t[((c + row) & 3) * 4 + row];
        if (r != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *x = &u[c * 4];
                uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
                x[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
                x[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
                x[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++)
            s[i] = u[i] ^ rk[r][i];
    }
    memcpy(out, s, 16);
}

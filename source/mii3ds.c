/*
 * A Wii Mii as a 3DS / Wii U one (Ver3StoreData, 0x60 bytes) and the encrypted QR payload Mii Maker on the 3DS and
 * Wii U (and Miitomo, Tomodachi Life...) scan. Field layout from 3dbrew's Mii page; the Wii face feature split into
 * the later makeup + wrinkles fields as MiiJS (Stewared, ISC) does it; the QR's AES-CCM wrapping as jaames' mii-qr.py
 * reads it.
 */
#include <string.h>

#include "aes.h"
#include "mii3ds.h"

static void put16le(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16le(p, v & 0xFFFF); put16le(p + 2, v >> 16); }

static unsigned crc16(const uint8_t *p, size_t n)
{
    unsigned c = 0;
    while (n--) {
        c ^= (unsigned)*p++ << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1);
    }
    return c & 0xFFFF;
}

static unsigned clamp(unsigned v, unsigned max) { return v > max ? max : v; }

void mii_to_ver3(const mii *m, uint8_t out[MII3DS_SIZE])
{
    /* Wii face feature 0-11 -> 3DS wrinkles (low nibble) or makeup (high nibble) */
    static const uint8_t feat[12] = { 0x00, 0x10, 0x60, 0x90, 0x05, 0x02, 0x03, 0x07, 0x08, 0xA0, 0x09, 0x0B };
    memset(out, 0, MII3DS_SIZE);
    out[0x00] = 3;
    out[0x01] = 1;                                          /* copying allowed */
    out[0x03] = 1 << 4;                                     /* made on a Wii */
    memcpy(out + 0x04, m->id + 4, 4);                       /* the Wii's own system ID */

    /* The Wii's creation time (4 s steps from 2006) as the 3DS's (2 s steps from 2010) */
    uint32_t wid = (uint32_t)m->id[0] << 24 | m->id[1] << 16 | m->id[2] << 8 | m->id[3];
    uint64_t secs = (uint64_t)(wid & 0x1FFFFFFF) * 4;
    uint32_t ts = secs > 126230400u ? (uint32_t)((secs - 126230400u) / 2) & 0x0FFFFFFF : 0;
    uint32_t id = 0x90000000u | ts;                          /* bit 31: an ordinary (not special) Mii */
    out[0x0C] = id >> 24; out[0x0D] = id >> 16; out[0x0E] = id >> 8; out[0x0F] = id;
    out[0x10] = 0x00; out[0x11] = 0x17; out[0x12] = 0xAB;    /* a Nintendo MAC from the system ID */
    memcpy(out + 0x13, m->id + 5, 3);

    put16le(out + 0x18, (m->sex & 1) | clamp(m->birth_month, 12) << 1 | clamp(m->birth_day, 31) << 5 |
                        clamp(m->fav_color, 11) << 10 | (m->favorite & 1) << 14);
    for (int i = 0; i < MII_NAME_MAX; i++) put16le(out + 0x1A + 2 * i, m->name[i]);
    out[0x2E] = (uint8_t)clamp(m->height, 127);
    out[0x2F] = (uint8_t)clamp(m->build, 127);
    out[0x30] = (m->local_only & 1) | (m->face_type & 15) << 1 | clamp(m->face_color, 5) << 5;
    out[0x31] = feat[clamp(m->face_tex, 11)];
    out[0x32] = m->hair_type;
    out[0x33] = (m->hair_color & 7) | (m->hair_flip & 1) << 3;
    put32le(out + 0x34, (uint32_t)m->eye_type | (uint32_t)m->eye_color << 6 | (uint32_t)m->eye_scale << 9 |
                        3u << 13 | (uint32_t)m->eye_rotate << 16 | (uint32_t)m->eye_x << 21 |
                        (uint32_t)m->eye_y << 25);
    put32le(out + 0x38, (uint32_t)m->brow_type | (uint32_t)m->brow_color << 5 | (uint32_t)m->brow_scale << 8 |
                        3u << 12 | (uint32_t)m->brow_rotate << 16 | (uint32_t)m->brow_x << 21 |
                        (uint32_t)m->brow_y << 25);
    put16le(out + 0x3C, m->nose_type | m->nose_scale << 5 | m->nose_y << 9);
    put16le(out + 0x3E, m->mouth_type | m->mouth_color << 6 | m->mouth_scale << 9 | 3u << 13);
    put16le(out + 0x40, m->mouth_y | m->mustache << 5);
    put16le(out + 0x42, m->beard_type | m->beard_color << 3 | m->beard_scale << 6 | m->beard_y << 10);
    put16le(out + 0x44, m->glass_type | m->glass_color << 4 | m->glass_scale << 7 | m->glass_y << 11);
    put16le(out + 0x46, (m->mole & 1) | m->mole_scale << 1 | m->mole_x << 5 | m->mole_y << 10);
    for (int i = 0; i < MII_NAME_MAX; i++) put16le(out + 0x48 + 2 * i, m->creator[i]);
    unsigned c = crc16(out, 0x5E);
    out[0x5E] = c >> 8; out[0x5F] = c;
}

/* AES-128-CCM (RFC 3610), a 12-byte nonce, a 16-byte tag, no associated data. */
static void ccm_encrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *in, size_t n, uint8_t *out,
                        uint8_t tag[16])
{
    uint8_t b[16], x[16], a[16], s[16];
    b[0] = 0x3A;                                            /* (16-2)/2 << 3 | (3-1) */
    memcpy(b + 1, nonce, 12);
    b[13] = n >> 16; b[14] = n >> 8; b[15] = n;
    aes128_ecb_encrypt(key, b, x);
    for (size_t off = 0; off < n; off += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= off + i < n ? in[off + i] : 0;
        aes128_ecb_encrypt(key, x, x);
    }
    a[0] = 2;
    memcpy(a + 1, nonce, 12);
    for (unsigned ctr = 0; ctr * 16 <= n + 15; ctr++) {
        a[13] = ctr >> 16; a[14] = ctr >> 8; a[15] = ctr;
        aes128_ecb_encrypt(key, a, s);
        if (!ctr) { for (int i = 0; i < 16; i++) tag[i] = x[i] ^ s[i]; continue; }
        size_t off = (ctr - 1) * 16;
        for (size_t i = 0; i < 16 && off + i < n; i++) out[off + i] = in[off + i] ^ s[i];
    }
}

void mii_qr_payload(const mii *m, uint8_t out[MII3DS_QR_SIZE])
{
    static const uint8_t key[16] = { 0x59, 0xFC, 0x81, 0x7E, 0x64, 0x46, 0xEA, 0x61,
                                     0x90, 0x34, 0x7B, 0x20, 0xE9, 0xBD, 0xCE, 0x52 };
    uint8_t v3[MII3DS_SIZE], plain[0x58], nonce[12] = { 0 };
    mii_to_ver3(m, v3);
    memcpy(nonce, v3 + 12, 8);                              /* the Mii ID and the MAC's first half */
    memcpy(plain, v3, 12);
    memcpy(plain + 12, v3 + 20, 0x58 - 12);
    memcpy(out, nonce, 8);
    ccm_encrypt(key, nonce, plain, sizeof(plain), out + 8, out + 8 + 0x58);
}

/*
 * QuickLZ decoders, the two stream versions nintoolbox's lib-quicklz.c tells apart: 1.4.0 at
 * compression level 3 (header bit 6 set; Monster Games' RST archives, "PMCr") and 1.20 (the top six
 * header bits clear). Written from the reference decoders in QuickLZ 1.4.0 / 1.20 (Lasse Mikkel
 * Reinhold), with every multi-byte field read as little endian byte by byte, since the reference
 * reads native words and assumes an x86, and with every copy bounds-checked.
 *
 * Header: flags byte (bit 0 compressed, bit 1 four-byte sizes), then the compressed size (the whole
 * stream, header included) and the decompressed size, one or four bytes each. A stream is only taken
 * when its compressed size is exactly the bytes given.
 */
#include <stdlib.h>
#include <string.h>

#include "qlz.h"

static uint32_t rd(const uint8_t *s, uint32_t n, uint32_t at, uint32_t bytes)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < bytes; i++)
        v |= (uint32_t)(at + i < n ? s[at + i] : 0) << (8 * i);
    return v;
}

static const uint8_t bitlut[16] = { 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0 };

bool qlz_header(const uint8_t *src, uint32_t n, uint32_t *out_size, uint32_t *hdr, int *version)
{
    if (n < 3) return false;
    uint32_t w = (src[0] & 2) ? 4 : 1;
    if (n < 1 + 2 * w) return false;
    int v;
    if (src[0] & 0x40) v = 140;
    else if (!(src[0] & 0xFC)) v = 120;
    else return false;
    uint32_t cs = rd(src, n, 1, w), ds = rd(src, n, 1 + w, w);
    if (cs != n || !ds) return false;
    if (!(src[0] & 1) && ds != n - 1 - 2 * w) return false;      /* stored: exactly its payload */
    *out_size = ds;
    *hdr = 1 + 2 * w;
    *version = v;
    return true;
}

/* 1.4.0, level 3 */
static bool dec140(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t size)
{
    uint32_t sp = 0, dp = 0, cword = 1;
    int64_t last_matchstart = (int64_t)size - 1 - 6 - 4;
    for (;;) {
        if (cword == 1) {
            if (sp + 4 > n) return false;
            cword = rd(s, n, sp, 4);
            sp += 4;
        }
        uint32_t fetch = rd(s, n, sp, 4);
        if (cword & 1) {
            uint32_t off, len;
            cword >>= 1;
            if ((fetch & 3) == 0) { off = (fetch & 0xFF) >> 2; len = 3; sp += 1; }
            else if ((fetch & 2) == 0) { off = (fetch & 0xFFFF) >> 2; len = 3; sp += 2; }
            else if ((fetch & 1) == 0) { off = (fetch & 0xFFFF) >> 6; len = ((fetch >> 2) & 15) + 3; sp += 2; }
            else if ((fetch & 127) != 3) { off = (fetch >> 7) & 0x1FFFF; len = ((fetch >> 2) & 0x1F) + 2; sp += 3; }
            else { off = fetch >> 15; len = ((fetch >> 7) & 255) + 3; sp += 4; }
            if (sp > n || off < 3 || off > dp || size - dp < 4 || len > size - dp - 4) return false;
            for (uint32_t k = 0; k < len; k++, dp++) d[dp] = d[dp - off];
        } else if ((int64_t)dp < last_matchstart) {
            uint32_t k = bitlut[cword & 15];
            if (sp + k > n || k > size - dp) return false;
            memcpy(d + dp, s + sp, k);
            dp += k;
            sp += k;
            cword >>= k;
        } else {
            while (dp < size) {
                if (cword == 1) {
                    sp += 4;
                    cword = 1u << 31;
                }
                if (sp >= n) return false;
                d[dp++] = s[sp++];
                cword >>= 1;
            }
            return true;
        }
    }
}

/* 1.20 */
static bool dec120(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t size)
{
    uint32_t sp = 0, dp = 0, cword = 1;
    int64_t guaranteed = (int64_t)size - 3;
    if (guaranteed <= 0) {                                 /* tiny: a control word, then literals */
        if (4 + (uint64_t)size > n) return false;
        memcpy(d, s + 4, size);
        return true;
    }
    for (;;) {
        if (cword == 1) {
            if (sp + 4 > n) return false;
            cword = rd(s, n, sp, 4) | 1u << 31;
            sp += 4;
        }
        uint32_t fetch = rd(s, n, sp, 4);
        if (cword & 1) {
            uint32_t off = 0, len;
            bool rle = false;
            uint8_t ch = 0;
            cword >>= 1;
            if ((fetch & 3) == 0) { off = (fetch & 0xFF) >> 2; len = 3; sp += 1; }
            else if ((fetch & 2) == 0) { off = (fetch & 0xFFFF) >> 2; len = 3; sp += 2; }
            else if ((fetch & 1) == 0) { off = (fetch & 0xFFFF) >> 6; len = ((fetch >> 2) & 15) + 3; sp += 2; }
            else if ((fetch & 4) == 0) { off = (fetch & 0xFFFFFF) >> 8; len = ((fetch >> 3) & 31) + 3; sp += 3; }
            else if ((fetch & 8) == 0) {
                off = fetch >> 15;
                if (off) {
                    len = ((fetch >> 4) & 2047) + 3;
                    sp += 4;
                } else {
                    len = rd(s, n, sp + 4, 4);
                    off = rd(s, n, sp + 8, 4);
                    sp += 12;
                }
            } else {
                rle = true;
                ch = (uint8_t)(fetch >> 16);
                len = (fetch >> 4) & 0xFFF;
                if (len) sp += 3;
                else {
                    len = rd(s, n, sp + 3, 4);
                    sp += 7;
                }
            }
            if (sp > n || (int64_t)len > guaranteed - dp) return false;
            if (rle) memset(d + dp, ch, len), dp += len;
            else {
                if (!off || off > dp) return false;
                for (uint32_t k = 0; k < len; k++, dp++) d[dp] = d[dp - off];
            }
        } else {
            uint32_t k = bitlut[cword & 15];
            if (sp + k > n || k > size - dp) return false;
            memcpy(d + dp, s + sp, k);
            dp += k;
            sp += k;
            cword >>= k;
            if ((int64_t)dp >= guaranteed) {
                while (dp < size) {
                    if (cword == 1) {
                        sp += 4;
                        cword = 1u << 31;
                    }
                    if (sp >= n) return false;
                    d[dp++] = s[sp++];
                    cword >>= 1;
                }
                return true;
            }
        }
    }
}

uint8_t *qlz_unpack(const uint8_t *src, uint32_t n, uint32_t max_out, uint32_t *out_size)
{
    uint32_t size, hdr;
    int v;
    if (!qlz_header(src, n, &size, &hdr, &v) || size > max_out) return NULL;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    bool ok = !(src[0] & 1) ? (memcpy(out, src + hdr, size), true)
            : v == 140 ? dec140(src + hdr, n - hdr, out, size) : dec120(src + hdr, n - hdr, out, size);
    if (!ok) {
        free(out);
        return NULL;
    }
    *out_size = size;
    return out;
}

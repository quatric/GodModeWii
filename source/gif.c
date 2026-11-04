/*
 * GIF87a / GIF89a, first frame, to RGBA. Fields are little-endian and read a byte at a time (the Wii is
 * big-endian). The frame is drawn over the logical screen, transparent around it; interlaced frames are
 * put back in order.
 */
#include <stdlib.h>
#include <string.h>

#include "gif.h"

#define GIF_MAX_SIDE 4096

static unsigned g16(const uint8_t *p) { return p[0] | p[1] << 8; }

uint8_t *gif_decode(const uint8_t *d, uint32_t n, int *out_w, int *out_h)
{
    if (n < 13 || memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9') || d[5] != 'a') return NULL;
    unsigned sw = g16(d + 6), sh = g16(d + 8), flags = d[10];
    uint32_t p = 13;
    const uint8_t *gct = NULL;
    unsigned gct_n = 0;
    if (flags & 0x80) {
        gct_n = 2u << (flags & 7);
        if (p + gct_n * 3 > n) return NULL;
        gct = d + p;
        p += gct_n * 3;
    }
    int trans = -1;
    while (p < n) {
        uint8_t b = d[p++];
        if (b == 0x21) {                                /* extension: keep a graphic control's transparency */
            if (p >= n) return NULL;
            uint8_t label = d[p++];
            if (label == 0xF9 && p + 6 <= n && d[p] >= 4 && (d[p + 1] & 1)) trans = d[p + 4];
            while (p < n && d[p]) p += d[p] + 1u;
            p++;
            continue;
        }
        if (b != 0x2C) return NULL;                     /* not an image (or the trailer, 0x3B): nothing drawn */
        if (p + 9 > n) return NULL;
        unsigned fx = g16(d + p), fy = g16(d + p + 2), fw = g16(d + p + 4), fh = g16(d + p + 6), ff = d[p + 8];
        p += 9;
        const uint8_t *ct = gct;
        unsigned ct_n = gct_n;
        if (ff & 0x80) {
            ct_n = 2u << (ff & 7);
            if (p + ct_n * 3 > n) return NULL;
            ct = d + p;
            p += ct_n * 3;
        }
        if (!sw || !sh) sw = fx + fw, sh = fy + fh;
        if (!ct || !fw || !fh || !sw || !sh || sw > GIF_MAX_SIDE || sh > GIF_MAX_SIDE || p >= n) return NULL;
        unsigned min = d[p++];
        if (min < 2 || min > 11) return NULL;

        /* the LZW data, its sub-blocks joined */
        uint32_t q = p, total = 0;
        while (q < n && d[q]) total += d[q], q += d[q] + 1u;
        uint8_t *lz = malloc(total + 1);
        uint8_t *idx = malloc((size_t)fw * fh);
        uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
        uint8_t *suffix = malloc(4096), *stack = malloc(4097);
        uint8_t *rgba = calloc((size_t)sw * sh, 4);
        if (!lz || !idx || !prefix || !suffix || !stack || !rgba) {
            free(lz), free(idx), free(prefix), free(suffix), free(stack), free(rgba);
            return NULL;
        }
        for (uint32_t k = 0; p < n && d[p];) {
            uint32_t l = d[p];
            if (p + 1 + l > n) l = n - p - 1;
            memcpy(lz + k, d + p + 1, l);
            k += l;
            p += d[p] + 1u;
        }

        unsigned clear = 1u << min, eoi = clear + 1, size = min + 1, next = clear + 2, old = 0xFFFF;
        uint8_t first = 0;
        uint32_t bits = 0, nb = 0, bp = 0, out = 0, want = fw * fh;
        for (unsigned i = 0; i < clear; i++) prefix[i] = 0xFFFF, suffix[i] = (uint8_t)i;
        while (out < want) {
            while (nb < size && bp < total) bits |= (uint32_t)lz[bp++] << nb, nb += 8;
            if (nb < size) break;
            unsigned code = bits & ((1u << size) - 1);
            bits >>= size;
            nb -= size;
            if (code == clear) {
                size = min + 1;
                next = clear + 2;
                old = 0xFFFF;
                continue;
            }
            if (code == eoi) break;
            unsigned c = code, sp = 0;
            if (old == 0xFFFF) {                        /* the first code after a clear: a plain index */
                if (code >= clear) break;
                idx[out++] = (uint8_t)code;
                first = (uint8_t)code;
                old = code;
                continue;
            }
            if (code > next || (code == next && next >= 4096)) break;
            if (code == next) stack[sp++] = first, c = old;
            while (c >= clear && sp < 4096) {
                if (c >= next) goto done;
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = (uint8_t)c;
            first = (uint8_t)c;
            while (sp && out < want) idx[out++] = stack[--sp];
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = first;
                next++;
                if (next == (1u << size) && size < 12) size++;
            }
            old = code;
        }
    done:
        memset(idx + out, trans >= 0 ? trans : 0, want - out);    /* a short stream: the rest left blank */

        bool inter = ff & 0x40;
        static const unsigned start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
        unsigned row = 0, pass = 0;
        for (unsigned y = 0; y < fh; y++) {
            unsigned ry = y;
            if (inter) {
                while (pass < 4 && start[pass] + row * step[pass] >= fh) pass++, row = 0;
                if (pass >= 4) break;
                ry = start[pass] + row * step[pass];
                row++;
            }
            unsigned dy = fy + ry;
            if (dy >= sh) continue;
            for (unsigned x = 0; x < fw; x++) {
                unsigned dx = fx + x, ci = idx[y * fw + x];
                if (dx >= sw || (int)ci == trans || ci >= ct_n) continue;
                uint8_t *o = rgba + ((size_t)dy * sw + dx) * 4;
                o[0] = ct[ci * 3], o[1] = ct[ci * 3 + 1], o[2] = ct[ci * 3 + 2], o[3] = 0xFF;
            }
        }
        free(lz), free(idx), free(prefix), free(suffix), free(stack);
        *out_w = (int)sw;
        *out_h = (int)sh;
        return rgba;
    }
    return NULL;
}

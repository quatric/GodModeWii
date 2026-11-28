/*
 * Infrared protocol encoders: a code becomes mark/space timings that the
 * platform transmitter (ir_wii.c / ir_wiiu.c) plays back.
 * Shared, unchanged, between libwiiir and libwiiuir.
 *
 * Based on WiiIR by Dakota Thorpe, used with permission.
 * Copyright 2025 Dakota Thorpe
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bit layouts and timings follow Flipper Zero's infrared app, so the address
 * and command values in Flipper-IRDB files send what the Flipper sends.
 */
#include <string.h>
#include <strings.h>

#include "ir_platform.h"

typedef struct {
    uint32_t *out;
    size_t max, n;
    bool full;
} tbuf;

/* Adds a mark or space, merging it into the last timing when the level is the
 * same; a leading space is dropped. */
static void put(tbuf *b, bool mark, uint32_t us)
{
    if (!us || (b->n == 0 && !mark))
        return;
    if ((b->n % 2 == 0) != mark) {
        b->out[b->n - 1] += us;
        return;
    }
    if (b->n == b->max) {
        b->full = true;
        return;
    }
    b->out[b->n++] = us;
}

/* Pads the frame that began at timing `start` out to `period`, or by at least `gap`. */
static void pad(tbuf *b, size_t start, uint32_t period, uint32_t gap)
{
    uint32_t len = 0;
    for (size_t i = start; i < b->n; i++)
        len += b->out[i];
    put(b, false, period > len + gap ? period - len : gap);
}

typedef struct {
    uint32_t hdr_mark, hdr_space, one_mark, one_space, zero_mark, zero_space, stop;
    uint32_t period, gap; /* frame start to start, or minimum gap after it */
    int repeats;
} pd_timing;

static const pd_timing NEC      = {9000, 4500, 560, 1690, 560, 560, 560, 110000, 4000, 1};
static const pd_timing SAMSUNG  = {4500, 4500, 550, 1650, 550, 550, 550, 108000, 4000, 1};
static const pd_timing KASEIKYO = {3456, 1728, 432, 1296, 432, 432, 432, 130000, 4000, 1};
static const pd_timing RCA      = {4000, 4000, 500, 2000, 500, 1000, 500, 0, 8000, 1};
static const pd_timing PIONEER  = {8500, 4225, 500, 1500, 500, 500, 500, 0, 26000, 2};
static const pd_timing SIRC     = {2400, 600, 1200, 600, 600, 600, 0, 45000, 10000, 3};
static const pd_timing JVC      = {8400, 4200, 526, 1574, 526, 524, 526, 60000, 4000, 1};

/* Pulse distance / pulse width frames, bits sent LSB first. */
static void pulses(tbuf *b, const pd_timing *t, uint64_t bits, int len)
{
    for (int r = 0; r < t->repeats; r++) {
        size_t start = b->n;
        put(b, true, t->hdr_mark);
        put(b, false, t->hdr_space);
        for (int i = 0; i < len; i++) {
            bool one = (bits >> i) & 1;
            put(b, true, one ? t->one_mark : t->zero_mark);
            put(b, false, one ? t->one_space : t->zero_space);
        }
        put(b, true, t->stop);
        pad(b, start, t->period, t->gap);
    }
}

/* Bi-phase bits, MSB first. RC5's one is space-mark, RC6's is mark-space. */
static void manchester(tbuf *b, uint32_t value, int len, uint32_t half, bool rc6)
{
    for (int i = len - 1; i >= 0; i--) {
        bool one = (value >> i) & 1;
        put(b, rc6 ? one : !one, half);
        put(b, rc6 ? !one : one, half);
    }
}

static bool rc5_toggle, rc6_toggle;
static bool repeating; /* IR_SendRepeat: use the toggle bit of the last send again */

static const char *const names[IR_PROTO_COUNT] = {
    "NEC", "NECext", "NEC42", "NEC42ext", "Samsung32", "RC5", "RC5X", "RC6",
    "SIRC", "SIRC15", "SIRC20", "Kaseikyo", "RCA", "Pioneer", "JVC", "RAW",
};

const char *IR_ProtocolName(ir_protocol protocol)
{
    return (unsigned)protocol < IR_PROTO_COUNT ? names[protocol] : "";
}

int IR_ProtocolFromName(const char *name)
{
    for (int i = 0; name && i < IR_PROTO_COUNT; i++)
        if (!strcasecmp(name, names[i]))
            return i;
    return -1;
}

size_t IR_Encode(const ir_signal *s, uint32_t *out, size_t max,
                 uint32_t *frequency, float *duty_cycle)
{
    if (!s || !out || !max)
        return 0;
    tbuf b = {out, max, 0, false};
    uint32_t freq = 38000;
    float duty = 0.33f;
    uint32_t a = s->address, c = s->command;

    switch (s->protocol) {
    case IR_PROTO_NEC:
        pulses(&b, &NEC, (a & 0xFF) | (~a & 0xFF) << 8 | (c & 0xFF) << 16 | (~c & 0xFF) << 24, 32);
        break;
    case IR_PROTO_NECEXT:
        pulses(&b, &NEC, (a & 0xFFFF) | (c & 0xFFFF) << 16, 32);
        break;
    case IR_PROTO_NEC42:
        pulses(&b, &NEC, (a & 0x1FFF) | (uint64_t)(~a & 0x1FFF) << 13 |
                         (uint64_t)(c & 0xFF) << 26 | (uint64_t)(~c & 0xFF) << 34, 42);
        break;
    case IR_PROTO_NEC42EXT:
        pulses(&b, &NEC, (a & 0x3FFFFFF) | (uint64_t)(c & 0xFFFF) << 26, 42);
        break;
    case IR_PROTO_SAMSUNG32:
        pulses(&b, &SAMSUNG, (a & 0xFF) | (a & 0xFF) << 8 | (c & 0xFF) << 16 | (~c & 0xFF) << 24, 32);
        break;
    case IR_PROTO_KASEIKYO: {
        /* address: id << 24 | vendor << 8 | genre1 << 4 | genre2 */
        uint8_t d[6];
        d[0] = a >> 8;
        d[1] = a >> 16;
        uint8_t vp = d[0] ^ d[1];
        d[2] = ((vp & 0xF) ^ (vp >> 4)) | ((a >> 4) & 0xF) << 4;
        d[3] = (a & 0xF) | (c & 0xF) << 4;
        d[4] = ((a >> 24) & 3) << 6 | ((c >> 4) & 0x3F);
        d[5] = d[2] ^ d[3] ^ d[4];
        uint64_t bits = 0;
        for (int i = 5; i >= 0; i--)
            bits = bits << 8 | d[i];
        pulses(&b, &KASEIKYO, bits, 48);
        break;
    }
    case IR_PROTO_RCA:
        pulses(&b, &RCA, (a & 0xF) | (c & 0xFF) << 4 | (~a & 0xF) << 12 | (~c & 0xFF) << 16, 24);
        break;
    case IR_PROTO_PIONEER:
        freq = 40000;
        pulses(&b, &PIONEER, (a & 0xFF) | (~a & 0xFF) << 8 | (c & 0xFF) << 16 | (~c & 0xFF) << 24, 32);
        break;
    case IR_PROTO_SIRC:
        freq = 40000;
        pulses(&b, &SIRC, (c & 0x7F) | (a & 0x1F) << 7, 12);
        break;
    case IR_PROTO_SIRC15:
        freq = 40000;
        pulses(&b, &SIRC, (c & 0x7F) | (a & 0xFF) << 7, 15);
        break;
    case IR_PROTO_SIRC20:
        freq = 40000;
        pulses(&b, &SIRC, (c & 0x7F) | (a & 0x1FFF) << 7, 20);
        break;
    case IR_PROTO_JVC:
        pulses(&b, &JVC, (a & 0xFF) | (c & 0xFF) << 8, 16);
        break;
    case IR_PROTO_RC5:
    case IR_PROTO_RC5X: {
        /* start, field (inverted command bit 6), toggle, 5 address, 6 command.
         * Flipper stores RC5X commands without bit 6, so RC5X always clears the field. */
        bool field = s->protocol == IR_PROTO_RC5 && !(c & 0x40);
        freq = 36000;
        bool toggle = repeating ? !rc5_toggle : rc5_toggle;
        manchester(&b, 1u << 13 | field << 12 | toggle << 11 | (a & 0x1F) << 6 | (c & 0x3F), 14, 889, false);
        put(&b, false, 27000);
        if (!repeating)
            rc5_toggle = !rc5_toggle;
        break;
    }
    case IR_PROTO_RC6:
        /* leader, start bit, mode 0, double-length toggle, 8 address, 8 command */
        freq = 36000;
        put(&b, true, 2666);
        put(&b, false, 889);
        manchester(&b, 0x8, 4, 444, true);
        manchester(&b, repeating ? !rc6_toggle : rc6_toggle, 1, 889, true);
        manchester(&b, (a & 0xFF) << 8 | (c & 0xFF), 16, 444, true);
        put(&b, false, 27000);
        if (!repeating)
            rc6_toggle = !rc6_toggle;
        break;
    case IR_PROTO_RAW:
        if (!s->timings)
            return 0;
        if (s->frequency)
            freq = s->frequency;
        if (s->duty_cycle > 0.0f && s->duty_cycle < 1.0f)
            duty = s->duty_cycle;
        for (size_t i = 0; i < s->count; i++)
            put(&b, i % 2 == 0, s->timings[i]);
        break;
    default:
        return 0;
    }

    if (b.full)
        return 0;
    if (frequency)
        *frequency = freq;
    if (duty_cycle)
        *duty_cycle = duty;
    return b.n;
}

size_t IR_ProntoToTimings(const uint16_t *pronto, size_t length,
                          uint32_t *out, size_t max, uint32_t *frequency)
{
    /* 0000 (learned, modulated), frequency code, once pairs, repeat pairs */
    if (!pronto || length < 4 || pronto[0] != 0x0000 || !pronto[1])
        return 0;
    size_t pairs = (size_t)pronto[2] + pronto[3];
    if (!pairs || length < 4 + 2 * pairs || 2 * pairs > max)
        return 0;
    double unit = pronto[1] * 0.241246; /* microseconds per carrier cycle */
    for (size_t i = 0; i < 2 * pairs; i++)
        out[i] = (uint32_t)(pronto[4 + i] * unit + 0.5);
    if (frequency)
        *frequency = (uint32_t)(1000000.0 / unit + 0.5);
    return 2 * pairs;
}

int IR_Send(const ir_signal *signal)
{
    if (!signal)
        return -1;
    if (signal->protocol == IR_PROTO_RAW)
        return IR_SendTimings(signal->frequency ? signal->frequency : 38000,
                              signal->duty_cycle > 0.0f ? signal->duty_cycle : 0.33f,
                              signal->timings, signal->count);
    uint32_t t[IR_MAX_TIMINGS], freq;
    float duty;
    size_t n = IR_Encode(signal, t, IR_MAX_TIMINGS, &freq, &duty);
    return n ? IR_SendTimings(freq, duty, t, n) : -1;
}

int IR_SendRepeat(const ir_signal *signal)
{
    repeating = true;
    int ret = IR_Send(signal);
    repeating = false;
    return ret;
}

int IR_SendCode(ir_protocol protocol, uint32_t address, uint32_t command)
{
    ir_signal s;
    memset(&s, 0, sizeof s);
    s.protocol = protocol;
    s.address = address;
    s.command = command;
    return protocol == IR_PROTO_RAW ? -1 : IR_Send(&s);
}

int IR_SendPronto(const uint16_t *pronto, size_t length)
{
    uint32_t t[512], freq;
    size_t n = IR_ProntoToTimings(pronto, length, t, 512, &freq);
    return n ? IR_SendTimings(freq, 0.33f, t, n) : -1;
}

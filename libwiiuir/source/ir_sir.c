/*
 * A consumer IR carrier out of an IrDA SIR port.
 *
 * SIR sends each byte as 10 bit slots (start, 8 data bits LSB first, stop)
 * and lights the LED for a short pulse in every slot holding a 0. With the
 * bit rate set to the carrier frequency, slots that are 0 make the carrier:
 * a mark is 0 bits, a space is 1 bits. The start slot of every byte always
 * pulses and the stop slot never does, so a space carries a faint rate/10
 * pulse train and a mark misses one pulse in ten; IR receivers tolerate both.
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ir_platform.h"

size_t ir_sir_encode(const uint32_t *timings, size_t count, uint32_t rate,
                     uint8_t *out, size_t max)
{
    if (!timings || !count || !rate)
        return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += timings[i];
    size_t bytes = (total * rate / 1000000 + 9) / 10;
    if (!bytes || bytes > max)
        return 0;

    size_t i = 0;
    uint64_t end = timings[0]; /* end of timing i, microseconds */
    for (size_t b = 0; b < bytes; b++) {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++) {
            /* middle of the slot: (2 * slot + 1) / (2 * rate) seconds */
            uint64_t mid = (2 * (b * 10 + 1 + bit) + 1) * 1000000ULL;
            while (i < count && end * 2 * rate <= mid)
                if (++i < count)
                    end += timings[i];
            if (!(i < count && i % 2 == 0))
                v |= 1 << bit; /* dark */
        }
        out[b] = v;
    }
    return bytes;
}

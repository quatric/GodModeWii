/*
 * Host test of the IrDA SIR carrier bytes.
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>

#include "ir_platform.h"

/* Is slot s lit when the port sends these bytes? (start pulses, stop dark, 0 bits pulse) */
static bool lit(const uint8_t *b, size_t s)
{
    size_t k = s % 10;
    return k == 0 || (k < 9 && !(b[s / 10] >> (k - 1) & 1));
}

int main(void)
{
    uint8_t out[4096];
    const uint32_t rate = 38400;

    /* NEC-like: 9 ms mark, 4.5 ms space, 560 us mark */
    uint32_t t[] = {9000, 4500, 560};
    size_t n = ir_sir_encode(t, 3, rate, out, sizeof out);
    assert(n == (14060ull * rate / 1000000 + 9) / 10);

    /* every data slot matches the level at its middle */
    for (size_t s = 0; s < n * 10; s++) {
        if (s % 10 == 0 || s % 10 == 9)
            continue;
        double mid = (s + 0.5) * 1e6 / rate;
        bool mark = mid < 9000 || (mid >= 13500 && mid < 14060);
        assert(lit(out, s) == mark);
    }
    /* the mark is 0 bytes, the space 0xFF */
    assert(out[0] == 0x00 && out[10] == 0x00 && out[40] == 0xFF);

    assert(ir_sir_encode(t, 3, rate, out, 10) == 0); /* does not fit */
    assert(ir_sir_encode(t, 0, rate, out, sizeof out) == 0);
    puts("ok");
    return 0;
}

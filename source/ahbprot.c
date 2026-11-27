/*
 * Turns HW_AHBPROT off from inside IOS, so GodModeWii reaches the hardware (NAND patches, keys, SEEPROM,
 * the disc drive) even when the loader that started it didn't leave it off (the Homebrew Channel does, for
 * apps whose meta.xml asks with <ahb_access/>; forwarders and other loaders may not).
 *
 * From Aeplet's IOS (https://github.com/Aeplet/IOS, ios.c), MIT License, used with its notice below. It is
 * mkwcat's /dev/sha exploit: SHA_Init with its context at 0xFFFE0028 overwrites an IOS thread's saved PC,
 * so stage0 (at the start of MEM1) runs in IOS, calls stage1 to set AHBPROT's bits, and leaves that thread
 * looping in IOS's own exception vectors. Changes here: the stage code is flushed from the PPC's cache
 * before IOS reads it, and the bytes it borrows at 0x80000000 (the disc ID) are put back afterwards.
 *
 * MIT License
 *
 * Copyright (c) 2025 Aep
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/ipc.h>

#include "ahbprot.h"
#include "runtimeiospatch.h"

static const u32 stage0[] = {
    0x4903468D, /* ldr r1, =0x10100000; mov sp, r1; */
    0x49034788, /* ldr r1, =entrypoint; blx r1; */
    /* Overwrite reserved handler to loop infinitely */
    0x49036209, /* ldr r1, =0xFFFF0014; str r1, [r1, #0x20]; */
    0x47080000, /* bx r1 */
    0x10100000, /* temporary stack */
    0x00000000, /* entrypoint */
    0xFFFF0014, /* reserved handler */
};

static const u32 stage1[] __attribute__((aligned(32))) = {
    0xE3A01536, /* mov r1, #0x0D800000 */
    0xE5910064, /* ldr r0, [r1, #0x64] */
    0xE380013A, /* orr r0, #0x8000000E */
    0xE3800EDF, /* orr r0, #0x00000DF0 */
    0xE5810064, /* str r0, [r1, #0x64] */
    0xE12FFF1E, /* bx  lr */
};

bool ahbprot_disable(void)
{
    if (AHBPROT_DISABLED)
        return true;

    u32 *const mem1 = (u32 *)0x80000000;
    u32 saved[sizeof(stage0) / 4];
    memcpy(saved, mem1, sizeof(saved));

    __attribute__((__aligned__(32))) ioctlv vectors[3] = {
        [1] = { .data = (void *)0xFFFE0028, .len = 0 },
        [2] = { .data = mem1, .len = 0x20 },
    };

    memcpy(mem1, stage0, sizeof(stage0));
    mem1[5] = ((u32)stage1) & ~0xC0000000;
    DCFlushRange(mem1, 32);
    DCFlushRange((void *)stage1, sizeof(stage1));

    bool ok = IOS_Ioctlv(0x10001, 0, 1, 2, vectors) >= 0;
    for (int tries = 1000; ok && !AHBPROT_DISABLED; tries--) {
        if (!tries)
            ok = false;
        else
            usleep(1000);
    }

    /* the IOS thread has left stage0 by now (it loops in the exception vectors) */
    memcpy(mem1, saved, sizeof(saved));
    DCFlushRange(mem1, 32);
    return ok && AHBPROT_DISABLED;
}

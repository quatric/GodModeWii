/*
 * Read-only OTP access: one command word per 32-bit block through the
 * Hollywood OTP registers, as done by libOTP (joedj) and xyzzy-mod
 * (DarkMatterCore).
 */
#include <string.h>
#include "otp.h"

#define HW_OTP_COMMAND (*(vu32 *)0xCD8001EC)
#define HW_OTP_DATA    (*(vu32 *)0xCD8001F0)

bool otp_read_all(u8 out[OTP_SIZE])
{
    for (u32 i = 0; i < OTP_SIZE / 4; i++) {
        HW_OTP_COMMAND = 0x80000000 | i;
        u32 v = HW_OTP_DATA;
        out[i * 4] = v >> 24;
        out[i * 4 + 1] = v >> 16;
        out[i * 4 + 2] = v >> 8;
        out[i * 4 + 3] = v;
    }
    u8 all_or = 0, all_and = 0xFF;
    for (int i = 0; i < OTP_SIZE; i++) {
        all_or |= out[i];
        all_and &= out[i];
    }
    return all_or != 0 && all_and != 0xFF;
}

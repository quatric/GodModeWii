#ifndef OTP_H
#define OTP_H

#include <stdbool.h>
#include <gccore.h>

#define OTP_SIZE 0x80

/* Layout of the 128-byte OTP (see WiiBrew "Hardware/OTP"). */
typedef struct {
    u8 boot1_hash[20];
    u8 common_key[16];
    u8 ng_id[4];
    union {                     /* the first two bytes of nand_hmac overlap the last two of ng_priv */
        struct {
            u8 ng_priv[30];
            u8 _pad1[18];
        };
        struct {
            u8 _pad2[28];
            u8 nand_hmac[20];
        };
    };
    u8 nand_key[16];
    u8 rng_key[16];
    u32 unk1;
    u32 unk2;
} otp_t;

#include <stddef.h>
_Static_assert(sizeof(otp_t) == OTP_SIZE, "otp_t must be 128 bytes");
_Static_assert(offsetof(otp_t, ng_priv) == 0x28, "ng_priv offset");
_Static_assert(offsetof(otp_t, nand_hmac) == 0x44, "nand_hmac offset");
_Static_assert(offsetof(otp_t, nand_key) == 0x58, "nand_key offset");
_Static_assert(offsetof(otp_t, rng_key) == 0x68, "rng_key offset");

/* Reads the whole OTP (needs HW_AHBPROT off). */
bool otp_read_all(u8 out[OTP_SIZE]);

#endif

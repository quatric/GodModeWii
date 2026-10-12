/*
 * Read-only SEEPROM access for the original Wii.
 *
 * The SEEPROM is a Microwire EEPROM (93C56 or similar, 128 x 16 bit words)
 * that hangs off four Hollywood GPIO lines which are owned by Starlet:
 * pin 10 = CS, 11 = CLK, 12 = MOSI, 13 = MISO. We bit-bang the READ command
 * through HW_GPIO_{OUT,IN}; this needs the PPC to have full register
 * access (HW_AHBPROT disabled, i.e. launched from the Homebrew Channel).
 * Only the four EEPROM pins are touched and they are restored afterwards.
 * The access sequence is the same one xyzzy-mod (DarkMatterCore) uses, which
 * took it from libSEEPROM (joedj) / MINI (Sven Peter, Haxx, marcan, ...).
 *
 * Register map and pin numbers: WiiBrew "Hardware/Hollywood Registers" and
 * "Hardware/Hollywood GPIOs"; READ framing (start bit + opcode 10 + 8 bit
 * word address, then 16 data bits) follows the 93Cxx Microwire protocol as
 * used by BootMii.
 */
#include <string.h>
#include <unistd.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>

#include "seeprom.h"

#define HW_GPIO_OUT (*(vu32 *)0xCD8000E0)
#define HW_GPIO_IN  (*(vu32 *)0xCD8000E8)

#define EEP_CS   (1u << 10)
#define EEP_CLK  (1u << 11)
#define EEP_MOSI (1u << 12)
#define EEP_MISO (1u << 13)
#define EEP_MASK (EEP_CS | EEP_CLK | EEP_MOSI | EEP_MISO)

/* busy-wait ~5 us: usleep() would sleep far longer than a clock phase */
static inline void tick(void)
{
    u64 start = gettime();
    while (ticks_to_microsecs(gettime() - start) < 5)
        ;
}

static void send_bits(u32 value, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        if ((value >> i) & 1) HW_GPIO_OUT |= EEP_MOSI;
        else                  HW_GPIO_OUT &= ~EEP_MOSI;
        tick();
        HW_GPIO_OUT |= EEP_CLK;
        tick();
        HW_GPIO_OUT &= ~EEP_CLK;
        tick();
    }
}

static u16 recv_bits16(void)
{
    u16 v = 0;
    HW_GPIO_OUT &= ~EEP_MOSI;
    for (int i = 0; i < 16; i++) {
        HW_GPIO_OUT |= EEP_CLK;
        tick();
        HW_GPIO_OUT &= ~EEP_CLK;
        tick();
        v = (v << 1) | ((HW_GPIO_IN & EEP_MISO) ? 1 : 0);
    }
    return v;
}

bool seeprom_read(u8 out[SEEPROM_SIZE])
{
    u32 save_out = HW_GPIO_OUT;

    /* The pin directions are already set up by IOS, as in xyzzy-mod. */
    HW_GPIO_OUT &= ~(EEP_CS | EEP_CLK | EEP_MOSI);
    tick();

    for (int w = 0; w < SEEPROM_SIZE / 2; w++) {
        HW_GPIO_OUT |= EEP_CS;
        tick();
        send_bits(0x600 | w, 11);               /* 1 | 10 (READ) | 8 bit address */
        u16 v = recv_bits16();
        HW_GPIO_OUT &= ~(EEP_CS | EEP_CLK | EEP_MOSI);
        tick();
        out[w * 2] = v >> 8;
        out[w * 2 + 1] = v & 0xFF;
    }

    HW_GPIO_OUT = (HW_GPIO_OUT & ~EEP_MASK) | (save_out & EEP_MASK);

    /* A bus that isn't answering reads back as all 0x00 or all 0xFF. */
    u8 all_or = 0, all_and = 0xFF;
    for (int i = 0; i < SEEPROM_SIZE; i++) {
        all_or |= out[i];
        all_and &= out[i];
    }
    return all_or != 0 && all_and != 0xFF;
}

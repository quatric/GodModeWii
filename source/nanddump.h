#ifndef NANDDUMP_H
#define NANDDUMP_H

#include <stdbool.h>

/* NAND backup the way nanddumper@IOS by thepikachugamer (https://github.com/Naim2000/nanddumper_ios) makes
 * it: a 1:1 BootMii-format nand.bin (pages with their spare / ECC data, then keys.bin) read through
 * /dev/flash, which IOS is patched to allow from inside IOS (mkwcat's /dev/sha exploit), with keys.bin and a
 * SHA-1 beside it in /wii/backups/ on the SD card or a USB drive. Or keys.bin alone.
 *
 * Shows its own menu, questions, progress and result. */
void nanddump_menu(void);

/* True while an IOS thread taken over for the backup idles in the first bytes of MEM1 (until the next IOS
 * reload): nothing may write there, so a disc can't be booted (its ID goes there). */
bool nanddump_ios_busy(void);

#endif

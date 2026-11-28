#ifndef DISCDUMP_H
#define DISCDUMP_H

/* Dump a disc to an ISO, the way CleanRip does: GameCube and Wii discs from the Wii's drive, and DVDs / CDs
 * (DVD-Video decrypted with libdvdcss when built WITH_DVDCSS=1) from the Wii's drive or a USB one. Unreadable
 * sectors are retried, then filled with 0x55 and logged, or stop the dump. Split into 4 GB parts on FAT32. */
void discdump_run(void);

/* Risky Mode: seeks and reads all over the disc to find a failing drive. */
void discdump_stress_test(void);

#endif

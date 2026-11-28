#ifndef SAVEMGR_H
#define SAVEMGR_H

/* The save manager: every Wii save on the NAND and every GameCube save on the memory cards, with their icons and
 * banners (after SaveGame Manager GX, GCMM and GerbilSoft's rom-properties). One save at a time can be looked at,
 * exported (data.bin, folder, .gci), copied, deleted (Risky Mode), or replaced from a data.bin / .gci found on the
 * SD card or a USB drive, also for a game that has never been started. Box / disc art from GameTDB. */
void savemgr_run(void);

#endif

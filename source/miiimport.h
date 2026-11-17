#ifndef MIIIMPORT_H
#define MIIIMPORT_H

#include <stdbool.h>

/* Puts Miis from files (.miigx / .mii / .rcd / .rsd, 74 bytes, or 76 with a CRC) into the Wii's Mii
 * database (nand:/shared2/menu/FaceLib/RFL_DB.dat) with MiiKit: either the Mii Channel's 100 plaza
 * slots, or the Mii Parade (the hidden database of Miis the Wii has met, up to 10,000). The old
 * database is copied to GodModeWii/mii-backups/ on the SD card or a USB drive first, and the NAND
 * needs the Konami code like any other change to it. */

/* Is this file name a Mii file? */
bool miiimport_is_name(const char *name);

/* One Mii file: asks where it goes (plaza or parade) and puts it there. */
void miiimport_file(const char *path);

/* Every Mii file in a folder and the folders in it (4 levels): asks where they go and puts them
 * there, the Mii Parade first. */
void miiimport_folder(const char *dir);

#endif

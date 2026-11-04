#ifndef WADINSTALL_H
#define WADINSTALL_H

/* Installs WAD packages to the NAND through ES, the way YAWM ModMii Edition
 * (https://github.com/modmii/YAWM-ModMii-Edition) does it, with its brick
 * checks. Save data is never deleted: nothing here removes a title, and an
 * installed title's data folder is backed up to the SD card (or a USB drive)
 * before the install and checked again afterwards.
 *
 * Shows its own confirmation, progress and result dialogs. Asks for the
 * Konami code first (risk_ensure_unlocked). */

/* Installs each of the `n` WADs at `paths` (one confirmation for all). */
void wad_install_paths(char **paths, int n);

#endif

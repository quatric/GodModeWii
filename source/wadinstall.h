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

/* Installs each of the `n` WADs at `paths` (one confirmation for all). Returns how many were
 * installed (0 when declined). */
int wad_install_paths(char **paths, int n);

/* Installs each WAD to the SD card instead (wad2bin): content.bin / <index>.bin made with this
 * console's keys, then the ticket and TMD on the NAND. Asks first (and which key for DLC).
 * Returns how many were installed. */
int wad_install_sd(char **paths, int n);

/* Installs each WAD into an emulated NAND on the SD card or a USB drive (neek2o / uneek / WiiFlow / Dolphin
 * layout, decrypted contents): asks which one first. The real NAND isn't touched. Returns how many went in. */
int wad_install_emunand(char **paths, int n);

/* True for a DLC WAD (title type 00010005). */
bool wad_is_dlc(const char *path);

/* Splits each DLC WAD into small WADs, as dlc-wad-splitter does (content 0 and one other each, the TMD
 * as it is: a song each for song DLC), into <SD or USB>/GodModeWii/wad-split/. Asks first, nothing more.
 * Returns how many WADs were split. */
int wad_split_paths(char **paths, int n);

/* This console's ID, NG key, PRNG key and device certificate (needs HW_AHBPROT off); false with `why`. */
#include "wad2bin.h"
bool wad_console_keys(w2b_keys_t *k, char *why, size_t wn);

/* Copies nand:/title/<hi>/<lo>/data to <SD or USB>/GodModeWii/save-backups/<tid>-<time>/data (where says
 * which). 1 when backed up, 0 when there is nothing to back up, -1 on failure. */
int wad_backup_save(u64 tid, char *where, size_t wsz, u32 *files_before);

#endif

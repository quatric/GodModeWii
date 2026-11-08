#ifndef DATABIN_H
#define DATABIN_H

#include <stdbool.h>
#include <stddef.h>
#include <gccore.h>

#include "wad2bin.h"

/* A title's save data (its NAND data folder) -> data.bin, the encrypted, signed backup the Wii's Data
 * Management screen makes and copies back from private/wii/title/<ID>/data.bin on the SD card. The layout
 * and signature follow Dolphin's WiiSave export: the header with the banner (SD key), the "Bk" header, every
 * file and folder under its own header (contents with the SD key), then an AP signature of SHA-1(SHA-1 of
 * the Bk header and files), the device certificate and the AP certificate. The banner's "no copy" flag is
 * cleared, as Dolphin does. */

/* `data_dir` is "nand:/title/<hi>/<lo>/data". 0 on success; otherwise err says why and nothing is left. */
int databin_export(u64 tid, const char *data_dir, const char *out_path, const w2b_keys_t *keys, char *err,
                   size_t errsz);

/* "private/wii/title/RMGE" for a title (its last four bytes as letters, as the System Menu names it). */
void databin_dir(u64 tid, char *out, size_t n);

#endif

#ifndef VIRT_H
#define VIRT_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* Read-only virtual drives:
 *   seeprom:/  seeprom.bin     the console's 256-byte SEEPROM
 *   disc:/     the files on the Wii / GameCube disc in the drive, browsable
 *              (plus sys/ with boot.bin, bi2.bin, apploader.img, fst.bin, main.dol
 *              and, for GameCube / unrecognised discs, a raw disc.iso)
 *              like any folder (GameCube FST, or the Wii data partition
 *              decrypted by IOS - no keys needed).
 * Neither exists on a Wii U's vWii, and both need HW_AHBPROT off (Homebrew
 * Channel). Paths passed in are relative to the drive root ("" = root). */

/* Re-detects the device; updates g_devs[dev].mounted and .note. */
void virt_refresh(int dev);

bool virt_list(int dev, const char *rel, vfs_entry **out, int *count);
bool virt_stat(int dev, const char *rel, bool *is_dir, u64 *size);
bool virt_open(vfs_file *f, int dev, const char *rel);
s32  virt_read(vfs_file *f, void *buf, u32 len);
bool virt_seek(vfs_file *f, u64 off);
void virt_close(vfs_file *f);
void virt_shutdown(void);

/* While on, unreadable disc sectors are zero-filled instead of failing the read,
 * and their byte ranges are collected (offsets relative to the file being read). */
void virt_disc_tolerate(bool on, bool (*cancel)(void));
u32  virt_disc_error_count(void);
u64  virt_disc_error_bytes(void);
bool virt_disc_error_overflow(void);
bool virt_disc_error_get(u32 i, u64 *file_off, u64 *disc_off, u64 *len);

#endif

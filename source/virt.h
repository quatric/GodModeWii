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
bool virt_disc_ejected(void);           /* the eject button pushed a disc out (once) */

bool virt_list(int dev, const char *rel, vfs_entry **out, int *count);
bool virt_stat(int dev, const char *rel, bool *is_dir, u64 *size);
bool virt_open(vfs_file *f, int dev, const char *rel);
s32  virt_read(vfs_file *f, void *buf, u32 len);
bool virt_seek(vfs_file *f, u64 off);
void virt_close(vfs_file *f);
void virt_shutdown(void);
/* At start-up and on remount: whether a disc is in, without spinning it (the motor is stopped);
 * it is read when the drive is opened. */
void virt_disc_peek(void);
/* Called every frame: stops the disc motor once nothing has read the disc for 20 s, and after a
 * disc was put in that nobody reads. */
void virt_disc_idle(void);

/* While on, unreadable disc sectors are zero-filled instead of failing the read,
 * and their byte ranges are collected (offsets relative to the file being read). */
void virt_disc_tolerate(bool on, bool (*cancel)(void));
u32  virt_disc_error_count(void);
u64  virt_disc_error_bytes(void);
bool virt_disc_error_overflow(void);
bool virt_disc_error_get(u32 i, u64 *file_off, u64 *disc_off, u64 *len);

/* The disc dumper's access: the whole disc in 2048-byte sectors, outside any partition. begin forgets what
 * was read in (the drive is read in again when next opened); each read is one try; dvd uses the DVD read
 * command a DVD-Video needs, otherwise the unencrypted read Nintendo discs take. buf is 32-byte aligned. */
bool virt_disc_raw_begin(void);
bool virt_disc_raw_read(u32 lba, u32 n, void *buf, bool dvd);
u32  virt_disc_raw_error(void);
void virt_disc_raw_reset(void);
void virt_disc_raw_stop(void);

#endif

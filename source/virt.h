#ifndef VIRT_H
#define VIRT_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* Read-only virtual drives:
 *   seeprom:/  seeprom.bin           the console's 256-byte SEEPROM
 *   disc:/     disc.iso, disc-info.txt   raw image of the disc in the drive
 * Neither exists on a Wii U's vWii, and both need HW_AHBPROT off (Homebrew
 * Channel). */

/* Re-detects the device; updates g_devs[dev].mounted and .note. */
void virt_refresh(int dev);

bool virt_list(int dev, vfs_entry **out, int *count);
bool virt_stat(int dev, const char *name, u64 *size);
bool virt_open(vfs_file *f, int dev, const char *name);
s32  virt_read(vfs_file *f, void *buf, u32 len);
bool virt_seek(vfs_file *f, u64 off);
void virt_close(vfs_file *f);
void virt_shutdown(void);

#endif

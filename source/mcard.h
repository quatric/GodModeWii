#ifndef MCARD_H
#define MCARD_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* GameCube memory cards in the console's Memory Card slots A / B (original
 * Wii only), shown as read-only virtual drives (mca:/ and mcb:/).
 *
 * Every save appears as one file named <company>-<gamecode>-<filename>.gci.
 * The file is a standard .gci: the 0x40 byte directory entry followed by the
 * save data, which is the format GCMM and Dolphin import / export, so
 * copying a save to an SD card or USB drive makes a GCMM-compatible backup. */

void mcard_refresh(int dev);             /* (re)mounts the card, sets .mounted / .note */
bool mcard_list(int dev, const char *rel, vfs_entry **out, int *count);
bool mcard_stat(int dev, const char *rel, bool *is_dir, u64 *size);
bool mcard_open(vfs_file *f, int dev, const char *rel);
s32  mcard_read(vfs_file *f, void *buf, u32 len);
void mcard_shutdown(void);

#endif

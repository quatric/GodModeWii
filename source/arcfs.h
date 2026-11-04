#ifndef ARCFS_H
#define ARCFS_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* Read-only view into archives, wired into the VFS with a '|' in the path:
 *   sd:/title.wad|/00000001.tik        a file inside a WAD
 *   sd:/a.arc.lz|/sound|/x.bin         archives nest
 * Supported: U8 (+ IMET / IMD5), LZ10 / LZ11, Camelot STPL, zlib / gzip, WAD, content.bin, data.bin, VFF, HSD .dat,
 * BootMii nand.bin, partition tables (MBR / GPT / Apple) and ext2/3/4, XFS, HFS+, APFS, ZFS volumes. */

bool arcfs_is_path(const char *path);                 /* has a '|' */
/* "x.wad|/a" -> "x.wad"; the archive file itself */
bool arcfs_container(const char *path, char *out, size_t outsz);
bool arcfs_can_open(const char *path);                /* a file that is one of the formats above */
const char *arcfs_error(void);

bool arcfs_list(const char *path, vfs_entry **out, int *count);
bool arcfs_stat(const char *path, bool *is_dir, u64 *size);
bool arcfs_open(vfs_file *f, const char *path);
s32  arcfs_read(vfs_file *f, void *buf, u32 len);
bool arcfs_seek(vfs_file *f, u64 off);
void arcfs_close(vfs_file *f);

/* a USB volume shown through the archive readers (WFS, or a disk's partitions / file system) */
bool arcfs_dev_pinned(int dev);
/* try to read the raw drive as a Wii U WFS volume, a partitioned disk, ext, XFS, HFS+, APFS or ZFS */
bool arcfs_mount_wfs(int dev, int usb_slot);
/* "WFS", "GPT", "ext4", ... of a pinned drive */
const char *arcfs_pin_kind(int dev);
void arcfs_unpin_all(void);                           /* before the USB handles are closed */

/* drop cached archives nobody is reading (after any change) */
void arcfs_flush(void);

#endif

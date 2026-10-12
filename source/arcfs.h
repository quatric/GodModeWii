#ifndef ARCFS_H
#define ARCFS_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* Read-only view into archives, wired into the VFS with a '|' in the path:
 *   sd:/title.wad|/00000001.tik        a file inside a WAD
 *   sd:/a.arc.lz|/sound|/x.bin         archives nest
 * Supported: U8 (+ IMET / IMD5), LZ10 / LZ11, WAD, content.bin, data.bin, VFF. */

bool arcfs_is_path(const char *path);                 /* has a '|' */
bool arcfs_container(const char *path, char *out, size_t outsz);   /* "x.wad|/a" -> "x.wad"; the archive file itself */
bool arcfs_can_open(const char *path);                /* a file that is one of the formats above */
const char *arcfs_error(void);

bool arcfs_list(const char *path, vfs_entry **out, int *count);
bool arcfs_stat(const char *path, bool *is_dir, u64 *size);
bool arcfs_open(vfs_file *f, const char *path);
s32  arcfs_read(vfs_file *f, void *buf, u32 len);
bool arcfs_seek(vfs_file *f, u64 off);
void arcfs_close(vfs_file *f);

void arcfs_flush(void);                               /* drop cached archives nobody is reading (after any change) */

#endif

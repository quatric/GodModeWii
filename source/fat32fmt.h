#ifndef FAT32FMT_H
#define FAT32FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <gctypes.h>

typedef struct {
    u32 ss, start, vol, spc, rsvd, fat, clusters;   /* sector size, partition start / length, sectors per cluster... */
} fat32_plan;

/* Where everything goes on a drive of `sectors` sectors of `ss` bytes; false if FAT32 can't fit it. */
bool fat32_layout(u64 sectors, u32 ss, fat32_plan *p);

/* Erases the drive's partition table and makes one FAT32 partition over it. Everything on the drive is lost. */
typedef bool (*fat32_write_fn)(void *io, u64 lba, u32 n, const void *buf);   /* buf is 64-byte aligned */
bool fat32_format(fat32_write_fn write, void *io, u64 sectors, u32 ss, const char *label,
                  void (*progress)(u64 done, u64 total, void *ctx), void *ctx, char *err, size_t en);

#endif

#ifndef VFS_H
#define VFS_H

#include <stdio.h>
#include <stdbool.h>
#include <gccore.h>

/* One flat namespace for every storage device:
 *   sd:/path      SD card              (libfat)
 *   gca:/ gcb:/   SD Gecko, slot A / B (libfat)
 *   usb1:/..usb8:/ USB mass storage    (libfat, one per drive / LUN)
 *   nand:/path    Wii NAND             (ISFS, 12 char names, risky to modify)
 *   seeprom:/     SEEPROM dump         (virtual, read-only)
 *   disc:/        files on the disc    (virtual, read-only)

 *   mca:/ mcb:/   GameCube memory cards, slot A / B (virtual, read-only, .gci saves)
 *   keys:/        console keys        (virtual, read-only, xyzzy-mod layout)
 */

#define USB_SLOTS 8

enum { DEV_SD, DEV_GCA, DEV_GCB, DEV_USB0, DEV_USB1, DEV_USB2, DEV_USB3, DEV_USB4, DEV_USB5, DEV_USB6, DEV_USB7, DEV_NAND, DEV_SEEPROM, DEV_DISC, DEV_MCA, DEV_MCB, DEV_KEYS, DEV_COUNT };

#define VFS_PATH_MAX 512
#define VFS_NAME_MAX 256
#define NAND_NAME_MAX 12

typedef struct {
    char name[VFS_NAME_MAX];
    bool is_dir;
    u64 size;
} vfs_entry;

typedef struct {
    int dev;
    FILE *fp;      /* FAT   */
    s32 fd;        /* NAND  */
    u64 size;
    int vkind;     /* virtual drives */
    u64 pos;
    u64 base;      /* start of the file inside the virtual drive */
    void *ctx;     /* memory card writes, archive entry */
    bool arc;      /* a file inside an archive (read-only) */
} vfs_file;

typedef struct {
    const char *prefix;  /* "sd:" */
    const char *root;    /* "sd:/" */
    const char *name;    /* "SD Card" */
    const char *tag;     /* "SD" */
    bool always_show;    /* listed on the home screen even when empty */
    bool mounted;
    bool space_known;
    u64 total;
    u64 free;
    char note[48];
    bool exfat;          /* no 4 GiB file limit */
    bool maybe_wiiu;     /* found but unreadable: possibly a Wii U drive without its keys */
    bool ntfs;           /* mounted with libntfs: no 4 GiB file limit */
    u8 ext;              /* 2, 3 or 4 for an ext volume, mounted read-only */
} vfs_dev;

extern vfs_dev g_devs[DEV_COUNT];
extern bool g_is_vwii;      /* running inside the Wii U's vWii */
extern bool g_is_dolphin;
extern bool g_nand_full;    /* ISFS permission patch applied (AHBPROT off) */
extern const char *g_write_err; /* why the last write / close failed, if a backend says so */

void vfs_init(void);
void vfs_shutdown(void);
void vfs_remount(void);                 /* re-detect SD / USB, refresh space */
void vfs_update_space(int dev);

int  vfs_dev_of(const char *path);      /* -1 if unknown */
bool vfs_is_virtual(int dev);           /* not FAT / NAND: SEEPROM, disc, memory cards, keys */
bool vfs_is_card(int dev);              /* GameCube memory card: .gci saves can be added / deleted */
bool vfs_is_readonly(const char *path);
bool vfs_is_archive(const char *path);  /* inside an archive: "sd:/a.wad|/x" */
const char *vfs_dev_root(int dev);      /* "sd:/" */
bool vfs_is_root(const char *path);     /* "sd:/" */
bool vfs_parent(const char *path, char *out, size_t outsz);
const char *vfs_basename(const char *path);
bool vfs_join(char *out, size_t outsz, const char *dir, const char *name);
bool vfs_is_within(const char *path, const char *dir); /* path == dir or below */

bool vfs_list(const char *path, vfs_entry **out, int *count);      /* everything, for copy / delete */
bool vfs_list_visible(const char *path, vfs_entry **out, int *count); /* without OS junk, for display */
bool vfs_is_hidden_name(const char *name);
bool vfs_stat(const char *path, bool *is_dir, u64 *size);
bool vfs_exists(const char *path);
bool vfs_mkdir(const char *path);
bool vfs_remove(const char *path, bool is_dir);   /* empty dirs only */
bool vfs_rename(const char *from, const char *to);

/* Raw sectors of a USB volume (for file systems libfat can't mount, like Wii U WFS). */
bool vfs_usb_raw_read(int slot, u64 off, void *buf, u32 len);
u64  vfs_usb_raw_size(int slot);

bool vfs_open_read(vfs_file *f, const char *path);
bool vfs_open_write(vfs_file *f, const char *path);   /* create / truncate */
void vfs_unbuffered(vfs_file *f);                     /* for big sequential copies */
s32  vfs_read(vfs_file *f, void *buf, u32 len);       /* buf 32-byte aligned */
s32  vfs_write(vfs_file *f, const void *buf, u32 len);
bool vfs_seek(vfs_file *f, u64 off);
void vfs_close(vfs_file *f);
bool vfs_close_checked(vfs_file *f);                  /* false if a card save was left incomplete */
void vfs_clone_attrs(const char *from, const char *to); /* NAND only, best effort */

#endif

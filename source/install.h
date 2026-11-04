#ifndef INSTALL_H
#define INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unpacks a .zip / .7z / .rar that was uploaded over FTP onto an SD card or a
 * USB drive, working out where the contents belong:
 *
 *   - archives that already hold the drive layout ("apps/...", "wiiu/...")
 *     are unpacked at the root of the drive, with any wrapper folder removed
 *   - a single folder that is a program (boot.dol, an .rpx ...) lands in its
 *     homebrew folder ("apps/<folder>" / "wiiu/apps/<folder>")
 *   - loose files are put in a new folder named after the archive
 *
 * `cb` is called while working (file name, bytes done, bytes total); return
 * false from it to cancel. */
typedef bool (*install_cb)(const char *file, uint64_t done, uint64_t total);

bool install_is_archive_name(const char *name);

/* Returns the number of files written, or -1 on failure with `err` filled in. */
int install_archive(const char *arc_path, int dev, install_cb cb, char *err, size_t errsz);

/* The same for programs only (wiiload): each folder holding a boot.dol / boot.elf (or another
 * .dol / .elf) goes into apps/<folder>, whatever folders wrap it in the archive ("apps/",
 * "MyApp-v1.2/apps/" ...); a program loose at the top goes into apps/<archive name>. Archives
 * without a program are placed as install_archive does it. */
int install_archive_apps(const char *arc_path, int dev, install_cb cb, char *err, size_t errsz);

/* The drive to install onto: first mounted USB drive for want_usb, else the
 * SD card. -1 if there is none. */
int install_pick_device(bool want_usb);

#endif

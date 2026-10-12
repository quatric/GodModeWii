#ifndef KEYS_H
#define KEYS_H

#include <stdbool.h>
#include <gccore.h>
#include "vfs.h"

/* "Console Keys" virtual drive. Besides one file per key it holds the same
 * files xyzzy-mod writes to /xyzzy/<console id>:
 *
 *   keys.txt            hex dump of every key, wad2bin format
 *   otp.bin             raw OTP dump
 *   seeprom.bin         raw SEEPROM dump
 *   bootmii_keys.bin    BootMii keys.bin layout
 *   device.cert         raw device certificate
 *   keys/<name>.bin     each key on its own, named like the keys.txt entries
 *
 * Original Wii only, needs HW_AHBPROT off. Keys are unique to the console:
 * keep the files private. */

void keys_refresh(void);               /* sets g_devs[DEV_KEYS].mounted / .note */
bool keys_list(const char *rel, vfs_entry **out, int *count);
bool keys_stat(const char *rel, bool *is_dir, u64 *size);
bool keys_open(vfs_file *f, const char *rel);
s32  keys_read(vfs_file *f, void *buf, u32 len);
void keys_shutdown(void);              /* wipes the keys from memory */

#endif

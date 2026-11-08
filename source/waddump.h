#ifndef WADDUMP_H
#define WADDUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <gccore.h>

/* Installed title -> installable WAD, the way Yet Another BlueDump MOD (YABDM) by DarkMatterCore makes
 * them: the certificate chain, the ticket, the TMD, every content encrypted again with the title key
 * (shared ones found through /shared1/content.map) and the first content as the footer.
 *
 * A source is a title folder on a NAND (nand:/title/<hi>/<lo>, or the same folder inside a BootMii
 * nand.bin) or a content.bin the System Menu wrote to the SD card (decrypted with this console's keys;
 * the ticket and any shared contents come from this console's NAND).
 *
 * The ticket is fakesigned (signature zeroed, padding brute-forced for the signing bug), its console ID
 * and ECDH data wiped and its common key index set to 0 (a Korean or vWii title key is encrypted again
 * with the Wii common key), so it installs on any Wii. The TMD is left exactly as Nintendo signed it. */

/* True when `path` is something a WAD can be made from; tid (may be NULL) gets its title ID when known
 * from the path alone (0 for a content.bin). */
bool waddump_is_source(const char *path, u64 *tid);

/* Makes `out` (a file path) from `src`. progress may be NULL; it returning false cancels. 0 on success,
 * otherwise err says why and no file is left behind. */
typedef bool (*waddump_progress_fn)(void *ctx, const char *what, u64 done, u64 total);
int waddump_build(const char *src, const char *out, char *err, size_t errsz, waddump_progress_fn progress,
                  void *ctx);

/* The file name for a title: "00010001-48414241-HABA-v2.wad". */
void waddump_name(u64 tid, u16 version, char *out, size_t n);

/* The browser's action: makes a WAD from each source in paths[0..n) on a drive the user picks
 * (<drive>/GodModeWii/wad-dump/). Asks first and shows the outcome. Returns how many were made. */
int waddump_paths(char **paths, int n);

#endif

#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

/* Wii Menu themes. DANGEROUS: a theme replaces the System Menu's layout archive on the NAND, and a
 * bad one stops the Wii Menu from starting. With the checks of Naim2000's csm-installer
 * (https://github.com/Naim2000/csm-installer):
 *   - the System Menu is identified from its TMD (version, region, Wii / vWii / Wii mini) and its
 *     layout archive found by content
 *   - Priiloader must be installed (it can still start homebrew when the Wii Menu doesn't)
 *   - the theme must be a U8 archive that holds the menu's files, this region's iplsetting.ash and
 *     main.sel, and main.sel must name the same System Menu major version and platform
 *   - vWii: the WiiWare 4:3 database is patched for the WiiConnect24 channels, as csm-installer
 *     does by default
 *   - the stock archive is saved to <SD / USB>:/GodModeWii/themes/original/ (hash checked against
 *     the TMD) and an installed custom theme to .../themes/backup/ before anything is written; no
 *     backup, no install
 *   - the new archive is written to /tmp, read back and hash checked, and only then moved over
 *     the old one, then checked again
 * A .mym is first built into a .csm from the stock archive (mym.c), for System Menu 4.3 on a Wii
 * only, and that csm goes through the same checks. Shows its own dialogs; asks for the Konami
 * code (risk_ensure_unlocked). */

/* A file the theme installer takes, by name: .csm, .mym. */
bool theme_is_name(const char *name);
bool theme_is_mym_name(const char *name);

/* Installs the .csm (or the stock .app) or .mym at `path` as the Wii Menu's theme. */
void theme_install(const char *path);
/* Builds a .csm from the .mym at `path` and saves it to the SD card (or a USB drive); the NAND is
 * only read. */
void theme_convert(const char *path);

#endif

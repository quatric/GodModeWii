#ifndef HBCREBORN_H
#define HBCREBORN_H

/* Installs HBC Reborn's latest release from GitHub: the installed Homebrew Channel is replaced in place,
 * under its own title ID (LULZ, OHBC, or an older JODI / HAXX); with none, it asks which. The WAD is
 * checked against the release's SHA256SUMS, kept in GodModeWii/hbc-reborn/, and installed with the usual
 * WAD installer (the Konami code, its checks, save data kept). */
void hbcreborn_install(void);

#endif

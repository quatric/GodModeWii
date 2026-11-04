#ifndef WIILOAD_H
#define WIILOAD_H

#include <stdbool.h>
#include <stddef.h>

/* A wiiload receiver on TCP port 4299, the Homebrew Channel's, so the usual client works:
 *
 *     wiiload MyApp.zip
 *
 * A .zip / .7z / .rar sent to it is installed onto the SD card (or a USB drive) the way the FTP
 * install server does it (install.h): "apps/MyApp/boot.dol" lands in apps, a lone program folder
 * goes into apps/<folder>. A .dol / .elf can be started right away (after asking). Anything else
 * is kept in GodModeWii/wiiload/ on that drive.
 *
 * Runs on its own screen until B / HOME. Returns true with `launch` filled in when the user chose
 * to start a program that was sent (the caller then launches it), false otherwise. */
bool wiiload_run(char *launch, size_t launchsz);

#endif

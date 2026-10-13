#ifndef FTP_H
#define FTP_H

#include <stdbool.h>

/* Runs the FTP server on its own screen until the user stops it. Everything
 * is single threaded and non-blocking, so it never races with the file
 * browser. `read_write` = false serves read-only. Returns false if the
 * network or the server could not be started. */
enum { FTP_INSTALL_OFF, FTP_INSTALL_SD, FTP_INSTALL_USB };

/* `install_mode` != FTP_INSTALL_OFF makes the server read / write and unpacks
 * every .zip / .7z / .rar uploaded to it onto the SD card or the USB drive
 * (see install.h), then deletes the archive. */
bool ftp_run(bool read_write, int install_mode);

void ftp_net_shutdown(void);

#endif

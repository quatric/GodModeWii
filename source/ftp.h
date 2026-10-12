#ifndef FTP_H
#define FTP_H

#include <stdbool.h>

/* Runs the FTP server on its own screen until the user stops it. Everything
 * is single threaded and non-blocking, so it never races with the file
 * browser. `read_write` = false serves read-only. Returns false if the
 * network or the server could not be started. */
bool ftp_run(bool read_write);

void ftp_net_shutdown(void);

#endif

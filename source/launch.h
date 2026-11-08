#ifndef LAUNCH_H
#define LAUNCH_H

#include <stdbool.h>

/* Starting homebrew (.dol / .elf) from the file browser, the way YAWM ModMii
 * Edition (https://github.com/modmii/YAWM-ModMii-Edition) does it: the
 * program is read into memory, everything is shut down, and YAWM's small
 * app booter (appboot/) copies it into place and jumps to it. Arguments from
 * a meta.xml next to it are passed on, after the program's own path. */

/* Reads the program and its arguments. False (after showing why) if it can't
 * be started. On success the caller leaves the main loop with EXIT_LAUNCH. */
bool launch_prepare(const char *path);

/* A program built into this one (its bytes, and the name to pass as its own path). */
bool launch_prepare_memory(const unsigned char *data, unsigned size, const char *name);

/* Called last, after the drives and controllers are shut down. Doesn't return. */
void launch_go(void);

#endif

#ifndef RISK_H
#define RISK_H

#include <stdbool.h>

/* Anything that changes the Wii NAND can brick the console, so the first
 * attempt shows a warning and asks for the Konami code. The code is asked for once per
 * session. There is no menu to visit first: the prompt
 * appears when you try the risky thing (delete, paste, rename, ...). */

bool risk_unlocked(void);
void risk_lock(void);
/* Warning + Konami code if the NAND is not unlocked yet. */
bool risk_ensure_unlocked(void);

/* Gate for any modifying operation on `path`: asks for the Konami code when
 * needed. Returns false if the user cancelled. */
bool risk_authorize(const char *path, const char *what);

#endif

#ifndef RISK_H
#define RISK_H

#include <stdbool.h>
#include <gccore.h>

/* Anything that changes the Wii NAND can brick the console, so the first
 * attempt shows a warning and asks for the Konami code. The code is asked for once per
 * session. There is no menu to visit first: the prompt
 * appears when you try the risky thing (delete, paste, rename, ...). */

bool risk_unlocked(void);
bool risk_forced(void);
void risk_lock(void);
/* Warning + Konami code if the NAND is not unlocked yet. */
bool risk_ensure_unlocked(void);

/* Gate for any modifying operation on `path`: asks for the Konami code when
 * needed. Returns false if the user cancelled. */
bool risk_authorize(const char *path, const char *what);

/* Startup-only helpers (see main.c). */
void risk_load_forced(void);
void risk_startup_poll_begin(void);
bool risk_startup_poll_end(u32 min_ms, u64 began_ms);
bool risk_toggle_forced(void);

#endif

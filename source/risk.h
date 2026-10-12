#ifndef RISK_H
#define RISK_H

#include <stdbool.h>

/* Anything that writes to the Wii NAND can brick the console, so it stays
 * locked until the user has read the disclaimer and typed the Konami code. */

bool risk_unlocked(void);
void risk_lock(void);
bool risk_unlock_flow(void);

/* True for NAND areas where a mistake means a brick (IOS, System Menu...). */
bool risk_is_critical(const char *path);

/* Gate for any modifying operation on `path`. Shows the appropriate
 * message and returns false when the action must not go ahead. */
bool risk_authorize(const char *path, const char *what);

#endif

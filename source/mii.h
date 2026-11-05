#ifndef MII_H
#define MII_H

#include <stdbool.h>

/* Lists the Wii's Miis with their faces (drawn by Arian Kordi's Mii renderer) and saves the marked ones (or the
 * highlighted one) to SD / USB. */
void mii_export(void);

/* A Mii file (.miigx / .mii / .rcd / .rsd): its face and details; an RFL_DB.dat: the list. */
bool mii_view(const char *path);

#endif

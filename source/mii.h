#ifndef MII_H
#define MII_H

#include <stdbool.h>

/* Lists the Wii's Miis, each drawn by MiiKit, and saves the marked ones (or the highlighted one) to
 * SD / USB. */
void mii_export(void);

/* A Mii file (.miigx / .mii / .rcd / .rsd): its face and details; an RFL_DB.dat: the list. */
bool mii_file_view(const char *path);

#endif

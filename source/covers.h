#ifndef COVERS_H
#define COVERS_H

#include <stdbool.h>

/* Shows a game's box art (disc: the disc's label art instead) from GameTDB (art.gametdb.com), downloaded once and
 * kept in GodModeWii/covers/ on the SD card or a USB drive. id6: the six-character game ID. False if there is none. */
bool covers_show(const char *id6, bool disc);

#endif

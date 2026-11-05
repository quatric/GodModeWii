#ifndef USAGE_H
#define USAGE_H

/* An ncdu-like disk usage browser: the folder's entries sorted by total size, each with a bar of
 * its share. A opens a folder, B goes up (and leaves at the top), 1 flips size / name order,
 * 2 deletes. Returns when the user leaves it; the caller redraws. */
void usage_browse(const char *dir);

#endif

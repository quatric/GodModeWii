#ifndef ZIPWRITE_H
#define ZIPWRITE_H

#include <stdbool.h>
#include <stddef.h>

/* The files and folders (with everything in them) as a new .zip at zip_path, with a progress bar; hold B to stop.
 * False with the reason in err; a part-made zip is removed. */
bool zip_create(char **paths, int n, const char *zip_path, char *err, size_t en);

#endif

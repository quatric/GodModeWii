#ifndef SEARCH_H
#define SEARCH_H

#include <stdbool.h>
#include <stddef.h>

/* Asks for a name, searches everything under `root`, lists what matched. True and the path picked in `out`. */
bool file_search(const char *root, char *out, size_t outsz);

#endif

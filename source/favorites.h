#ifndef FAVORITES_H
#define FAVORITES_H

#include <stdbool.h>
#include <stddef.h>

#define FAV_MAX 16

/* The program a path stands for: a .dol / .elf itself, or an app folder's boot.dol / boot.elf. */
bool fav_program(const char *path, char *prog, size_t n);

/* Read again on next use (the SD card may have been swapped or mounted since). */
void fav_reload(void);
int fav_count(void);
const char *fav_path(int i);            /* the program's path */
const char *fav_name(int i);            /* its meta.xml name, or its folder's */
int fav_find(const char *prog);         /* -1 if it isn't one */
bool fav_add(const char *prog);         /* false: the list is full or couldn't be saved */
bool fav_remove(int i);

#endif

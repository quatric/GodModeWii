#ifndef OPS_H
#define OPS_H

#include <stdbool.h>
#include <gccore.h>

/* Copy / move / delete trees with a progress screen. Each returns true if
 * the operation ran to completion without being cancelled or failing. */
bool ops_paste(char **srcs, int n, bool cut, const char *dst_dir);
bool ops_delete(char **srcs, int n);
/* Copies srcs[i] into the existing folder dst_dirs[i] (same name), all in one
 * progress screen and one summary. No risk prompt: for copies off the NAND. */
bool ops_copy_each(char **srcs, char **dst_dirs, int n, const char *title);

/* Recursive size of a path (files and folders counted separately). */
bool ops_tree_info(const char *path, u64 *bytes, u32 *files, u32 *dirs);

#endif

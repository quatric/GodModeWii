#ifndef MC_IO_H
#define MC_IO_H
/* GodModeWii: open any vfs path (SD, USB, NAND, disc, inside an archive)
 * as a read-only stdio FILE so the decoders can keep using fread/fseek. */
#include <stdio.h>
FILE *mc_fopen(const char *path);
#endif

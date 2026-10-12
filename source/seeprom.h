#ifndef SEEPROM_H
#define SEEPROM_H

#include <stdbool.h>
#include <gccore.h>

#define SEEPROM_SIZE 256

/* Reads the whole 93C56-style SEEPROM (128 x 16 bit words, big endian). */
bool seeprom_read(u8 out[SEEPROM_SIZE]);

#endif

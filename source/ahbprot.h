#ifndef AHBPROT_H
#define AHBPROT_H

#include <stdbool.h>

/* HW_AHBPROT off (the PPC gets the hardware), through /dev/sha when the loader left it on. True when it
 * is off. Not for Dolphin. */
bool ahbprot_disable(void);

#endif

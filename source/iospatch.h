/*
 * GodModeWii - patches the running IOS in MEM2 (needs HW_AHBPROT off).
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 */
#ifndef IOSPATCH_H
#define IOSPATCH_H

#include <gctypes.h>

/* the PPC can reach the Starlet's registers */
#define AHBPROT_DISABLED (*(vu32 *)0xCD800064 == 0xFFFFFFFF)

/* Each returns how many places it patched, or -1 without HW_AHBPROT access. */
int iospatch_ahbprot(void);          /* ES keeps HW_AHBPROT off across an IOS reload or a disc TMD */
int iospatch_isfs(void);             /* ISFS: every file is readable/writable */
int iospatch_di_readlimit(void);     /* DI: read past the normal disc limit */
int iospatch_wad_install(bool vwii); /* ES: fakesigned and older titles (and system titles on vWii) */

#endif

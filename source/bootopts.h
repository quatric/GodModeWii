#ifndef BOOTOPTS_H
#define BOOTOPTS_H

#include <gctypes.h>

/* Leaving GodModeWii for BootMii or Priiloader. */
bool bootmii_ios_present(void);        /* BootMii installed as IOS254 (not the stub IOS updates leave there) */
bool priiloader_present(void);         /* Priiloader installed in front of the Wii Menu */
void priiloader_magic(bool menu);      /* the next Wii Menu start: Priiloader's menu, or straight to the Wii Menu */
void bootmii_boot(void);               /* reloads into BootMii; only comes back if it couldn't */

#endif

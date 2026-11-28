#ifndef MII3DS_H
#define MII3DS_H

#include <stdint.h>

#include "miikit/mii.h"

#define MII3DS_SIZE    0x60    /* Ver3StoreData, as the 3DS and Wii U keep a Mii */
#define MII3DS_QR_SIZE 0x70    /* nonce (8) + encrypted Mii (0x58) + tag (16) */

void mii_to_ver3(const mii *m, uint8_t out[MII3DS_SIZE]);
/* What a 3DS / Wii U Mii QR code holds. */
void mii_qr_payload(const mii *m, uint8_t out[MII3DS_QR_SIZE]);

#endif

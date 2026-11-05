#ifndef MIIFACE_H
#define MIIFACE_H

/* Mii faces, rendered online by Arian Kordi's Mii renderer (mii-unsecure.ariankordi.net), the one
 * PictoCard's Mii faces come from: each 74-byte Mii record is sent and a PNG of its face comes
 * back. Faces are kept for the session, so a Mii is only sent once. */

#include <stdbool.h>
#include <stddef.h>
#include <gccore.h>

#define MIIFACE_SIZE 192                /* pixels, square */
#define MIIFACE_HOST "mii-unsecure.ariankordi.net"

typedef struct {
    u8 *rgba;                           /* MIIFACE_SIZE x MIIFACE_SIZE, or NULL: not drawn */
} miiface_t;

/* Faces for n Mii records (74 bytes each, recs[i]). Brings the network up first (B leaves).
 * progress (may be NULL) is told how many are done; returning false from it stops. The faces
 * stay owned by the cache: don't free them. False with err set when nothing could be fetched;
 * Miis the server couldn't draw just get no face. */
bool miiface_fetch(const u8 *const *recs, int n, const miiface_t **faces,
                   bool (*progress)(void *ctx, int done, int total), void *ctx, char *err, size_t errsz);

/* A face already in the cache (NULL if it wasn't fetched). */
const miiface_t *miiface_cached(const u8 *rec);

/* Draws a face onto the screen at framebuffer pixel (x, y), each pixel scale x scale, over bg_rgb
 * (0xRRGGBB). */
void miiface_draw(const miiface_t *f, int x, int y, int scale, u32 bg_rgb);

#endif

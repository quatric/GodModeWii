#ifndef MIIFACE_H
#define MIIFACE_H

/* Miis drawn on the Wii itself by MiiKit (source/miikit), head and shoulders, from the Wii Menu's
 * face models and the Mii Channel's bodies (read from the NAND, or from sd:/mii/ when the NAND can't be
 * read). */

#include <stdbool.h>
#include <stddef.h>
#include <gccore.h>

#define MIIFACE_WIDTH 192               /* pixels; both multiples of 4 */
#define MIIFACE_HEIGHT 192
#define MIIFACE_EXPRESSIONS 7

/* Loads the face models and bodies. False with err set when there are no face models; the Miis
 * then go without pictures. Call miiface_close after, either way. */
bool miiface_open(char *err, size_t errsz);
void miiface_close(void);

/* Draws a 74-byte Mii record's Mii, head and shoulders, in an expression (0 to MIIFACE_EXPRESSIONS - 1),
 * at framebuffer pixel (x, y) on black. Pictures are kept until miiface_close, so a Mii is drawn
 * by the GPU only once an expression. False when there's nothing to show: the area is left
 * black. */
bool miiface_draw(const u8 *rec, int expression, int x, int y);

/* "Normal", "Smile", ... */
const char *miiface_expression_name(int expression);

/* True once a picture came back empty in Dolphin, whose GPU keeps its copies to itself unless
 * "Store EFB Copies to Texture Only" is off. */
bool miiface_dolphin_blank(void);

#endif

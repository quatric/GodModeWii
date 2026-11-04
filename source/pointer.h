#ifndef POINTER_H
#define POINTER_H

/* The Wii Remote pointer (or a USB mouse) in the image and 3D model viewers: an arrow drawn into
 * the console framebuffer, and dragging with A held. libwiiinput tracks the cursor. */

#include <stdbool.h>
#include <gccore.h>

/* Call when a viewer opens: matches the cursor's range to the framebuffer. */
void ptr_begin(void);
/* Where the cursor is, in framebuffer pixels; false while nothing points at the screen. */
bool ptr_get(int *x, int *y);
/* Draws the arrow at the cursor (or takes it away when nothing points at the screen), keeping
 * what was under it. */
void ptr_show(void);
/* Puts back what was under the arrow: before redrawing the screen under it. */
void ptr_hide(void);

/* A held with the pointer on screen. Feed it every frame with the held buttons. */
typedef struct { bool down, dragged, ptr_press; int x0, y0, lx, ly; } ptr_drag;
enum { PD_NONE, PD_MOVE, PD_TAP };
/* PD_MOVE while dragging (*dx / *dy: the movement this frame), PD_TAP when A was let go without
 * dragging. A pressed while nothing points at the screen is left to the caller (PD_NONE). */
int ptr_drag_update(ptr_drag *d, u32 held, int *dx, int *dy);
/* True while A is held for a drag that started on screen: the caller skips its own A press. */
static inline bool ptr_drag_owns_a(const ptr_drag *d) { return d->ptr_press; }

/* Mouse wheel steps since the last poll (positive: toward the user). */
int ptr_wheel(void);

#endif

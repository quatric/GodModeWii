/* The public header of this library, so the shared sources are the same file
 * in libwiiir and libwiiuir. */
#include "ir_wiiu.h"

/* ir_sir.c: timings to IrDA SIR bytes at `rate` bit/s. Returns the byte count,
 * 0 when they do not fit in `max`. */
size_t ir_sir_encode(const uint32_t *timings, size_t count, uint32_t rate,
                     uint8_t *out, size_t max);

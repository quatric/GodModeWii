#ifndef IRREMOTE_H
#define IRREMOTE_H
#include <stdbool.h>

/* The IR remote: TV / AV codes sent with any controller's buttons, through
 * presets kept on the SD card. quick: straight to the last preset in use. */
void irremote_run(bool quick);

/* True while 1 (Y on the GamePad, Classic and GameCube pads) is held: the
 * start-up shortcut to the remote. */
bool irremote_shortcut_held(void);

#endif

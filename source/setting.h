#ifndef SETTING_H
#define SETTING_H

#include <stdbool.h>
#include <stddef.h>

/* The System Menu's setting.txt (nand:/title/00000001/00000002/data/setting.txt): 0x100 bytes of
 * "KEY=VALUE\r\n" lines (AREA, MODEL, DVD, MPCH, CODE, SERNO, VIDEO, GAME), XORed with a rolling key. */

#define SETTING_SIZE 0x100

enum { MODEL_UNKNOWN, MODEL_WII, MODEL_FAMILY, MODEL_MINI, MODEL_NDEV, MODEL_VWII };

/* The decrypted text, read once and kept (setting_reload reads it again). False if it can't be read. */
bool setting_text(const char **text);
void setting_reload(void);
/* One value ("MODEL" -> "RVL-001(USA)"); false when missing or longer than n - 1. */
bool setting_get(const char *key, char *out, size_t n);
/* What console this is: from MODEL, or vWii from the console ID. */
int setting_model(void);
const char *setting_model_name(int model);
/* Wii mini (RVL-201): no SD card slot, no Wi-Fi, no GameCube ports. */
bool setting_is_mini(void);

/* Lists the fields and lets them be changed (backup first, the Konami code, then a checked write). */
void setting_editor(void);

#endif

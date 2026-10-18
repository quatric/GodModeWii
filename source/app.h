#ifndef APP_H
#define APP_H

#include <stdbool.h>

#define GMW_VERSION "0.6.0"

enum { EXIT_LOADER, EXIT_MENU, EXIT_POWEROFF };

extern volatile bool g_quit_request;
extern volatile bool g_power_request;

/* Runs the interface until the user leaves; returns an EXIT_* code. */
int app_run(void);

#endif

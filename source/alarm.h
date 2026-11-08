#ifndef ALARM_H
#define ALARM_H

#include <stdbool.h>

/* An alarm clock, after MoonShell's Morning Timer: at the set time the sound and the disc slot light go
 * off until it's stopped (or snoozed). The time is kept on the card (sd:/apps/GodModeWii/alarm). */

void alarm_load(void);
/* Polled with every input poll: starts the ringing at the set time and keeps it going. */
void alarm_poll(void);
bool alarm_ringing(void);
void alarm_stop(void);
void alarm_snooze(int minutes);
/* The screen that sets it. */
void alarm_menu(void);

#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "alarm.h"
#include "input.h"
#include "pointer.h"
#include "sfx.h"
#include "ui.h"
#include "wiilight.h"

#define SETTING "sd:/apps/GodModeWii/alarm"
#define RING_MAX_MS (5 * 60 * 1000)

static struct { bool on; int h, m; } al = { false, 7, 0 };
static bool ringing;
static bool snoozing;
static int snooze_h, snooze_m;
static long last_minute = -1;               /* the minute it last went off in (days * 1440 + minutes) */
static u64 ring_began, last_sound, last_light, last_check;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static void save(void)
{
    mkdir("sd:/apps", 0777);
    mkdir("sd:/apps/GodModeWii", 0777);
    FILE *f = fopen(SETTING, "wb");
    if (!f) return;
    fprintf(f, "%d %d %d\n", al.on ? 1 : 0, al.h, al.m);
    fclose(f);
}

void alarm_load(void)
{
    FILE *f = fopen(SETTING, "rb");
    if (!f) return;
    int on, h, m;
    if (fscanf(f, "%d %d %d", &on, &h, &m) == 3 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        al.on = on != 0;
        al.h = h;
        al.m = m;
    }
    fclose(f);
}

bool alarm_ringing(void) { return ringing; }

void alarm_stop(void)
{
    ringing = false;
    wiilight_off();
}

void alarm_snooze(int minutes)
{
    time_t t = time(NULL) + minutes * 60;
    struct tm tmv;
    alarm_stop();
    if (!localtime_r(&t, &tmv)) return;
    snoozing = true;
    snooze_h = tmv.tm_hour;
    snooze_m = tmv.tm_min;
}

static void start_ringing(void)
{
    ringing = true;
    ring_began = now_ms();
    last_sound = last_light = 0;
}

void alarm_poll(void)
{
    u64 now = now_ms();
    if (ringing) {
        if (now - ring_began > RING_MAX_MS) {
            alarm_stop();
            return;
        }
        if (now - last_sound >= 1300) {
            last_sound = now;
            sfx_play_force(SFX_NOTIFY);
        }
        if (now - last_light >= 4000) {
            last_light = now;
            wiilight_pattern(WL_BUSH_WARBLER);
        }
        return;
    }
    if ((!al.on && !snoozing) || now - last_check < 1000) return;
    last_check = now;
    time_t t = time(NULL);
    struct tm tmv;
    if (t < 1000000000 || !localtime_r(&t, &tmv)) return;
    long minute = (long)(t / 86400) * 1440 + tmv.tm_hour * 60 + tmv.tm_min;
    if (minute == last_minute) return;
    if ((al.on && tmv.tm_hour == al.h && tmv.tm_min == al.m) ||
        (snoozing && tmv.tm_hour == snooze_h && tmv.tm_min == snooze_m)) {
        last_minute = minute;
        snoozing = false;
        start_ringing();
    }
}

static void draw(int field)
{
    ui_clear();
    ui_color(C_CYAN, true);
    ui_field(0, 1, ui_cols() - 2, "Alarm clock");
    ui_reset();
    ui_color(C_WHITE, false);
    ui_field(2, 2, ui_cols() - 4, "At the set time the sound and the disc slot light go off.");
    ui_reset();

    for (int f = 0; f < 3; f++) {
        char text[24];
        if (f == 0) snprintf(text, sizeof(text), " %02d ", al.h);
        else if (f == 1) snprintf(text, sizeof(text), " %02d ", al.m);
        else snprintf(text, sizeof(text), " %s ", al.on ? "ON " : "OFF");
        int col = f == 0 ? 6 : f == 1 ? 13 : 22;
        if (f == field) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else {
            ui_color(al.on || f < 2 ? C_WHITE : C_RED, true);
        }
        ui_field(5, col, 6, "%s", text);
        ui_reset();
    }
    ui_color(C_WHITE, true);
    ui_field(5, 11, 2, ":");
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, ui_cols() - 2, "L/R Pick  U/D Change  +/- Jump  A On/Off  1 Test  B Back");
    ui_reset();
}

void alarm_menu(void)
{
    int field = 0;
    draw(field);
    for (;;) {
        u32 p = input_pressed() | ptr_ui_buttons();
        int step = 0;
        if (p & WPAD_BUTTON_UP) step = 1;
        if (p & WPAD_BUTTON_DOWN) step = -1;
        if (p & WPAD_BUTTON_PLUS) step = 5;
        if (p & WPAD_BUTTON_MINUS) step = -5;
        if (p & WPAD_BUTTON_LEFT) field = (field + 2) % 3;
        if (p & WPAD_BUTTON_RIGHT) field = (field + 1) % 3;
        if (step) {
            if (field == 0) al.h = ((al.h + step) % 24 + 24) % 24;
            else if (field == 1) al.m = ((al.m + step) % 60 + 60) % 60;
            else al.on = !al.on;
        }
        if (p & WPAD_BUTTON_A) al.on = !al.on;
        if (p & WPAD_BUTTON_1) start_ringing();
        if (p & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN |
                 WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS | WPAD_BUTTON_A))
            draw(field);
        if (ringing) {                                  /* a test: A or B stops it */
            if (p & (WPAD_BUTTON_A | WPAD_BUTTON_B)) {
                alarm_stop();
                p &= ~WPAD_BUTTON_B;
            }
        }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
    }
    alarm_stop();
    save();
    ui_clear();
}

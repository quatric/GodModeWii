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
static const int SLEEP_MIN[] = { 0, 15, 30, 60, 90, -1 };   /* 0 is off, -1 is on until stopped */
#define NSLEEP ((int)(sizeof(SLEEP_MIN) / sizeof(SLEEP_MIN[0])))
static int sleep_idx;                       /* sleep noise length: not kept, so it's never on at start-up */
static u64 sleep_end;                       /* when the sleep noise stops (ms), 0 when it isn't playing */
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
    int on, h, m;                                   /* an older file has a 4th number: ignored */
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
    sfx_noise_stop();
    sleep_end = 0;
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
    sfx_noise_stop();                                   /* the sleep noise gives way to the beep */
    sleep_end = 0;
    sleep_idx = 0;
}

static void sleep_apply(void)
{
    if (SLEEP_MIN[sleep_idx]) {
        sleep_end = SLEEP_MIN[sleep_idx] > 0 ? now_ms() + (u64)SLEEP_MIN[sleep_idx] * 60000 : 0;
        sfx_noise_start();                              /* no end time: it plays until it's turned off */
    } else {
        sleep_end = 0;
        sfx_noise_stop();
    }
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
    if (sleep_end && now >= sleep_end) {
        sfx_noise_stop();
        sleep_end = 0;
        sleep_idx = 0;
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

#define NFIELDS 5

static void draw(int field)
{
    ui_clear();
    ui_color(C_CYAN, true);
    ui_field(0, 1, ui_cols() - 2, "Alarm clock");
    ui_reset();
    ui_color(C_WHITE, false);
    ui_field(2, 2, ui_cols() - 4, "At the set time, a beep and the disc slot light go off.");
    ui_field(3, 2, ui_cols() - 4, "Sleep plays white noise now: for a while, or until you stop it.");
    ui_reset();

    int h12 = al.h % 12 ? al.h % 12 : 12;
    for (int f = 0; f < NFIELDS; f++) {
        char text[24];
        if (f == 0) snprintf(text, sizeof(text), " %2d ", h12);
        else if (f == 1) snprintf(text, sizeof(text), " %02d ", al.m);
        else if (f == 2) snprintf(text, sizeof(text), " %s ", al.h < 12 ? "AM" : "PM");
        else if (f == 3) snprintf(text, sizeof(text), " %s ", al.on ? "ON " : "OFF");
        else if (SLEEP_MIN[sleep_idx] > 0) snprintf(text, sizeof(text), " Sleep %2dm ", SLEEP_MIN[sleep_idx]);
        else if (SLEEP_MIN[sleep_idx] < 0) snprintf(text, sizeof(text), " Sleep on  ");
        else snprintf(text, sizeof(text), " Sleep off ");
        static const int COL[NFIELDS] = { 6, 13, 19, 26, 33 };
        if (f == field) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else {
            ui_color(f == 3 && !al.on ? C_RED : C_WHITE, true);
        }
        ui_field(5, COL[f], f == 4 ? 11 : 5, "%s", text);
        ui_reset();
    }
    ui_color(C_WHITE, true);
    ui_field(5, 10, 3, ":");
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
        UI_TEST_SHOT();
        u32 p = input_pressed() | ptr_ui_buttons();
        int step = 0;
        if (p & WPAD_BUTTON_UP) step = 1;
        if (p & WPAD_BUTTON_DOWN) step = -1;
        if (p & WPAD_BUTTON_PLUS) step = 5;
        if (p & WPAD_BUTTON_MINUS) step = -5;
        if (p & WPAD_BUTTON_LEFT) field = (field + NFIELDS - 1) % NFIELDS;
        if (p & WPAD_BUTTON_RIGHT) field = (field + 1) % NFIELDS;
        if (step) {
            if (field == 0)                         /* 12-hour: round the AM or the PM half */
                al.h = (((al.h % 12) + step) % 12 + 12) % 12 + (al.h >= 12 ? 12 : 0);
            else if (field == 1) al.m = ((al.m + step) % 60 + 60) % 60;
            else if (field == 2) al.h = (al.h + 12) % 24;
            else if (field == 3) al.on = !al.on;
            else {
                int d = step > 0 ? 1 : -1;
                sleep_idx = (sleep_idx + d + NSLEEP) % NSLEEP;
                sleep_apply();
            }
        }
        if (ringing && (p & (WPAD_BUTTON_A | WPAD_BUTTON_B))) {     /* a test: A or B stops it, nothing more */
            alarm_stop();
            p &= ~(WPAD_BUTTON_A | WPAD_BUTTON_B);
            draw(field);
        }
        if (p & WPAD_BUTTON_A) al.on = !al.on;
        if (p & WPAD_BUTTON_1) start_ringing();
        if (p & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN |
                 WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS | WPAD_BUTTON_A))
            draw(field);
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
    }
    if (ringing) alarm_stop();                      /* the sleep noise carries on after the menu */
    save();
    ui_clear();
}

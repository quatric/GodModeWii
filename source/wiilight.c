#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "wiilight.h"
#include "vfs.h"
#include "wiilight_patterns.h"

#define SETTING "sd:/apps/GodModeWii/light_off"
#define PERIOD_US 10200                     /* one switching period, as GlowWii's */

enum { M_OFF, M_GLOW, M_PULSE, M_PATTERN, M_BREATHE };

static vu32 *const reg = (vu32 *)0xCD0000C0;
static lwp_t thread;
static volatile bool running, usable, enabled = true;
static volatile int mode, glow_level, pat_id;
static volatile u32 mode_serial;            /* bumped by every request, so the thread restarts its clock */

static void turn(bool on)
{
    u32 v = *reg & ~0x20;
    if (on) v |= 0x20;
    *reg = v;
}

static void pwm(int level, int periods_left_us)
{
    if (level <= 0) {
        turn(false);
        usleep(periods_left_us);
        return;
    }
    if (level >= 255) {
        turn(true);
        usleep(periods_left_us);
        return;
    }
    int on = PERIOD_US * level / 255;
    turn(true);
    usleep(on);
    turn(false);
    usleep(PERIOD_US - on);
}

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

/* brightness of the current effect at `t` ms after it began (-1: it is over) */
static int level_at(int m, u64 t)
{
    switch (m) {
    case M_GLOW:
        return glow_level;
    case M_PULSE:                           /* up for 120 ms, down for 480 */
        if (t >= 600) return -1;
        return t < 120 ? (int)(255 * t / 120) : (int)(255 * (600 - t) / 480);
    case M_BREATHE: {                       /* a 2.4 s triangle, never fully dark */
        int ph = (int)(t % 2400);
        int tri = ph < 1200 ? ph : 2400 - ph;
        return 12 + (int)(200 * tri / 1200);
    }
    default:
        return 0;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    u32 serial = ~0u;
    u64 began = 0;
    int step = 0;
    u64 step_end = 0;
    while (running) {
        int m = mode;
        if (serial != mode_serial) {
            serial = mode_serial;
            began = now_ms();
            step = 0;
            step_end = 0;
        }
        u64 t = now_ms() - began;
        if (m == M_OFF) {
            turn(false);
            usleep(30000);
        } else if (m == M_PATTERN) {
            if (step_end == 0 || now_ms() >= step_end) {
                if (step_end) step++;
                u16 cmd = step < 255 ? LightPatterns[pat_id][step] : 0;
                if (step >= 255 || (cmd >> 12) == 0) {      /* the end (a loop command isn't used) */
                    mode = M_OFF;
                    continue;
                }
                step_end = now_ms() + ((cmd >> 12) << ((cmd >> 8) & 0x0f)) * 20;
                glow_level = cmd & 0xff;
            }
            pwm(glow_level, PERIOD_US);
        } else {
            int lv = level_at(m, t);
            if (lv < 0) {
                mode = M_OFF;
                continue;
            }
            pwm(lv, PERIOD_US);
        }
    }
    turn(false);
    return NULL;
}

static void request(int m)
{
    if (!usable || !enabled) return;
    mode = m;
    mode_serial++;
}

void wiilight_init(void)
{
    enabled = access(SETTING, F_OK) != 0;
    usable = !g_is_dolphin && !g_is_vwii;   /* neither has a light */
    if (!usable || running) return;
    running = true;
    mode = M_OFF;
    if (LWP_CreateThread(&thread, worker, NULL, NULL, 0, 80) != 0) {
        running = false;
        usable = false;
    }
}

void wiilight_shutdown(void)
{
    if (!running) return;
    running = false;
    LWP_JoinThread(thread, NULL);
    turn(false);
}

void wiilight_off(void) { request(M_OFF); }
void wiilight_glow(int level)
{
    glow_level = level < 0 ? 0 : level > 255 ? 255 : level;
    request(M_GLOW);
}
void wiilight_pulse(void) { request(M_PULSE); }
void wiilight_pattern(int id)
{
    if (id < 0 || id >= 17) return;
    pat_id = id;
    request(M_PATTERN);
}
void wiilight_breathe(bool on) { request(on ? M_BREATHE : M_OFF); }

bool wiilight_enabled(void) { return enabled; }

void wiilight_set_enabled(bool on)
{
    if (!on) {
        mode = M_OFF;
        mode_serial++;
    }
    enabled = on;
    if (on) {
        unlink(SETTING);
    } else {
        mkdir("sd:/apps", 0777);
        mkdir("sd:/apps/GodModeWii", 0777);
        FILE *f = fopen(SETTING, "wb");
        if (f) fclose(f);
    }
}

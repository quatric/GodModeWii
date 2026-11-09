#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <asndlib.h>
#include <gccore.h>
#include <malloc.h>

#include "sfx.h"
#include "sfx_bin.h"
#include "tivo_bin.h"

#define DIR "sd:/apps/GodModeWii"
#define SETTING DIR "/sfx_off"
#define TIVO_SETTING DIR "/tivo"

static bool asnd_on, enabled = true, tivo;
static int last_voice = -1;

/* the order of the sounds in res/sfx.bin (scripts/make_sfx.py) */
enum { MS_CLICK, MS_OPEN, MS_NOTIFY, MS_POWEROFF, MS_MOVEPAGE, MS_LONGTAP, MS_MEMOERASE, MS_PENDOWN,
       MS_UNDO, MS_OPENING };
/* the order of the sounds in res/tivo.bin (scripts/make_tivo.py) */
enum { TV_SELECT, TV_DESELECT, TV_PAGEUP, TV_PAGEDOWN, TV_UPDOWN, TV_THUMBSUP, TV_THUMBSDOWN, TV_BONK,
       TV_ERROR, TV_ALERT, TV_TAGUP, TV_TIVO, TV_SC_ENTER, TV_SC_RETURN, TV_SPEEDUP1, TV_SPEEDUP2,
       TV_SPEEDUP3, TV_SPEEDUP4, TV_SLOWDOWN1 };

/* Each event's sound in either set; -1 is silence. */
static const signed char moonshell[SFX_COUNT] = {
    [SFX_CLICK] = MS_CLICK,         [SFX_OPEN] = MS_OPEN,           [SFX_NOTIFY] = MS_NOTIFY,
    [SFX_POWEROFF] = MS_POWEROFF,   [SFX_MOVEPAGE] = MS_MOVEPAGE,   [SFX_LONGTAP] = MS_LONGTAP,
    [SFX_MEMOERASE] = MS_MEMOERASE, [SFX_PENDOWN] = MS_PENDOWN,     [SFX_UNDO] = MS_UNDO,
    [SFX_OPENING] = MS_OPENING,     [SFX_STARTUP] = MS_LONGTAP,     [SFX_PAGEUP] = MS_MOVEPAGE,
    [SFX_PAGEDOWN] = MS_MOVEPAGE,   [SFX_UPDOWN] = -1,              [SFX_MENU] = -1,
    [SFX_BONK] = -1,                [SFX_ERROR] = MS_CLICK,         [SFX_DONE] = MS_CLICK,
    [SFX_YES] = -1,                 [SFX_NO] = -1,                  [SFX_SPEED1] = -1,
    [SFX_SPEED2] = -1,              [SFX_SPEED3] = -1,              [SFX_SPEED4] = -1,
    [SFX_SLOWDOWN] = -1,
};
static const signed char tivo_set[SFX_COUNT] = {
    [SFX_CLICK] = TV_SELECT,        [SFX_OPEN] = TV_SELECT,         [SFX_NOTIFY] = TV_ALERT,
    [SFX_POWEROFF] = TV_SC_RETURN,  [SFX_MOVEPAGE] = TV_PAGEDOWN,   [SFX_LONGTAP] = TV_DESELECT,
    [SFX_MEMOERASE] = TV_THUMBSDOWN, [SFX_PENDOWN] = TV_UPDOWN,     [SFX_UNDO] = TV_SLOWDOWN1,
    [SFX_OPENING] = TV_TIVO,        [SFX_STARTUP] = TV_TIVO,        [SFX_PAGEUP] = TV_PAGEUP,
    [SFX_PAGEDOWN] = TV_PAGEDOWN,   [SFX_UPDOWN] = TV_UPDOWN,       [SFX_MENU] = TV_SC_ENTER,
    [SFX_BONK] = TV_BONK,           [SFX_ERROR] = TV_BONK,           [SFX_DONE] = TV_TAGUP,
    [SFX_YES] = TV_THUMBSUP,        [SFX_NO] = TV_THUMBSDOWN,       [SFX_SPEED1] = TV_SPEEDUP1,
    [SFX_SPEED2] = TV_SPEEDUP2,     [SFX_SPEED3] = TV_SPEEDUP3,     [SFX_SPEED4] = TV_SPEEDUP4,
    [SFX_SLOWDOWN] = TV_SLOWDOWN1,
};

void sfx_asnd_acquire(void)
{
    if (!asnd_on) {
        ASND_Init();
        asnd_on = true;
    }
    ASND_Pause(0);
}

bool sfx_enabled(void) { return enabled; }

/* Creates (on) or removes (off) a setting's file. */
static bool flag_file(const char *path, bool on)
{
    if (!on)
        return unlink(path) == 0 || access(path, F_OK) != 0;
    mkdir("sd:/apps", 0777);
    mkdir(DIR, 0777);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fclose(f);
    return true;
}

void sfx_set_enabled(bool on)
{
    enabled = on;
    flag_file(SETTING, !on);
}

void sfx_load_setting(void)
{
    enabled = access(SETTING, F_OK) != 0;
    tivo = access(TIVO_SETTING, F_OK) == 0;
}

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* Plays sound i of a bank (sfx.bin at 48 kHz, tivo.bin at 32 kHz; both stereo 16-bit big-endian). */
static void play_bank(const unsigned char *b, int i, int rate, int vol)
{
    if (i < 0 || memcmp(b, "SFX0", 4) || (int)be32(b + 4) <= i) return;
    sfx_asnd_acquire();
    int v = ASND_GetFirstUnusedVoice();
    if (v < 0) return;
    unsigned off = be32(b + 8 + i * 8), len = be32(b + 12 + i * 8);
    ASND_SetVoice(v, VOICE_STEREO_16BIT, rate, 0, (void *)(b + off), len, vol, vol, NULL);
    last_voice = v;
}

static void play(int id)
{
    if (id < 0 || id >= SFX_COUNT) return;
    if (tivo) play_bank(tivo_bin, tivo_set[id], 32000, 230);
    else      play_bank(sfx_bin, moonshell[id], 48000, 200);
}

bool sfx_tivo(void) { return tivo; }

bool sfx_tivo_toggle(void)
{
    if (tivo) sfx_play(SFX_NO);                     /* the last TiVo sound: thumbs down */
    tivo = !tivo;
    bool saved = flag_file(TIVO_SETTING, tivo);
    if (tivo) sfx_play(SFX_STARTUP);                /* TiVo Central */
    return saved;
}

void sfx_cancel(void)
{
    if (asnd_on && last_voice >= 0) ASND_StopVoice(last_voice);
    last_voice = -1;
}

void sfx_play(int id)
{
    if (enabled) play(id);
}

void sfx_play_force(int id) { play(id); }

static int noise_voice = -1;

void sfx_wait(void)
{
    if (!enabled || !asnd_on) return;
    for (int i = 0; i < 90; i++) {                  /* at most 1.5 s */
        bool busy = false;
        for (int v = 0; v < 16; v++)                /* the white noise never ends: it doesn't count */
            if (v != noise_voice && ASND_StatusVoice(v) == SND_WORKING) busy = true;
        if (!busy) break;
        VIDEO_WaitVSync();
    }
}

/* White noise: one second of it, made once, looped until stopped. Plays even when the sounds are off. */
#define NOISE_FRAMES 48000
static s16 *noise_buf;

void sfx_noise_start(void)
{
    if (noise_voice >= 0) return;
    sfx_asnd_acquire();
    if (!noise_buf) {
        noise_buf = memalign(32, NOISE_FRAMES * 4);
        if (!noise_buf) return;
        unsigned seed = 0x2545F491u;
        for (int i = 0; i < NOISE_FRAMES * 2; i++) {
            seed = seed * 1664525u + 1013904223u;
            noise_buf[i] = (s16)((int)(seed >> 16) - 32768) / 3;
        }
        DCFlushRange(noise_buf, NOISE_FRAMES * 4);
    }
    int v = ASND_GetFirstUnusedVoice();
    if (v < 0) return;
    ASND_SetInfiniteVoice(v, VOICE_STEREO_16BIT, 48000, 0, noise_buf, NOISE_FRAMES * 4, 220, 220);
    noise_voice = v;
}

void sfx_noise_stop(void)
{
    if (noise_voice >= 0) ASND_StopVoice(noise_voice);
    noise_voice = -1;
}

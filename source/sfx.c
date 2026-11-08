#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <asndlib.h>
#include <gccore.h>

#include "sfx.h"
#include "sfx_bin.h"

#define SETTING "sd:/apps/GodModeWii/sfx_off"

static bool asnd_on, enabled = true;
static int last_voice = -1;

void sfx_asnd_acquire(void)
{
    if (!asnd_on) {
        ASND_Init();
        asnd_on = true;
    }
    ASND_Pause(0);
}

bool sfx_enabled(void) { return enabled; }

void sfx_set_enabled(bool on)
{
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

void sfx_load_setting(void)
{
    enabled = access(SETTING, F_OK) != 0;
}

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void play(int id)
{
    if (id < 0 || id >= SFX_COUNT) return;
    const unsigned char *b = sfx_bin;
    if (memcmp(b, "SFX0", 4) || (int)be32(b + 4) <= id) return;
    sfx_asnd_acquire();
    int v = ASND_GetFirstUnusedVoice();
    if (v < 0) return;
    unsigned off = be32(b + 8 + id * 8), len = be32(b + 12 + id * 8);
    ASND_SetVoice(v, VOICE_STEREO_16BIT, 48000, 0, (void *)(b + off), len, 200, 200, NULL);
    last_voice = v;
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

void sfx_wait(void)
{
    if (!enabled || !asnd_on) return;
    for (int i = 0; i < 90; i++) {                  /* at most 1.5 s */
        bool busy = false;
        for (int v = 0; v < 16; v++)
            if (ASND_StatusVoice(v) == SND_WORKING) busy = true;
        if (!busy) break;
        VIDEO_WaitVSync();
    }
}

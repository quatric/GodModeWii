/* Voice memos: record from a Wii Speak or a USB microphone (libwiiinput), keep them as 16-bit mono
 * .wav files in GodModeWii/voice, play them back with the audio player. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>
#include <input_wiispeak.h>
#include <input_usbmic.h>

#include "pda.h"
#include "input.h"
#include "media.h"
#include "ftype.h"
#include "pointer.h"
#include "sfx.h"
#include "wiilight.h"
#include "ui.h"
#include "vfs.h"
#include "pmic_firm_bin.h"

#define MAX_MEMOS 64

/* A pmic_firm.bin on the SD card, to use instead of the one built in. */
static u8 *load_firmware(u32 *size)
{
    static const char *const paths[] = { "sd:/apps/GodModeWii/pmic_firm.bin", "sd:/pmic_firm.bin" };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        u8 *b = n > 0 && n < (1 << 20) ? malloc(n) : NULL;
        if (b && fread(b, 1, n, f) == (size_t)n) {
            fclose(f);
            *size = n;
            return b;
        }
        free(b);
        fclose(f);
    }
    return NULL;
}

static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }

static void wav_header(FILE *f, u32 rate, u32 bytes)
{
    u8 h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                 1, 0, 1, 0 };
    put32(h + 4, 36 + bytes);
    put32(h + 24, rate);
    put32(h + 28, rate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    rewind(f);
    fwrite(h, 1, sizeof(h), f);
}

typedef enum { SRC_NONE, SRC_SPEAK, SRC_USB } mic_src;

/* The Wii Speak when there is one (and its firmware), else a USB microphone. */
static mic_src mic_open(u32 *rate, char *why, size_t why_n)
{
    /* the firmware is built in; a pmic_firm.bin on the card takes its place */
    u32 fsize = 0;
    u8 *firm = load_firmware(&fsize);
    if (!firm) {
        fsize = pmic_firm_bin_size;
        firm = malloc(fsize);
        if (firm) memcpy(firm, pmic_firm_bin, fsize);
    }

    ui_splash("Looking for a microphone...");
    s32 r = firm ? WS_Open(firm, fsize) : WS_ENODEV;
    free(firm);
    if (r == WS_OK) {
        WS_SetFreq(WS_FREQ_16KHZ);
        WS_SetSignalProcessing(false);
        if (WS_Start() == WS_OK) {
            *rate = WS_FreqToHz(WS_FREQ_16KHZ);
            return SRC_SPEAK;
        }
        WS_Close();
    }

    um_info info;
    if (UM_Open(&info) == UM_OK) {
        if (info.has_volume) UM_SetVolume(80);
        if (UM_Start() == UM_OK) {
            *rate = info.rate;
            return SRC_USB;
        }
        UM_Close();
    }

    snprintf(why, why_n, "%s",
             "No microphone found. Plug in a Wii Speak or a USB microphone and try again.");
    return SRC_NONE;
}

static u32 mic_read(mic_src src, s16 *buf, u32 n)
{
    return src == SRC_SPEAK ? WS_Read(buf, n) : UM_Read(buf, n);
}

static void mic_close(mic_src src)
{
    if (src == SRC_SPEAK) {
        WS_Stop();
        WS_Close();
    } else {
        UM_Stop();
        UM_Close();
    }
}

static void draw_recording(u32 secs, int level, const char *name)
{
    ui_clear();
    ui_color(C_RED, true);
    ui_field(2, 2, ui_cols() - 4, "RECORDING");
    ui_color(C_WHITE, true);
    ui_field(5, 2, ui_cols() - 4, "%s", name);
    ui_field(7, 2, 20, "%02u:%02u", secs / 60, secs % 60);
    ui_reset();
    ui_color(C_CYAN, true);
    ui_field(ui_rows() - 2, 1, ui_cols() - 2, "A Stop and save    B Cancel");
    ui_reset();
    (void)level;
}

static void record(void)
{
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX + 64], name[48], why[200];
    if (!pda_dir("voice", dir, sizeof(dir))) {
        ui_message("VOICE MEMO", C_RED, "No drive to save to: insert an SD card.");
        return;
    }
    for (int i = 1; i < 1000; i++) {
        snprintf(name, sizeof(name), "memo_%03d.wav", i);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (access(path, F_OK) != 0) break;
    }

    u32 rate = 16000;
    mic_src src = mic_open(&rate, why, sizeof(why));
    if (src == SRC_NONE) {
        ui_message("VOICE MEMO", C_YELLOW, why);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        mic_close(src);
        ui_message("VOICE MEMO", C_RED, "Could not create the file on the card.");
        return;
    }
    wav_header(f, rate, 0);
    sfx_play(SFX_PENDOWN);
    wiilight_glow(90);                      /* lit while recording */

    enum { CHUNK = 4096 };
    static s16 buf[CHUNK];
    static u8 le[CHUNK * 2];
    u32 samples = 0, shown = ~0u;
    bool save = false, failed = false;
    int peak = 0;
    draw_recording(0, 0, name);

    for (;;) {
        u32 p = DetectInput(IN_DOWN) | ptr_ui_buttons();
        u32 n;
        while ((n = mic_read(src, buf, CHUNK)) > 0) {
            for (u32 i = 0; i < n; i++) {
                int v = buf[i];
                le[i * 2] = v & 255;                    /* the file is little-endian */
                le[i * 2 + 1] = (v >> 8) & 255;
                if (abs(v) > peak) peak = abs(v);
            }
            if (fwrite(le, 2, n, f) != n) failed = true;
            samples += n;
        }
        u32 secs = samples / rate;
        if (secs != shown) {
            shown = secs;
            draw_recording(secs, 0, name);
        }
        /* a level meter, redrawn every frame */
        int bars = peak * 40 / 32768;
        ui_color(bars > 34 ? C_RED : C_GREEN, true);
        ui_goto(9, 2);
        for (int i = 0; i < 40; i++) putchar(i < bars ? '#' : '.');
        ui_reset();
        peak = peak * 7 / 8;

        if (failed || (p & WPAD_BUTTON_A)) {
            save = !failed;
            break;
        }
        if (p & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
    }

    mic_close(src);
    wiilight_off();
    wav_header(f, rate, samples * 2);
    bool ok = fclose(f) == 0 && !failed;
    if (!save || !ok || samples == 0) unlink(path);
    if (failed || !ok)
        ui_message("VOICE MEMO", C_RED, "The card is full or could not be written.");
    else if (save && samples) {
        sfx_play(SFX_NOTIFY);
        wiilight_pattern(WL_BUSH_WARBLER);
        snprintf(why, sizeof(why), "Saved GodModeWii/voice/%s", name);
        ui_message("VOICE MEMO", C_GREEN, why);
    }
}

static int cmp(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);      /* newest (highest number) first */
}

void memo_run(void)
{
    static char names[MAX_MEMOS][64];
    int sel = 0;
    for (;;) {
        char dir[VFS_PATH_MAX];
        int n = 0;
        if (pda_dir("voice", dir, sizeof(dir))) {
            DIR *d = opendir(dir);
            struct dirent *e;
            while (d && (e = readdir(d)) && n < MAX_MEMOS) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < sizeof(names[0]) && !strcasecmp(e->d_name + l - 4, ".wav"))
                    strcpy(names[n++], e->d_name);
            }
            if (d) closedir(d);
            qsort(names, n, sizeof(names[0]), cmp);
        }

        const char *items[MAX_MEMOS + 1];
        items[0] = "* Record a new voice memo";
        for (int i = 0; i < n; i++) items[i + 1] = names[i];
        int r = ui_menu_ex("VOICE MEMOS", items, NULL, n + 1, sel,
                           "A Select   B Back");
        if (r < 0) return;
        sel = r;
        if (r == 0) {
            record();
            sel = 0;
            continue;
        }

        const char *acts[] = { "Play", "Delete" };
        int act = ui_menu(names[r - 1], acts, NULL, 2, 0);
        char path[VFS_PATH_MAX + 80];
        snprintf(path, sizeof(path), "%s/%s", dir, names[r - 1]);
        if (act == 0) {
            if (!media_open_type(path, FT_AUDIO))
                ui_message("VOICE MEMO", C_YELLOW, "This memo couldn't be played.");
        } else if (act == 1 && ui_confirm("DELETE", C_RED, "Delete this voice memo?")) {
            unlink(path);
            sel = 0;
        }
    }
}

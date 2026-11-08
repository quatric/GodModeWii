#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gccore.h>

#include "colors.h"
#include "ui.h"

#define SETTING "sd:/apps/GodModeWii/theme"

typedef struct {
    const char *name;
    u32 bg, fg, bold;                       /* background, text, bold text: 0xRRGGBB */
    u32 hue[2][6];                          /* red green yellow blue magenta cyan; normal, bright */
} ctheme;

/* Terminal's own ANSI colours */
#define ANSI { { 0x990000, 0x00A600, 0x999900, 0x0000B2, 0xB200B2, 0x00A6B2 }, \
               { 0xE50000, 0x00D900, 0xE5E500, 0x0000FF, 0xE500E5, 0x00E5E5 } }

static const ctheme THEMES[] = {
    { "Classic",        0, 0, 0, ANSI },    /* the original colours: not applied, restored */
    { "Basic",          0xFFFFFF, 0x000000, 0x000000, ANSI },
    { "Grass",          0x13773D, 0xFFF0A5, 0xFFFFFF,
      { { 0x990000, 0xB4FF4D, 0x999900, 0x0A3D1E, 0xB200B2, 0x00A6B2 },
        { 0xE50000, 0xD8FF8A, 0xE5E500, 0x0000FF, 0xE500E5, 0x00E5E5 } } },
    { "Homebrew",       0x000000, 0x28FE14, 0x28FE14, ANSI },
    { "Man Page",       0xFEF49C, 0x000000, 0x000000, ANSI },
    { "Novel",          0xDFDBC3, 0x3B2322, 0x3B2322, ANSI },
    { "Ocean",          0x224FBC, 0xFFFFFF, 0xFFFFFF,
      { { 0x990000, 0x00A600, 0x999900, 0x0A1F6B, 0xB200B2, 0x00A6B2 },
        { 0xE50000, 0x00D900, 0xE5E500, 0x1A3FA0, 0xE500E5, 0x00E5E5 } } },
    { "Pro",            0x000000, 0xF2F2F2, 0xFFFFFF, ANSI },
    { "Red Sands",      0x7A251E, 0xD7C9A7, 0xFFFFFF,
      { { 0xFF3B2B, 0x00A600, 0x999900, 0x0000B2, 0xB200B2, 0x00A6B2 },
        { 0xFF6B5B, 0x00D900, 0xE5E500, 0x0000FF, 0xE500E5, 0x00E5E5 } } },
    { "Silver Aerogel", 0x929292, 0x000000, 0x000000, ANSI },
};
#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

#define MAX_TABLES 4
static u32 *tables[MAX_TABLES];             /* libogc's colorTable in memory (and any stale copy of the image) */
static int ntables;
static u32 *table;
static u32 orig[24];
static int current;

int colors_count(void) { return NTHEMES; }
const char *colors_name(int i) { return i >= 0 && i < NTHEMES ? THEMES[i].name : ""; }
int colors_current(void) { return current; }

u32 colors_fg_rgb(void) { return current ? THEMES[current].fg : 0xE0E0E0; }
u32 colors_bg_rgb(void) { return current ? THEMES[current].bg : 0x000000; }

static void rgb_yuv(u32 rgb, int *y, int *u, int *v)
{
    int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    *y = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
    *u = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
    *v = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
}

static u32 word(u32 rgb)
{
    int y, u, v;
    rgb_yuv(rgb, &y, &u, &v);
    return (u32)y << 24 | (u32)u << 16 | (u32)y << 8 | (u32)v;
}

static u32 mix(u32 a, u32 b, int pct_b)
{
    u32 out = 0;
    for (int s = 0; s < 24; s += 8) {
        int ca = a >> s & 255, cb = b >> s & 255;
        out |= (u32)((ca * (100 - pct_b) + cb * pct_b) / 100) << s;
    }
    return out;
}

/* The console's table, found by its contents (it's static in libogc). The self-extracting .dol can leave
 * a copy of the program lower in memory, so every match is kept and patched, not just the first. */
static bool find_table(void)
{
    if (ntables) return true;
    static const u32 key[3] = { 0x10801080, 0x317031b1, 0x51605157 };
    const u32 *p = (const u32 *)0x80003100;
    const u32 *end = (const u32 *)0x81800000 - 24;
    for (; p < end && ntables < MAX_TABLES; p++) {
        if (p[0] != key[0] || p[1] != key[1] || p[2] != key[2]) continue;
        if (p[7] != 0xc080c080 || p[15] != 0xf080f080 || p[23] != 0x70807080) continue;
        if (!ntables) memcpy(orig, p, sizeof(orig));
        tables[ntables++] = (u32 *)p;
    }
    table = ntables ? tables[0] : NULL;
    return ntables > 0;
}

static u32 theme_bg_word;

void colors_apply(int i, bool save)
{
    if (i < 0 || i >= NTHEMES || !find_table()) return;
    const ctheme *t = &THEMES[i];
    u32 w[24];
    if (i == 0) {
        memcpy(w, orig, sizeof(orig));
    } else {
        for (int set = 0; set < 2; set++) {
            u32 *row = w + set * 8;
            row[0] = word(set ? mix(t->bg, t->fg, 45) : t->bg);
            for (int h = 0; h < 6; h++) row[1 + h] = word(t->hue[set][h]);
            row[7] = word(set ? t->bold : t->fg);
        }
        for (int c = 0; c < 8; c++) {                       /* the dim set: the colours, faded */
            u32 n = w[c];
            int y = n >> 24, u = n >> 16 & 255, v = n & 255;
            int by = w[0] >> 24;
            w[16 + c] = (u32)((y + by) / 2) << 24 | (u32)u << 16 | (u32)((y + by) / 2) << 8 | (u32)v;
        }
    }
    for (int k = 0; k < ntables; k++) {
        memcpy(tables[k], w, sizeof(w));
        DCFlushRange(tables[k], sizeof(w));
    }
    theme_bg_word = w[0];
    current = i;
    if (save) {
        mkdir("sd:/apps", 0777);
        mkdir("sd:/apps/GodModeWii", 0777);
        FILE *f = fopen(SETTING, "wb");
        if (f) {
            fprintf(f, "%s\n", t->name);
            fclose(f);
        }
    }
    ui_wipe_with(theme_bg_word);
}

void colors_load(void)
{
    FILE *f = fopen(SETTING, "rb");
    if (!f) return;
    char name[48] = "";
    if (fgets(name, sizeof(name), f)) name[strcspn(name, "\r\n")] = '\0';
    fclose(f);
    for (int i = 0; i < NTHEMES; i++)
        if (!strcmp(name, THEMES[i].name)) {
            colors_apply(i, false);
            return;
        }
}

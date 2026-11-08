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

/* Every theme has its own six hues, not just a background, because the interface paints with them:
 *   normal red, blue, magenta   bars and dialog bodies under bold text (title bar, dialogs, errors)
 *   normal green, yellow, cyan  bars under background-coloured text (the selection, button strips)
 *   bright ones                 text on the background (headings, notes, warnings, errors)
 * So on a light theme the first three are pale and the last three dark, and the other way round on a
 * dark one. */
#define PAL(r, g, y, b, m, c, R, G, Y, B, M, C) { { r, g, y, b, m, c }, { R, G, Y, B, M, C } }

static const ctheme THEMES[] = {
    { "Classic",        0, 0, 0, PAL(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) },  /* the original colours, restored */
    { "Basic",          0xFFFFFF, 0x000000, 0x000000,
      PAL(0xF4C7C3, 0x1C7C1C, 0x8A6D00, 0xD6E4F5, 0xE8C8F0, 0x1660D0,
          0xC41A16, 0x1C8A1C, 0x9A6A00, 0x1F4FBF, 0x9B2393, 0x0B5CAD) },
    { "Grass",          0x13773D, 0xFFF0A5, 0xFFFFFF,
      PAL(0x8E1A12, 0xB4FF4D, 0xFFE066, 0x0A4A22, 0x7A2A6A, 0xE8D36B,
          0xFFB8A8, 0x8CFF8C, 0xFFE066, 0x9CC4FF, 0xFFB0E0, 0xB4FF4D) },
    /* green on black, but bold text, "done" and the bars are other shades so nothing green sits on green */
    { "Homebrew",       0x000000, 0x28FE14, 0xC8FFC0,
      PAL(0x6A0000, 0x28FE14, 0xC8C800, 0x0A3A0A, 0x4A004A, 0x28FE14,
          0xFF5A5A, 0xF0FFF0, 0xFFD24D, 0x6AA8FF, 0xFF6AFF, 0x7CF0FF) },
    { "Man Page",       0xFEF49C, 0x000000, 0x000000,
      PAL(0xF2B8A0, 0x2E6B00, 0x7A5A00, 0xE8DC80, 0xE8C0E0, 0x1A1A7A,
          0xCC0000, 0x007A00, 0x8A5A00, 0x0000CC, 0xA000A0, 0x00008A) },
    { "Novel",          0xDFDBC3, 0x3B2322, 0x3B2322,
      PAL(0xE0B0A0, 0x3F6E1A, 0x8A6014, 0xC9C2A0, 0xD8B8C8, 0x74331F,
          0xA0221A, 0x3A6418, 0x7A5208, 0x3A5A9A, 0x8E3A6E, 0x8B3A1E) },
    { "Ocean",          0x224FBC, 0xFFFFFF, 0xFFFFFF,
      PAL(0x9A1A1A, 0x9CF0A0, 0xF0E080, 0x0A2F80, 0x6A1A80, 0x9FD7FF,
          0xFFB4B4, 0x7CFF8C, 0xFFF066, 0xC0D8FF, 0xFF90FF, 0x9FEFFF) },
    { "Pro",            0x000000, 0xF2F2F2, 0xFFFFFF,
      PAL(0x7A0000, 0x00C000, 0xD0D000, 0x333333, 0x660066, 0xB0B0B0,
          0xFF4040, 0x40E040, 0xF0F040, 0x6A9CFF, 0xF060F0, 0x6CD0FF) },
    { "Red Sands",      0x7A251E, 0xD7C9A7, 0xFFFFFF,
      PAL(0xC8341C, 0xA8C878, 0xE8C060, 0x4A1410, 0x6A1A4A, 0xD7C9A7,
          0xFF7B6B, 0x8CE07C, 0xFFD700, 0x8CA8FF, 0xFF8CE0, 0xFFC98A) },
    { "Silver Aerogel", 0x929292, 0x000000, 0x000000,
      PAL(0xD8A0A0, 0x1E4A1E, 0x4A3A00, 0xB8B8B8, 0xC8A8C8, 0x202040,
          0x6A0000, 0x003800, 0x3A2600, 0x00008A, 0x6A006A, 0x101060) },
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
        if (p == orig) continue;                    /* the saved original (copied below) is not a table */
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

#ifdef GMW_AUTOTEST
/* "@colors <n>": applies theme n and notes where the colour table was found and what it holds. */
void colors_autotest(int i)
{
    colors_apply(i, false);
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    fprintf(lg, "theme %d (%s): current %d, %d table(s)\n", i, colors_name(i), current, ntables);
    for (int k = 0; k < ntables; k++)
        fprintf(lg, "  %p: %08x %08x %08x .. %08x\n", (void *)tables[k], (unsigned)tables[k][0],
                (unsigned)tables[k][1], (unsigned)tables[k][6], (unsigned)tables[k][7]);
    fprintf(lg, "  orig: %08x %08x %08x .. %08x\n", (unsigned)orig[0], (unsigned)orig[1],
            (unsigned)orig[6], (unsigned)orig[7]);
    fclose(lg);
}
#endif

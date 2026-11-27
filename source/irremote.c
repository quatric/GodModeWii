/*
 * IR remote: TV, receiver and set-top box codes from the Wii's sensor bar
 * (libwiiir) or the Wii U GamePad (libwiiuir), sent with the buttons of any
 * controller (libwiiinput / libwiiuinput), the way each preset maps them.
 *
 * The same file is in GodModeWii and GodModeU.
 *
 * Presets live in <app folder>/ir_presets.txt; the remotes come from
 * irdb.dat (Flipper-IRDB) and irdb_custom.xml (the user's own, written as a
 * commented template the first time).
 *
 * No sounds play here: any sound still playing is stopped before a code is
 * sent, and leaving or switching presets is told with rumble (and on the Wii
 * the disc slot light), so it works without looking at the screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __WIIU__
#include <coreinit/time.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <input_wiiu.h>
#include <ir_wiiu.h>
#define APP_DIR "sd:/wiiu/apps/GodModeU"
#define SENDER "the GamePad's IR window"
#else
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>
#include <ir_wii.h>
#include "wiilight.h"
#define APP_DIR "sd:/apps/GodModeWii"
#define SENDER "the sensor bar"
#endif

#include "input.h"
#include "irremote.h"
#include "sfx.h"
#include "ui.h"

#define PRESETS_FILE APP_DIR "/ir_presets.txt"
#define MAX_PRESETS  32
#define MAX_BINDS    64
#define KEYS         64   /* kind * 8 + port */
#define REPEAT_MS    450  /* a held button repeats its code after this */

typedef struct {
    u8   kind;            /* input_device_kind */
    bool ext;             /* a Wii Remote extension's button (raw_ext), else the device's own (raw) */
    u32  bit;
    char ext_name[12];    /* "Nunchuk" / "Classic", when ext */
    char signal[40];      /* the remote's button: "Power", or "Power (2)" for a second one of that name */
} binding;

typedef struct {
    char name[32];
    char category[48], maker[64], device[128];
    binding binds[MAX_BINDS];
    int nbinds;
} preset;

static preset *presets; /* MAX_PRESETS of them while the remote is open (130 KB), else NULL */
static int npresets, current;
static ir_db db;

static u64 now_ms(void)
{
#ifdef __WIIU__
    return OSTicksToMilliseconds(OSGetSystemTime());
#else
    return ticks_to_millisecs(gettime());
#endif
}

/* ---------------------------------------------------------------- button names */

typedef struct {
    u32 bit;
    const char *name;
} btn;

/* A Wii Remote's power button: not a button bit (pressing it turns the remote off), so it gets one of its own */
#define POWER_BIT 0x80000000u
#define HOLD_TO_LEAVE_MS 1000 /* when HOME sends a code, holding it this long leaves instead */

static const btn wiimote_btns[] = {
#ifndef __WIIU__
    {POWER_BIT, "Power"},
#endif
    {WPAD_BUTTON_A, "A"}, {WPAD_BUTTON_B, "B"}, {WPAD_BUTTON_1, "1"}, {WPAD_BUTTON_2, "2"},
    {WPAD_BUTTON_PLUS, "+"}, {WPAD_BUTTON_MINUS, "-"}, {WPAD_BUTTON_UP, "Up"}, {WPAD_BUTTON_DOWN, "Down"},
    {WPAD_BUTTON_LEFT, "Left"}, {WPAD_BUTTON_RIGHT, "Right"}, {WPAD_BUTTON_HOME, "HOME"},
};
static const btn nunchuk_btns[] = {{WPAD_NUNCHUK_BUTTON_Z, "Z"}, {WPAD_NUNCHUK_BUTTON_C, "C"}};

#ifdef __WIIU__
#define CLASSIC_L WPAD_CLASSIC_BUTTON_L
#define CLASSIC_R WPAD_CLASSIC_BUTTON_R
#else
#define CLASSIC_L WPAD_CLASSIC_BUTTON_FULL_L
#define CLASSIC_R WPAD_CLASSIC_BUTTON_FULL_R
#endif
static const btn classic_btns[] = {
    {WPAD_CLASSIC_BUTTON_A, "A"}, {WPAD_CLASSIC_BUTTON_B, "B"}, {WPAD_CLASSIC_BUTTON_X, "X"},
    {WPAD_CLASSIC_BUTTON_Y, "Y"}, {CLASSIC_L, "L"}, {CLASSIC_R, "R"}, {WPAD_CLASSIC_BUTTON_ZL, "ZL"},
    {WPAD_CLASSIC_BUTTON_ZR, "ZR"}, {WPAD_CLASSIC_BUTTON_PLUS, "+"}, {WPAD_CLASSIC_BUTTON_MINUS, "-"},
    {WPAD_CLASSIC_BUTTON_UP, "Up"}, {WPAD_CLASSIC_BUTTON_DOWN, "Down"}, {WPAD_CLASSIC_BUTTON_LEFT, "Left"},
    {WPAD_CLASSIC_BUTTON_RIGHT, "Right"}, {WPAD_CLASSIC_BUTTON_HOME, "HOME"},
};

#ifdef __WIIU__
static const btn gamepad_btns[] = {
    {VPAD_BUTTON_A, "A"}, {VPAD_BUTTON_B, "B"}, {VPAD_BUTTON_X, "X"}, {VPAD_BUTTON_Y, "Y"},
    {VPAD_BUTTON_L, "L"}, {VPAD_BUTTON_R, "R"}, {VPAD_BUTTON_ZL, "ZL"}, {VPAD_BUTTON_ZR, "ZR"},
    {VPAD_BUTTON_PLUS, "+"}, {VPAD_BUTTON_MINUS, "-"}, {VPAD_BUTTON_UP, "Up"}, {VPAD_BUTTON_DOWN, "Down"},
    {VPAD_BUTTON_LEFT, "Left"}, {VPAD_BUTTON_RIGHT, "Right"}, {VPAD_BUTTON_STICK_L, "L stick"},
    {VPAD_BUTTON_STICK_R, "R stick"}, {VPAD_BUTTON_TV, "TV"}, {VPAD_BUTTON_HOME, "HOME"},
};
static const btn pro_btns[] = {
    {WPAD_PRO_BUTTON_A, "A"}, {WPAD_PRO_BUTTON_B, "B"}, {WPAD_PRO_BUTTON_X, "X"}, {WPAD_PRO_BUTTON_Y, "Y"},
    {WPAD_PRO_BUTTON_L, "L"}, {WPAD_PRO_BUTTON_R, "R"}, {WPAD_PRO_BUTTON_ZL, "ZL"}, {WPAD_PRO_BUTTON_ZR, "ZR"},
    {WPAD_PRO_BUTTON_PLUS, "+"}, {WPAD_PRO_BUTTON_MINUS, "-"}, {WPAD_PRO_BUTTON_UP, "Up"},
    {WPAD_PRO_BUTTON_DOWN, "Down"}, {WPAD_PRO_BUTTON_LEFT, "Left"}, {WPAD_PRO_BUTTON_RIGHT, "Right"},
    {WPAD_PRO_BUTTON_STICK_L, "L stick"}, {WPAD_PRO_BUTTON_STICK_R, "R stick"}, {WPAD_PRO_BUTTON_HOME, "HOME"},
};
static const btn gc_btns[] = {
    {INPUT_GC_A, "A"}, {INPUT_GC_B, "B"}, {INPUT_GC_X, "X"}, {INPUT_GC_Y, "Y"}, {INPUT_GC_Z, "Z"},
    {INPUT_GC_L, "L"}, {INPUT_GC_R, "R"}, {INPUT_GC_START, "Start"}, {INPUT_GC_UP, "Up"},
    {INPUT_GC_DOWN, "Down"}, {INPUT_GC_LEFT, "Left"}, {INPUT_GC_RIGHT, "Right"},
};
#else
#define pro_btns classic_btns /* a Wii U Pro Controller reads as a Classic Controller here */
static const btn gc_btns[] = {
    {PAD_BUTTON_A, "A"}, {PAD_BUTTON_B, "B"}, {PAD_BUTTON_X, "X"}, {PAD_BUTTON_Y, "Y"}, {PAD_TRIGGER_Z, "Z"},
    {PAD_TRIGGER_L, "L"}, {PAD_TRIGGER_R, "R"}, {PAD_BUTTON_START, "Start"}, {PAD_BUTTON_UP, "Up"},
    {PAD_BUTTON_DOWN, "Down"}, {PAD_BUTTON_LEFT, "Left"}, {PAD_BUTTON_RIGHT, "Right"},
};
#endif

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The names of a device's buttons; NULL when it has no table (any bit is then taken). */
static const btn *buttons_of(int kind, bool ext, const char *ext_name, size_t *n)
{
    if (ext) {
        if (!strcmp(ext_name, "Nunchuk")) return *n = COUNT(nunchuk_btns), nunchuk_btns;
        if (!strcmp(ext_name, "Classic")) return *n = COUNT(classic_btns), classic_btns;
        return *n = 0, NULL;
    }
    switch (kind) {
    case INPUT_DEV_WIIMOTE: return *n = COUNT(wiimote_btns), wiimote_btns;
    case INPUT_DEV_PRO: return *n = COUNT(pro_btns), pro_btns;
    case INPUT_DEV_GAMECUBE:
    case INPUT_DEV_GCADAPTER:
    case INPUT_DEV_USBPAD: return *n = COUNT(gc_btns), gc_btns;
#ifdef __WIIU__
    case INPUT_DEV_GAMEPAD: return *n = COUNT(gamepad_btns), gamepad_btns;
#endif
    default: return *n = 0, NULL;
    }
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case INPUT_DEV_GAMEPAD: return "GamePad";
    case INPUT_DEV_WIIMOTE: return "Wii Remote";
    case INPUT_DEV_PRO: return "Pro Controller";
    case INPUT_DEV_GAMECUBE: return "GameCube";
    case INPUT_DEV_USBPAD: return "USB pad";
    case INPUT_DEV_GCADAPTER: return "GC adapter";
    case INPUT_DEV_DS3: return "DualShock 3";
    default: return "Controller";
    }
}

/* "Classic ZL", "GamePad TV", "Wii Remote 1". */
static void bind_label(const binding *b, char *out, size_t size)
{
    size_t n;
    const btn *t = buttons_of(b->kind, b->ext, b->ext_name, &n);
    const char *dev = b->ext ? b->ext_name : kind_name(b->kind);
    for (size_t i = 0; t && i < n; i++)
        if (t[i].bit == b->bit) {
            snprintf(out, size, "%s %s", dev, t[i].name);
            return;
        }
    int idx = 0;
    while (idx < 31 && !(b->bit >> idx & 1))
        idx++;
    snprintf(out, size, "%s button %d", dev, idx + 1);
}

/* Of the newly pressed bits, the first one that can be bound (a named one, or any on a device with no names). */
static u32 bindable(int kind, bool ext, const char *ext_name, u32 pressed)
{
    size_t n;
    const btn *t = buttons_of(kind, ext, ext_name, &n);
    if (!t && ext)
        return 0; /* an extension we have no names for (Guitar, Drums...): left alone */
    for (int i = 0; i < 32; i++) {
        u32 bit = 1u << i;
        if (!(pressed & bit))
            continue;
        if (!t)
            return bit;
        for (size_t k = 0; k < n; k++)
            if (t[k].bit == bit)
                return bit;
    }
    return 0;
}

/* ---------------------------------------------------------------- presets on the card */

static char *field(char **p)
{
    char *s = *p, *tab = s ? strchr(s, '\t') : NULL;
    if (tab) {
        *tab = 0;
        *p = tab + 1;
    } else {
        *p = NULL;
    }
    return s ? s : "";
}

static void copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

static void presets_load(void)
{
    npresets = 0;
    current = 0;
    FILE *f = fopen(PRESETS_FILE, "r");
    if (!f)
        return;
    char line[512];
    preset *p = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *rest = line;
        char *key = field(&rest);
        if (!strcmp(key, "current")) {
            current = atoi(field(&rest));
        } else if (!strcmp(key, "preset") && npresets < MAX_PRESETS) {
            p = &presets[npresets++];
            memset(p, 0, sizeof *p);
            copy(p->name, sizeof p->name, field(&rest));
        } else if (!strcmp(key, "device") && p) {
            copy(p->category, sizeof p->category, field(&rest));
            copy(p->maker, sizeof p->maker, field(&rest));
            copy(p->device, sizeof p->device, field(&rest));
        } else if (!strcmp(key, "bind") && p && p->nbinds < MAX_BINDS) {
            binding *b = &p->binds[p->nbinds];
            memset(b, 0, sizeof *b);
            b->kind = atoi(field(&rest));
            const char *ext = field(&rest);
            b->ext = strcmp(ext, "-") != 0;
            if (b->ext)
                copy(b->ext_name, sizeof b->ext_name, ext);
            b->bit = strtoul(field(&rest), NULL, 0);
            copy(b->signal, sizeof b->signal, field(&rest));
            if (b->bit && b->signal[0])
                p->nbinds++;
        }
    }
    fclose(f);
    if (current < 0 || current >= npresets)
        current = 0;
}

/* Written to a new file first, so a failed write never loses the presets already saved. */
static bool presets_save(void)
{
#ifdef __WIIU__
    mkdir("sd:/wiiu/apps", 0777);
#endif
    mkdir(APP_DIR, 0777);
    FILE *f = fopen(PRESETS_FILE ".new", "w");
    if (!f)
        return false;
    bool ok = fprintf(f, "# IR remote presets (tab-separated)\ncurrent\t%d\n", current) > 0;
    for (int i = 0; i < npresets && ok; i++) {
        const preset *p = &presets[i];
        ok = fprintf(f, "preset\t%s\ndevice\t%s\t%s\t%s\n", p->name, p->category, p->maker, p->device) > 0;
        for (int k = 0; k < p->nbinds && ok; k++) {
            const binding *b = &p->binds[k];
            ok = fprintf(f, "bind\t%d\t%s\t0x%X\t%s\n", b->kind, b->ext ? b->ext_name : "-", (unsigned)b->bit,
                         b->signal) > 0;
        }
    }
    if (fclose(f) || !ok) {
        remove(PRESETS_FILE ".new");
        return false;
    }
    remove(PRESETS_FILE);
    return rename(PRESETS_FILE ".new", PRESETS_FILE) == 0;
}

static void save_or_warn(void)
{
    if (!presets_save())
        ui_message("NOT SAVED", C_RED, "The presets could not be saved. Is the SD card in and writable?");
}

/* ---------------------------------------------------------------- the remote list */

/* irdb_custom.xml as a template the first time: examples to copy, all commented out */
static void custom_template(void)
{
    FILE *f = fopen(APP_DIR "/irdb_custom.xml", "r");
    if (f) {
        fclose(f);
        return;
    }
    f = fopen(APP_DIR "/irdb_custom.xml", "w");
    if (!f)
        return;
    fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<!-- Your own remotes. They show up in the IR remote next to the built-in list, and app updates\n"
          "     never replace this file.\n\n"
          "     A device needs a manufacturer, a name and a category (a new category is fine). Each\n"
          "     <ButtonEntry> is one button; its <Data> is the code, in one of three forms:\n\n"
          "       Protocol:address,command   NEC, NECext, NEC42, NEC42ext, Samsung32, RC5, RC5X, RC6,\n"
          "                                  SIRC, SIRC15, SIRC20, Kaseikyo, RCA, Pioneer or JVC, with the\n"
          "                                  address and command as Flipper-IRDB writes them (0x hex or\n"
          "                                  decimal; a Flipper .ir file's \"address: 04 00 00 00\" is 0x04)\n"
          "       raw timings                microseconds, mark first, with frequency=\"38000\" and\n"
          "                                  duty=\"0.33\" on the <Data> tag\n"
          "       RAW:pronto                 a learned Pronto hex code (0000 ...)\n\n"
          "     Copy the example out of this comment, between <Manufacturers> and </Manufacturers>,\n"
          "     and change it:\n\n"
          "  <Manufacturer name=\"My brand\">\n"
          "    <DeviceList>\n"
          "      <DeviceEntry name=\"Living room TV\" category=\"TVs\">\n"
          "        <ButtonEntry name=\"Power\"><Data>NEC:0x04,0x08</Data></ButtonEntry>\n"
          "        <ButtonEntry name=\"Vol_up\"><Data>NECext:0x7F00,0xEA15</Data></ButtonEntry>\n"
          "        <ButtonEntry name=\"Input\"><Data frequency=\"38000\" duty=\"0.33\">9000 4500 560 560 560 1690 560</Data></ButtonEntry>\n"
          "        <ButtonEntry name=\"Mute\"><Data>RAW:0000 006D 0002 0000 0157 00AC 0015 0689</Data></ButtonEntry>\n"
          "      </DeviceEntry>\n"
          "    </DeviceList>\n"
          "  </Manufacturer>\n"
          "-->\n"
          "<Manufacturers>\n"
          "</Manufacturers>\n",
          f);
    fclose(f);
}

static bool db_open(void)
{
    if (db.count)
        return true;
    ui_splash("Reading the remote list (irdb.dat)...");
    custom_template();
    IR_DbAdd(&db, APP_DIR "/irdb.dat");
    IR_DbAdd(&db, APP_DIR "/irdb_custom.xml");
    input_flush();
    if (db.count)
        return true;
    ui_message("NO REMOTE LIST", C_YELLOW,
               "irdb.dat was not found in " APP_DIR "/. It comes in the release zip next to the app: copy it "
               "there.");
    return false;
}

static bool load_remote(const preset *p, ir_remote *r)
{
    int i = IR_DbFind(&db, p->category, p->maker, p->device);
    return i >= 0 && IR_DbLoad(&db, i, r) == 0;
}

static void pretty(char *out, size_t size, const char *s)
{
    snprintf(out, size, "%s", s);
    for (char *c = out; *c; c++)
        if (*c == '_')
            *c = ' ';
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

/* Category, then manufacturer, then device. The device's index, or -1. */
static int pick_device(void)
{
    for (;;) {
        const char **cats = malloc((db.count + 1) * sizeof *cats);
        if (!cats)
            return -1;
        int nc = 0;
        for (size_t i = 0; i < db.count; i++) {
            int k = 0;
            while (k < nc && cats[k] != db.devices[i].category)
                k++;
            if (k == nc)
                cats[nc++] = db.devices[i].category;
        }
        qsort(cats, nc, sizeof *cats, by_name);
        char (*labels)[64] = malloc((nc + 1) * sizeof *labels);
        const char **items = malloc((nc + 1) * sizeof *items);
        if (!labels || !items) {
            free(cats), free(labels), free(items);
            return -1;
        }
        for (int k = 0; k < nc; k++) {
            pretty(labels[k], sizeof labels[k], cats[k]);
            items[k] = labels[k];
        }
        ui_clear();
        int c = ui_menu("WHAT IS IT?", items, NULL, nc, 0);
        const char *cat = c >= 0 && c < nc ? cats[c] : NULL;
        free(cats), free(labels), free(items);
        if (!cat)
            return -1;

        for (;;) {
            /* manufacturers that make one */
            const char **makers = malloc(db.count * sizeof *makers);
            int nm = 0;
            for (size_t i = 0; makers && i < db.count; i++) {
                if (db.devices[i].category != cat)
                    continue;
                int k = 0;
                while (k < nm && strcasecmp(makers[k], db.devices[i].manufacturer))
                    k++;
                if (k == nm)
                    makers[nm++] = db.devices[i].manufacturer;
            }
            if (!makers || !nm) {
                free(makers);
                break;
            }
            qsort(makers, nm, sizeof *makers, by_name);
            ui_clear();
            int m = ui_menu("MANUFACTURER", makers, NULL, nm, 0);
            const char *maker = m >= 0 ? makers[m] : NULL;
            free(makers);
            if (!maker)
                break;

            /* its devices */
            int *idx = malloc(db.count * sizeof *idx);
            char (*dl)[96] = malloc(db.count * sizeof *dl);
            const char **di = malloc(db.count * sizeof *di);
            int nd = 0;
            for (size_t i = 0; idx && dl && di && i < db.count; i++) {
                const ir_db_device *d = &db.devices[i];
                if (d->category != cat || strcasecmp(d->manufacturer, maker))
                    continue;
                snprintf(dl[nd], sizeof dl[nd], "%s  (%u buttons)", d->name, (unsigned)d->buttons);
                di[nd] = dl[nd];
                idx[nd++] = (int)i;
            }
            if (nd)
                ui_clear();
            int d = nd ? ui_menu(maker, di, NULL, nd, 0) : -1;
            int pick = d >= 0 ? idx[d] : -1;
            free(idx), free(dl), free(di);
            if (pick >= 0)
                return pick;
        }
    }
}

/* ---------------------------------------------------------------- sending */

/* "Power", or "Power (2)" for the second button of that name: some remotes list a name more than once,
 * with other codes (other models or years). */
static void signal_label(const ir_remote *r, size_t i, char *out, size_t size)
{
    int k = 1;
    for (size_t j = 0; j < i; j++)
        k += !strcmp(r->signals[j].name, r->signals[i].name);
    if (k > 1)
        snprintf(out, size, "%s (%d)", r->signals[i].name, k);
    else
        snprintf(out, size, "%s", r->signals[i].name);
}

static const ir_signal *find_signal(const ir_remote *r, const char *label)
{
    for (size_t i = 0; i < r->count; i++)
        if (!strcmp(r->signals[i].name, label))
            return &r->signals[i];
    const char *p = strrchr(label, '(');
    int k = p && p > label && p[-1] == ' ' ? atoi(p + 1) : 0;
    size_t len = p ? (size_t)(p - 1 - label) : 0;
    for (size_t i = 0; k >= 2 && i < r->count; i++)
        if (strlen(r->signals[i].name) == len && !strncmp(r->signals[i].name, label, len) && --k == 0)
            return &r->signals[i];
    return NULL;
}

static void send(const ir_remote *r, const char *name, bool repeat, char *status, size_t size)
{
    const ir_signal *s = find_signal(r, name);
    if (!s) {
        snprintf(status, size, "\"%s\" is not in this remote", name);
        return;
    }
    sfx_cancel(); /* nothing else plays while the code goes out */
    int ret = repeat ? IR_SendRepeat(s) : IR_Send(s);
    if (!repeat)
        snprintf(status, size, ret ? "Could not send %s" : "Sent %s", name);
}

/* `pulses` short buzzes on device `dev` (and a flash of the disc slot light), for knowing without looking. */
static void buzz(int dev, int pulses, int on_ms)
{
    for (int i = 0; i < pulses; i++) {
        if (dev >= 0)
            INPUT_DeviceRumble(dev, true);
#ifndef __WIIU__
        wiilight_pulse();
#endif
        usleep(on_ms * 1000);
        if (dev >= 0)
            INPUT_DeviceRumble(dev, false);
        usleep(160000);
    }
}

/* Each device's buttons as of the last INPUT_Update, and which are new since the call before. */
static u32 prev_raw[KEYS], prev_ext[KEYS];

static void read_devices(void (*each)(int index, const input_device *d, u32 new_raw, u32 new_ext, void *ctx),
                         void *ctx)
{
    int n = INPUT_DeviceCount();
    for (int i = 0; i < n; i++) {
        input_device d;
        if (!INPUT_DeviceGet(i, &d))
            continue;
        int key = (d.kind & 7) * 8 + (d.port & 7);
        u32 new_raw = d.raw & ~prev_raw[key], new_ext = d.raw_ext & ~prev_ext[key];
        prev_raw[key] = d.raw;
        prev_ext[key] = d.raw_ext;
        if (each)
            each(i, &d, new_raw, new_ext, ctx);
    }
}

/* ---------------------------------------------------------------- learning a button */

typedef struct {
    binding *b;
    bool got;
} learn_ctx;

static void learn_each(int index, const input_device *d, u32 new_raw, u32 new_ext, void *ctx)
{
    (void)index;
    learn_ctx *l = ctx;
    if (l->got)
        return;
    u32 bit = bindable(d->kind, false, NULL, new_raw);
    bool ext = false;
    if (!bit && d->raw_ext) {
        bit = bindable(d->kind, true, d->name, new_ext);
        ext = bit != 0;
    }
    if (!bit)
        return;
    memset(l->b, 0, sizeof *l->b);
    l->b->kind = d->kind;
    l->b->ext = ext;
    l->b->bit = bit;
    if (ext)
        copy(l->b->ext_name, sizeof l->b->ext_name, d->name);
    l->got = true;
}

static bool learn(const char *signal, binding *out)
{
    int cols = ui_cols(), rows = ui_rows();
    ui_clear();
    ui_color(C_CYAN, true);
    ui_field(1, 1, cols - 2, "IR REMOTE - ASSIGN A BUTTON");
    ui_color(C_WHITE, true);
    ui_field(4, 2, cols - 4, "Press the button for \"%s\"", signal);
    ui_color(C_WHITE, false);
    ui_field(6, 2, cols - 4, "Any button of any controller: a Wii Remote, Nunchuk, Classic,");
    ui_field(7, 2, cols - 4, "GameCube pad, Pro Controller, GamePad or USB pad.");
#ifndef __WIIU__
    ui_field(8, 2, cols - 4, "The Wii Remote's Power button works too (it turns that remote off).");
#endif
    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2, "Hold HOME (Start on a GameCube pad) Cancel");
    ui_reset();

    INPUT_Update();
    read_devices(NULL, NULL); /* what is already held (the A that chose this) does not count */
    INPUT_WiimotePowerPressed();
    learn_ctx l = {out, false};
    u64 home_at = 0;
    for (;;) {
        DetectInput(IN_DOWN);
        bool home = INPUT_ButtonsHeld() & WPAD_BUTTON_HOME;
        if (INPUT_WiimotePowerPressed()) {
            memset(out, 0, sizeof *out);
            out->kind = INPUT_DEV_WIIMOTE;
            out->bit = POWER_BIT;
            l.got = true;
            home = false;
        } else {
            read_devices(learn_each, &l);
        }
        /* the button that is HOME (or Start) counts once let go; held, it cancels */
        if (home && !home_at)
            home_at = now_ms();
        if (home && now_ms() - home_at > HOLD_TO_LEAVE_MS) {
            input_flush();
            return false;
        }
        if (!home && (l.got || home_at)) {
            if (!l.got) {
                home_at = 0;
                continue;
            }
            copy(out->signal, sizeof out->signal, signal);
            input_flush();
            return true;
        }
    }
}

/* ---------------------------------------------------------------- the remote */

typedef struct {
    preset *p;
    const ir_remote *r;
    int sent_bind, sent_key; /* the button being held, for repeats */
    u64 sent_at;
    char *status;
    size_t status_size;
    bool home;               /* HOME held: no codes, D-pad changes preset */
    bool home_new;           /* ...since this frame */
    int home_bind;           /* the binding of the button that is HOME, sent when it is let go quickly */
} remote_ctx;

static void remote_each(int index, const input_device *d, u32 new_raw, u32 new_ext, void *ctx)
{
    (void)index;
    remote_ctx *c = ctx;
    if (!new_raw && !new_ext)
        return;
    for (int i = 0; i < c->p->nbinds; i++) {
        const binding *b = &c->p->binds[i];
        if (b->kind != d->kind)
            continue;
        bool hit = b->ext ? !strcmp(d->name, b->ext_name) && (new_ext & b->bit) : (new_raw & b->bit) != 0;
        if (!hit)
            continue;
        if (c->home) { /* only the button that is HOME, pressed this frame, waits to be let go */
            if (c->home_new && c->home_bind < 0)
                c->home_bind = i;
            continue;
        }
        send(c->r, b->signal, false, c->status, c->status_size);
        c->sent_bind = i;
        c->sent_key = (d->kind & 7) * 8 + (d->port & 7);
        c->sent_at = now_ms();
        return;
    }
}

static const preset *sort_p;
static int by_binding(const void *a, const void *b)
{
    const binding *x = &sort_p->binds[*(const int *)a], *y = &sort_p->binds[*(const int *)b];
    int c = strcasecmp(x->signal, y->signal);
    if (!c) {
        char lx[48], ly[48];
        bind_label(x, lx, sizeof lx);
        bind_label(y, ly, sizeof ly);
        c = strcasecmp(lx, ly);
    }
    return c ? c : *(const int *)a - *(const int *)b;
}

/* HOME sends a code in this preset (so leaving takes holding it) */
static bool home_mapped(const preset *p)
{
    for (int i = 0; i < p->nbinds; i++) {
        char l[48];
        bind_label(&p->binds[i], l, sizeof l);
        size_t n = strlen(l);
        if ((n > 5 && !strcmp(l + n - 5, " HOME")) || (n > 6 && !strcmp(l + n - 6, " Start")))
            return true;
    }
    return false;
}

static void draw_remote(const preset *p, const char *status)
{
    int cols = ui_cols(), rows = ui_rows();
    ui_clear();
    ui_color(C_CYAN, true);
    ui_field(1, 1, cols - 2, "IR REMOTE  %d/%d  %s", current + 1, npresets, p->name);
    ui_color(C_WHITE, false);
    char dev[200];
    pretty(dev, sizeof dev, p->category);
    ui_field(2, 1, cols - 2, "%s: %s %s, from %s", dev, p->maker, p->device, SENDER);
    ui_fill(3, 0, cols - 1, '-');
    int row = 4, last = rows - 5;
    if (!p->nbinds) {
        ui_color(C_YELLOW, true);
        ui_field(row, 2, cols - 4, "No buttons are mapped yet: leave with HOME and pick \"Edit its buttons\".");
    }
    int order[MAX_BINDS];
    for (int i = 0; i < p->nbinds; i++)
        order[i] = i;
    sort_p = p;
    qsort(order, p->nbinds, sizeof *order, by_binding);
    for (int k = 0; k < p->nbinds && row <= last; k++, row++) {
        const binding *b = &p->binds[order[k]];
        char label[48];
        bind_label(b, label, sizeof label);
        ui_color(C_WHITE, false);
        ui_field(row, 2, 30, "%s", b->signal);
        ui_color(C_WHITE, true);
        ui_field(row, 33, cols - 35, "%s", label);
    }
    ui_color(C_GREEN, true);
    ui_field(rows - 3, 1, cols - 2, "%s", status);
    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2, "HOME+Left/Right Other preset (buzzes its number)   %s",
             home_mapped(p) ? "Hold HOME Leave" : "HOME Leave");
    ui_reset();
}

static void run_remote(void)
{
    if (!npresets) {
        ui_message("IR REMOTE", C_YELLOW, "There is no preset yet: make one with \"New preset\".");
        return;
    }
    if (!db_open())
        return;
    if (current >= npresets)
        current = 0;

    ir_remote r = {0};
    bool loaded = load_remote(&presets[current], &r);
    char status[96];
    snprintf(status, sizeof status, loaded ? "Ready" : "This preset's remote is not in the remote list");
    remote_ctx c = {&presets[current], &r, -1, 0, 0, status, sizeof status, false, false, -1};
    sfx_cancel();
    INPUT_Update();
    read_devices(NULL, NULL);
    INPUT_WiimotePowerPressed();

    bool dirty = true, home_used = false, home_was = false;
    u64 home_at = 0;
    for (;;) {
        if (dirty) {
            draw_remote(c.p, status);
            dirty = false;
        }
        u32 down = DetectInput(IN_DOWN);
        u32 held = INPUT_ButtonsHeld();
        c.home = held & WPAD_BUTTON_HOME;
        c.home_new = c.home && !home_was;
        if (c.home_new) {
            home_at = now_ms();
            c.home_bind = -1;
        }

        /* a Wii Remote's power button (that remote turns off; any button wakes it) */
        if (INPUT_WiimotePowerPressed() && loaded) {
            for (int i = 0; i < c.p->nbinds; i++)
                if (c.p->binds[i].kind == INPUT_DEV_WIIMOTE && c.p->binds[i].bit == POWER_BIT) {
                    send(&r, c.p->binds[i].signal, false, status, sizeof status);
                    dirty = true;
                    break;
                }
        }

        if (c.home && (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) && npresets > 1) {
            home_used = true;
            current = (current + ((down & WPAD_BUTTON_RIGHT) ? 1 : npresets - 1)) % npresets;
            IR_FreeRemote(&r);
            loaded = load_remote(&presets[current], &r);
            c.p = &presets[current];
            c.sent_bind = -1;
            c.home_bind = -1;
            snprintf(status, sizeof status, loaded ? "Preset %d: %s" : "Preset %d: its remote is missing",
                     current + 1, c.p->name);
            draw_remote(c.p, status);
            /* the controller that asked: one buzz per preset number, or a long one when it cannot be used */
            int who = -1, n = INPUT_DeviceCount();
            for (int i = 0; i < n && who < 0; i++) {
                input_device d;
                if (INPUT_DeviceGet(i, &d) && (d.held & WPAD_BUTTON_HOME))
                    who = i;
            }
            if (loaded)
                buzz(who, current + 1, 120);
            else
                buzz(who, 1, 700);
            presets_save();
            INPUT_Update();
            read_devices(NULL, NULL);
            continue;
        }
        if (home_was && !c.home) {
            if (!home_used) {
                /* a mapped HOME sends its code when tapped; held longer (or not mapped) it leaves */
                if (c.home_bind < 0 || now_ms() - home_at >= HOLD_TO_LEAVE_MS)
                    break;
                send(&r, c.p->binds[c.home_bind].signal, false, status, sizeof status);
                dirty = true;
            }
            home_used = false;
            c.home_bind = -1;
        }
        home_was = c.home;

        int before = c.sent_bind;
        u64 before_at = c.sent_at;
        if (loaded)
            read_devices(remote_each, &c);
        else
            read_devices(NULL, NULL);
        if (c.sent_bind != before || c.sent_at != before_at) {
            dirty = true;
            continue;
        }

        /* a mapped button still held: the code again, as a remote does */
        if (c.sent_bind >= 0) {
            const binding *b = &c.p->binds[c.sent_bind];
            u32 now_bits = b->ext ? prev_ext[c.sent_key] : prev_raw[c.sent_key];
            if (!(now_bits & b->bit) || c.home)
                c.sent_bind = -1;
            else if (now_ms() - c.sent_at > REPEAT_MS)
                send(&r, b->signal, true, status, sizeof status);
        }
    }
    IR_FreeRemote(&r);
    presets_save();
    input_flush();
}

/* ---------------------------------------------------------------- editing */

static char (*sort_names)[40];
static int by_signal_name(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b, c = strcasecmp(sort_names[x], sort_names[y]);
    return c ? c : x - y;
}

static void edit_buttons(preset *p)
{
    if (!db_open())
        return;
    ir_remote r;
    if (!load_remote(p, &r)) {
        ui_message("IR REMOTE", C_RED, "This preset's remote is not in irdb.dat any more.");
        return;
    }
    /* its buttons by name, A to Z; a name the remote has twice is "Name (2)" */
    char (*names)[40] = malloc(r.count * sizeof *names);
    int *order = malloc(r.count * sizeof *order);
    char (*labels)[112] = malloc(r.count * sizeof *labels);
    const char **items = malloc((r.count + 1) * sizeof *items);
    for (size_t i = 0; names && order && i < r.count; i++) {
        signal_label(&r, i, names[i], sizeof names[i]);
        order[i] = (int)i;
    }
    if (names && order) {
        sort_names = names;
        qsort(order, r.count, sizeof *order, by_signal_name);
    }
    char status[96] = "";
    int sel = 0;
    while (names && order && labels && items) {
        items[0] = "Done";
        for (size_t k = 0; k < r.count; k++) {
            const char *name = names[order[k]];
            char mapped[72] = "", one[48];
            int n = 0;
            for (int b = 0; b < p->nbinds; b++)
                if (!strcmp(p->binds[b].signal, name)) {
                    bind_label(&p->binds[b], one, sizeof one);
                    if (n++)
                        strncat(mapped, ", ", sizeof mapped - strlen(mapped) - 1);
                    strncat(mapped, one, sizeof mapped - strlen(mapped) - 1);
                }
            snprintf(labels[k], sizeof labels[k], "%-24.24s %s", name, n ? mapped : "-");
            items[k + 1] = labels[k];
        }
        ui_clear(); /* nothing left behind from a bigger window before */
        int c = ui_menu_ex(p->name, items, NULL, r.count + 1, sel, "A Choose   B Done");
        if (c <= 0)
            break;
        sel = c;
        const char *name = names[order[c - 1]];
        int has = 0;
        for (int b = 0; b < p->nbinds; b++)
            has += !strcmp(p->binds[b].signal, name);
        const char *acts[] = {has ? "Add another button for it" : "Assign a button", "Send it now (test)",
                              "Remove its buttons"};
        bool on[] = {true, true, has > 0};
        int a = ui_menu(name, acts, on, 3, 0);
        if (a == 0) {
            binding b;
            if (!learn(name, &b))
                continue;
            if (p->nbinds >= MAX_BINDS) {
                ui_message("IR REMOTE", C_YELLOW, "This preset has as many buttons as it can hold.");
                continue;
            }
            /* a button does one thing: take it from whatever it did before (a code can have several) */
            for (int k = 0; k < p->nbinds;) {
                binding *o = &p->binds[k];
                if (o->kind == b.kind && o->ext == b.ext && o->bit == b.bit && !strcmp(o->ext_name, b.ext_name))
                    p->binds[k] = p->binds[--p->nbinds];
                else
                    k++;
            }
            p->binds[p->nbinds++] = b;
            save_or_warn();
        } else if (a == 1) {
            send(&r, name, false, status, sizeof status);
            ui_message("IR REMOTE", C_GREEN, status);
        } else if (a == 2 && has) {
            for (int k = 0; k < p->nbinds;)
                if (!strcmp(p->binds[k].signal, name))
                    p->binds[k] = p->binds[--p->nbinds];
                else
                    k++;
            save_or_warn();
        }
    }
    free(names);
    free(order);
    free(labels);
    free(items);
    IR_FreeRemote(&r);
}

static void new_preset(void)
{
    if (npresets >= MAX_PRESETS) {
        ui_message("IR REMOTE", C_YELLOW, "There are 32 presets already: delete one first.");
        return;
    }
    if (!db_open())
        return;
    int d = pick_device();
    if (d < 0)
        return;
    preset *p = &presets[npresets];
    memset(p, 0, sizeof *p);
    const ir_db_device *dev = &db.devices[d];
    copy(p->category, sizeof p->category, dev->category);
    copy(p->maker, sizeof p->maker, dev->manufacturer);
    copy(p->device, sizeof p->device, dev->name);
    snprintf(p->name, sizeof p->name, "%s %s", dev->manufacturer, dev->name);
    if (!ui_text_edit("NAME THIS PRESET", p->name, sizeof p->name - 1) || !p->name[0])
        snprintf(p->name, sizeof p->name, "%s %s", dev->manufacturer, dev->name);
    current = npresets++;
    save_or_warn();
    edit_buttons(p);
}

static void preset_menu(int i)
{
    const char *acts[] = {"Use it", "Edit its buttons", "Rename", "Move up (earlier when cycling)", "Delete"};
    bool on[] = {true, true, true, i > 0, true};
    ui_clear();
    int a = ui_menu(presets[i].name, acts, on, 5, 0);
    switch (a) {
    case 0:
        current = i;
        run_remote();
        break;
    case 1:
        edit_buttons(&presets[i]);
        break;
    case 2: {
        char name[32];
        copy(name, sizeof name, presets[i].name);
        if (ui_text_edit("RENAME", name, sizeof name - 1) && name[0]) {
            copy(presets[i].name, sizeof presets[i].name, name);
            save_or_warn();
        }
        break;
    }
    case 3:
        if (i > 0) {
            preset t = presets[i];
            presets[i] = presets[i - 1];
            presets[i - 1] = t;
            if (current == i)
                current = i - 1;
            else if (current == i - 1)
                current = i;
            save_or_warn();
        }
        break;
    case 4:
        if (ui_confirm("DELETE PRESET", C_RED, "Delete this preset? Its button mapping is lost.")) {
            memmove(&presets[i], &presets[i + 1], (npresets - i - 1) * sizeof *presets);
            npresets--;
            if (current >= i && current > 0)
                current--;
            save_or_warn();
        }
        break;
    }
}

static const char HELP[] =
    "Sends TV, receiver and set-top box codes from " SENDER ", with the buttons of any controller.\n\n"
    "New preset: pick the device, then give each of its buttons a button on any controller (Wii Remote, "
    "Nunchuk, Classic, GameCube pad, Pro Controller, GamePad or USB pad).\n\n"
    "A code can have several buttons (on one controller or more): pick it again and add another. HOME "
#ifndef __WIIU__
    "and the Wii Remote's Power button can be mapped too (Power turns that remote off; any button wakes it). "
#else
    "can be mapped too. "
#endif
    "\n\nIn the remote: press a mapped button to send its code; hold it to repeat. HOME + Left / Right "
    "switches preset, and the controller buzzes once per preset number (1st = one buzz), so it works with "
    "the TV on another input. HOME on its own leaves (hold it for a second when HOME sends a code).\n\n"
    "Shortcut: hold 1 (Y on a GamePad, Classic or GameCube pad) while the app starts.\n\n"
    "More devices or buttons: add them to irdb_custom.xml in " APP_DIR "/ (it has examples to copy).";

/* The presets and the remote list only while the IR remote is open. */
static bool ir_open(void)
{
    if (!presets)
        presets = calloc(MAX_PRESETS, sizeof *presets);
    if (!presets) {
        ui_message("IR REMOTE", C_RED, "Not enough memory to open the IR remote.");
        return false;
    }
    return true;
}

static void ir_close(void)
{
    IR_DbFree(&db);
    free(presets);
    presets = NULL;
    npresets = 0;
}

void irremote_run(bool quick)
{
    if (!ir_open())
        return;
    presets_load();
    if (quick && npresets)
        run_remote();

    int sel = 0;
    for (;;) {
        const char *items[MAX_PRESETS + 2];
        char labels[MAX_PRESETS][96];
        int n = 0;
        for (int i = 0; i < npresets; i++, n++) {
            snprintf(labels[i], sizeof labels[i], "%s%.31s", i == current ? "> " : "  ", presets[i].name);
            items[n] = labels[i];
        }
        items[n++] = "New preset (pick a device)";
        items[n++] = "Help";
        ui_clear();
        int r = ui_menu_ex("IR REMOTE", items, NULL, n, sel, "A Select   B Back");
        if (r < 0)
            break;
        sel = r;
        if (r < npresets)
            preset_menu(r);
        else if (r == npresets)
            new_preset();
        else
            ui_message("IR REMOTE", C_CYAN, HELP);
    }
    ir_close();
}

bool irremote_shortcut_held(void)
{
    INPUT_Update();
    return (INPUT_ButtonsHeld() & WPAD_BUTTON_1) != 0;
}

#ifdef GMW_AUTOTEST
/* "@irremote" (test builds): every device of irdb.dat read and encoded, a timed send of a NEC and an RC6
 * code, and the remote's screen for the first preset, saved like a viewer's. Results in sd:/gmwtest.log. */
void irremote_autotest(void)
{
    extern bool ui_test_dump(void);
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg)
        return;
    u64 t0 = now_ms();
    IR_DbFree(&db);
    int added = IR_DbAdd(&db, APP_DIR "/irdb.dat");
    u64 t1 = now_ms();
    size_t buttons = 0, bad = 0, unencodable = 0;
    for (size_t i = 0; i < db.count; i++) {
        ir_remote r;
        if (IR_DbLoad(&db, i, &r) || r.count != db.devices[i].buttons) {
            bad++;
            continue;
        }
        for (size_t k = 0; k < r.count; k++) {
            static uint32_t t[4096];
            uint32_t freq;
            float duty;
            if (!IR_Encode(&r.signals[k], t, 4096, &freq, &duty))
                unencodable++;
        }
        buttons += r.count;
        IR_FreeRemote(&r);
    }
    u64 t2 = now_ms();
    fprintf(lg, "irremote db %d devices %u buttons %u bad %u unencodable %u index_ms %u load_all_ms %u\n", added,
            (unsigned)db.count, (unsigned)buttons, (unsigned)bad, (unsigned)unencodable, (unsigned)(t1 - t0),
            (unsigned)(t2 - t1));

    u64 s0 = now_ms();
    int nec = IR_SendCode(IR_PROTO_NEC, 0x04, 0x08);
    u64 s1 = now_ms();
    int rc6 = IR_SendCode(IR_PROTO_RC6, 0x00, 0x0C);
    u64 s2 = now_ms();
    fprintf(lg, "irremote send nec %d %u ms (110 expected), rc6 %d %u ms\n", nec, (unsigned)(s1 - s0), rc6,
            (unsigned)(s2 - s1));

    if (!ir_open()) {
        fclose(lg);
        return;
    }
    presets_load();
    fprintf(lg, "irremote presets %d\n", npresets);
    if (npresets) {
        ir_remote r;
        bool ok = load_remote(&presets[0], &r);
        fprintf(lg, "irremote preset0 %s loaded %d binds %d\n", presets[0].name, ok, presets[0].nbinds);
        if (ok)
            IR_FreeRemote(&r);
        current = 0;
        draw_remote(&presets[0], "Sent Power");
        ui_test_dump();
    }
    fclose(lg);
    ir_close();
}
#endif

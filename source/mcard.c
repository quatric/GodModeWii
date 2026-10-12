#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <ogc/card.h>

#include "mcard.h"

#define GCI_HEADER 0x40u
#define CHUNK      (32u * 1024)
#define SLOTS      2
#define CARD_READ_SIZE 0x200u   /* CARD_Read offset / length granularity */

typedef struct {
    char name[VFS_NAME_MAX];
    card_dir dir;
    u8 header[GCI_HEADER];   /* raw directory entry, the start of the .gci */
} save_t;

typedef struct {
    bool mounted;
    u8 *work;
    save_t *saves;
    int count;
} slot_t;

static slot_t slots[SLOTS];
static bool card_inited;
static u8 *bounce;

static int chan_of(int dev) { return dev == DEV_MCB ? 1 : 0; }

static void set_note(int dev, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_note(int dev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_devs[dev].note, sizeof(g_devs[dev].note), fmt, ap);
    va_end(ap);
}

static void clean(char *s)
{
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
              *s == ' ' || *s == '.' || *s == '_' || *s == '-'))
            *s = '_';
}

static void field(char *out, const u8 *in, int len)
{
    memcpy(out, in, len);
    out[len] = '\0';
    clean(out);
    for (int i = len - 1; i >= 0 && (out[i] == ' ' || out[i] == '_'); i--)
        out[i] = '\0';
}

static void forget(int dev)
{
    slot_t *s = &slots[chan_of(dev)];
    if (s->mounted)
        CARD_Unmount(chan_of(dev));
    s->mounted = false;
    free(s->saves);
    s->saves = NULL;
    s->count = 0;
    g_devs[dev].mounted = false;
    g_devs[dev].space_known = false;
}

static bool load_saves(int dev, u32 *used)
{
    int chn = chan_of(dev);
    slot_t *s = &slots[chn];
    save_t *l = calloc(CARD_MAXFILES, sizeof(save_t));
    if (!l) return false;
    int n = 0;
    *used = 0;

    card_dir dir;
    s32 r = CARD_FindFirst(chn, &dir, true);
    while (r == CARD_ERROR_READY && n < CARD_MAXFILES) {
        save_t *v = &l[n];
        v->dir = dir;
        char co[3], gc[5], fn[CARD_FILENAMELEN + 1];
        field(co, dir.company, 2);
        field(gc, dir.gamecode, 4);
        field(fn, (const u8 *)dir.filename, CARD_FILENAMELEN);
        snprintf(v->name, sizeof(v->name), "%s-%s-%s.gci", co, gc, fn);
        for (int i = 0; i < n; i++)    /* names are unique on a card, but stay safe */
            if (!strcasecmp(l[i].name, v->name)) {
                size_t len = strlen(v->name) - 4;
                snprintf(v->name + len, sizeof(v->name) - len, "_%u.gci", (unsigned)dir.fileno);
                break;
            }
        card_direntry ent;
        if (CARD_GetStatusEx(chn, dir.fileno, &ent) == CARD_ERROR_READY)
            memcpy(v->header, &ent, GCI_HEADER);
        *used += dir.filelen;
        n++;
        r = CARD_FindNext(&dir);
    }

    for (int a = 1; a < n; a++) {   /* by name, like every other drive */
        save_t t = l[a];
        int b = a;
        while (b > 0 && strcasecmp(l[b - 1].name, t.name) > 0) {
            l[b] = l[b - 1];
            b--;
        }
        l[b] = t;
    }
    s->saves = l;
    s->count = n;
    return true;
}

void mcard_refresh(int dev)
{
    int chn = chan_of(dev);
    slot_t *s = &slots[chn];
    forget(dev);
    g_devs[dev].note[0] = '\0';
    if (g_is_vwii) { set_note(dev, "not available in vWii"); return; }

    if (!card_inited) {
        CARD_Init(NULL, NULL);
        card_inited = true;
    }
    s32 mem = 0, sect = 0;
    s32 r = CARD_ProbeEx(chn, &mem, &sect);
    if (r == CARD_ERROR_NOCARD)     { set_note(dev, "no card"); return; }
    if (r == CARD_ERROR_WRONGDEVICE) { set_note(dev, "not a memory card"); return; }
    if (r < 0)                      { set_note(dev, "card not readable"); return; }

    if (!s->work)
        s->work = memalign(32, CARD_WORKAREA_SIZE);
    if (!s->work) { set_note(dev, "out of memory"); return; }

    r = CARD_ERROR_BUSY;
    for (int tries = 0; tries < 20 && (r == CARD_ERROR_BUSY || r == CARD_ERROR_IOERROR); tries++) {
        r = CARD_Mount(chn, s->work, NULL);
        if (r == CARD_ERROR_BUSY || r == CARD_ERROR_IOERROR)
            usleep(100 * 1000);
    }
    if (r == CARD_ERROR_BROKEN)     { set_note(dev, "card damaged"); return; }
    if (r < 0 && r != CARD_ERROR_ENCODING) { set_note(dev, "mount failed (%d)", (int)r); return; }
    s->mounted = true;

    u32 used = 0;
    if (!load_saves(dev, &used)) { set_note(dev, "out of memory"); return; }

    u32 sector = 0;
    if (CARD_GetSectorSize(chn, &sector) != CARD_ERROR_READY || !sector)
        sector = (u32)sect ? (u32)sect : 8192;
    vfs_dev *d = &g_devs[dev];
    d->mounted = true;
    /* 5 blocks hold the card's own directory / allocation tables */
    u64 total = (u64)mem * 131072;
    if (total > 5ull * sector) {
        d->total = total - 5ull * sector;
        d->free = d->total > used ? d->total - used : 0;
        d->space_known = true;
    }
    set_note(dev, "%d save%s, read-only", s->count, s->count == 1 ? "" : "s");
}

static save_t *find(int dev, const char *rel)
{
    slot_t *s = &slots[chan_of(dev)];
    if (!s->mounted) return NULL;
    while (*rel == '/') rel++;
    for (int i = 0; i < s->count; i++)
        if (!strcasecmp(s->saves[i].name, rel))
            return &s->saves[i];
    return NULL;
}

bool mcard_list(int dev, const char *rel, vfs_entry **out, int *count)
{
    *out = NULL;
    *count = 0;
    slot_t *s = &slots[chan_of(dev)];
    while (*rel == '/') rel++;
    if (!s->mounted || *rel) return false;
    vfs_entry *l = calloc(s->count ? s->count : 1, sizeof(vfs_entry));
    if (!l) return false;
    for (int i = 0; i < s->count; i++) {
        snprintf(l[i].name, sizeof(l[i].name), "%s", s->saves[i].name);
        l[i].size = (u64)s->saves[i].dir.filelen + GCI_HEADER;
    }
    *out = l;
    *count = s->count;
    return true;
}

bool mcard_stat(int dev, const char *rel, bool *is_dir, u64 *size)
{
    while (*rel == '/') rel++;
    if (!slots[chan_of(dev)].mounted) return false;
    if (!*rel) {
        if (is_dir) *is_dir = true;
        if (size) *size = 0;
        return true;
    }
    save_t *v = find(dev, rel);
    if (!v) return false;
    if (is_dir) *is_dir = false;
    if (size) *size = (u64)v->dir.filelen + GCI_HEADER;
    return true;
}

bool mcard_open(vfs_file *f, int dev, const char *rel)
{
    slot_t *s = &slots[chan_of(dev)];
    save_t *v = find(dev, rel);
    if (!v) return false;
    f->dev = dev;
    f->size = (u64)v->dir.filelen + GCI_HEADER;
    f->pos = 0;
    f->base = (u64)(v - s->saves);
    return true;
}

s32 mcard_read(vfs_file *f, void *buf, u32 len)
{
    int dev = f->dev;
    int chn = chan_of(dev);
    slot_t *s = &slots[chn];
    if (!s->mounted || f->base >= (u64)s->count) return -1;
    save_t *v = &s->saves[f->base];
    if (f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = (u32)(f->size - f->pos);
    if (!bounce && !(bounce = memalign(32, CHUNK))) return -1;

    u8 *dst = buf;
    u32 done = 0;
    while (done < len) {
        u64 pos = f->pos + done;
        if (pos < GCI_HEADER) {
            u32 n = GCI_HEADER - (u32)pos;
            if (n > len - done) n = len - done;
            memcpy(dst + done, v->header + pos, n);
            done += n;
            continue;
        }
        u32 off = (u32)(pos - GCI_HEADER);
        u32 start = off & ~(u32)(CARD_READ_SIZE - 1);
        u32 n = v->dir.filelen - start;
        if (n > CHUNK) n = CHUNK;
        card_file cf;
        s32 r = CARD_OpenEntry(chn, &v->dir, &cf);
        if (r == CARD_ERROR_READY) {
            r = CARD_Read(&cf, bounce, n, start);
            CARD_Close(&cf);
        }
        if (r != CARD_ERROR_READY) return done ? (s32)done : -1;
        u32 skip = off - start;
        u32 take = n - skip;
        if (take > len - done) take = len - done;
        memcpy(dst + done, bounce + skip, take);
        done += take;
    }
    f->pos += done;
    return (s32)done;
}

void mcard_shutdown(void)
{
    for (int d = 0; d < SLOTS; d++) {
        forget(d ? DEV_MCB : DEV_MCA);
        free(slots[d].work);
        slots[d].work = NULL;
    }
    free(bounce);
    bounce = NULL;
}

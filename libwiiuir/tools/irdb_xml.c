/*
 * Writes irdb.xml from a Flipper-IRDB checkout (host tool):
 *   make irdb IRDB=path/to/Flipper-IRDB      -> irdb.xml
 *
 * Folders are Category/Manufacturer/Device.ir; deeper folders become part of
 * the device's name. _Converted_ (remotes converted from other databases) is
 * filed under the category "Converted", by the folder holding each file.
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ir_platform.h"

int IR_SendTimings(uint32_t frequency, float duty_cycle, const uint32_t *timings, size_t count)
{
    (void)frequency, (void)duty_cycle, (void)timings, (void)count;
    return -1;
}

typedef struct {
    char *path, *category, *maker, *name;
} entry;

static entry *list;
static size_t count, cap;
static size_t root_len;
static bool converted; /* --converted: only _Converted_, else everything but it */

static int add(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st, (void)ftw;
    size_t len = strlen(path);
    if (type != FTW_F || len < 4 || strcmp(path + len - 3, ".ir") || strstr(path, "/.git/"))
        return 0;
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", path + root_len + (path[root_len] == '/'));
    rel[strlen(rel) - 3] = 0;

    char *parts[32];
    int n = 0;
    for (char *p = strtok(rel, "/"); p && n < 32; p = strtok(NULL, "/"))
        parts[n++] = p;
    if (n < 2 || !strcmp(parts[0], "_Converted_") != converted)
        return 0;

    entry e = {strdup(path), NULL, NULL, NULL};
    char name[1024] = "";
    if (!strcmp(parts[0], "_Converted_")) {
        e.category = strdup("Converted");
        /* a single letter is an alphabetical folder, not a manufacturer */
        bool folder = n >= 3 && strlen(parts[n - 2]) > 1;
        e.maker = strdup(folder ? parts[n - 2] : "Various");
        snprintf(name, sizeof name, "%s", parts[n - 1]);
    } else {
        e.category = strdup(parts[0]);
        e.maker = strdup(n >= 3 ? parts[1] : "Various");
        for (int i = n >= 3 ? 2 : 1; i < n; i++) {
            strncat(name, parts[i], sizeof name - strlen(name) - 2);
            if (i + 1 < n)
                strcat(name, "/");
        }
    }
    e.name = strdup(name);
    if (count == cap) {
        cap = cap ? cap * 2 : 1024;
        list = realloc(list, cap * sizeof *list);
    }
    list[count++] = e;
    return 0;
}

static int by_maker(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    int c = strcasecmp(x->maker, y->maker);
    if (!c)
        c = strcasecmp(x->category, y->category);
    if (!c)
        c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->path, y->path);
}

static void esc(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break;
        default:
            if ((unsigned char)*s >= 0x20 || *s == '\t')
                fputc(*s, f);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--converted")) {
        converted = true;
        argv++;
        argc--;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s [--converted] <Flipper-IRDB folder> <irdb.xml> [source note]\n", argv[0]);
        return 1;
    }
    root_len = strlen(argv[1]);
    if (nftw(argv[1], add, 32, FTW_PHYS) || !count) {
        fprintf(stderr, "no .ir files under %s\n", argv[1]);
        return 1;
    }
    qsort(list, count, sizeof *list, by_maker);

    FILE *f = fopen(argv[2], "w");
    if (!f)
        return 1;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<!-- Infrared remotes for libwiiir / libwiiuir, from Flipper-IRDB (CC0)%s%s.\n"
               "     Add your own: a <ButtonEntry> in a device, or a whole <DeviceEntry>. <Data> is\n"
               "     Protocol:address,command (Flipper's values, decimal or 0x hex),\n"
               "     timings in microseconds with frequency=\"38000\" duty=\"0.33\", or RAW:<Pronto hex>. -->\n"
               "<Manufacturers>\n",
            argc > 3 ? " " : "", argc > 3 ? argv[3] : "");

    size_t devices = 0, buttons = 0;
    for (size_t i = 0; i < count;) {
        /* one <Manufacturer> per name, whatever its case, listing its categories */
        size_t j = i;
        char cats[4096] = "";
        while (j < count && !strcasecmp(list[j].maker, list[i].maker)) {
            if (!strstr(cats, list[j].category) || j == i) {
                if (cats[0])
                    strncat(cats, " ", sizeof cats - strlen(cats) - 1);
                strncat(cats, list[j].category, sizeof cats - strlen(cats) - 1);
            }
            j++;
        }
        fputs("  <Manufacturer name=\"", f);
        esc(f, list[i].maker);
        fputs("\" deviceCategories=\"", f);
        esc(f, cats);
        fputs("\">\n    <DeviceList>\n", f);

        for (size_t k = i; k < j; k++) {
            ir_remote r;
            if (IR_LoadRemote(list[k].path, &r))
                continue;
            fputs("      <DeviceEntry name=\"", f);
            esc(f, list[k].name);
            fputs("\" category=\"", f);
            esc(f, list[k].category);
            fputs("\">\n", f);
            for (size_t s = 0; s < r.count; s++) {
                const ir_signal *g = &r.signals[s];
                fputs("        <ButtonEntry name=\"", f);
                esc(f, g->name);
                if (g->protocol == IR_PROTO_RAW) {
                    fprintf(f, "\"><Data frequency=\"%u\" duty=\"%g\">", (unsigned)g->frequency,
                            g->duty_cycle > 0.0f ? g->duty_cycle : 0.33f);
                    for (size_t t = 0; t < g->count; t++)
                        fprintf(f, t ? " %u" : "%u", (unsigned)g->timings[t]);
                } else {
                    fprintf(f, "\"><Data>%s:0x%X,0x%X", IR_ProtocolName(g->protocol),
                            (unsigned)g->address, (unsigned)g->command);
                }
                fputs("</Data></ButtonEntry>\n", f);
                buttons++;
            }
            fputs("      </DeviceEntry>\n", f);
            devices++;
            IR_FreeRemote(&r);
        }
        fputs("    </DeviceList>\n  </Manufacturer>\n", f);
        i = j;
    }
    fputs("</Manufacturers>\n", f);
    if (fclose(f))
        return 1;
    printf("%zu devices, %zu buttons\n", devices, buttons);
    return 0;
}

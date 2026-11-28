/*
 * Reader for Flipper Zero .ir files, the format of Flipper-IRDB.
 * Shared, unchanged, between libwiiir and libwiiuir.
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ir_platform.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

/* "00 7F 00 00" is little endian: 0x7F00. */
static uint32_t hex_le(const char *s)
{
    uint32_t v = 0;
    char *end;
    for (int shift = 0; shift < 32; shift += 8) {
        unsigned long byte = strtoul(s, &end, 16);
        if (end == s)
            break;
        v |= (uint32_t)(byte & 0xFF) << shift;
        s = end;
    }
    return v;
}

/* Appends the numbers of a "data:" line (long signals may span several). */
static int add_timings(ir_signal *s, const char *text)
{
    size_t n = 0;
    for (const char *p = text; *p;) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        n++;
        while (*p && !isspace((unsigned char)*p))
            p++;
    }
    if (!n)
        return 0;
    uint32_t *t = realloc(s->timings, (s->count + n) * sizeof *t);
    if (!t)
        return -1;
    s->timings = t;
    char *end;
    for (const char *p = text; n--; p = end)
        t[s->count++] = strtoul(p, &end, 10);
    return 0;
}

/* Keeps the signal if it is complete, else frees it. */
static int finish(ir_remote *r, ir_signal *s, int proto)
{
    bool ok = s->name[0] && proto >= 0 && (proto != IR_PROTO_RAW || s->count);
    if (ok) {
        ir_signal *list = realloc(r->signals, (r->count + 1) * sizeof *list);
        if (!list) {
            free(s->timings);
            return -1;
        }
        s->protocol = (ir_protocol)proto;
        r->signals = list;
        r->signals[r->count++] = *s;
    } else {
        free(s->timings);
    }
    memset(s, 0, sizeof *s);
    return 0;
}

int IR_ParseRemote(const char *text, ir_remote *remote)
{
    if (!remote)
        return -1;
    memset(remote, 0, sizeof *remote);
    char *copy = text ? strdup(text) : NULL;
    if (!copy)
        return -1;

    ir_signal cur;
    memset(&cur, 0, sizeof cur);
    int proto = -1, err = 0;
    for (char *line = copy, *next; line && !err; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *colon = strchr(line, ':');
        if (!colon || line[0] == '#')
            continue;
        *colon = 0;
        char *key = trim(line), *val = trim(colon + 1);

        if (!strcmp(key, "name")) {
            err = finish(remote, &cur, proto);
            proto = -1;
            strncpy(cur.name, val, sizeof cur.name - 1);
        } else if (!strcmp(key, "type")) {
            proto = strcasecmp(val, "raw") ? -1 : IR_PROTO_RAW;
        } else if (!strcmp(key, "protocol")) {
            proto = IR_ProtocolFromName(val);
        } else if (!strcmp(key, "address")) {
            cur.address = hex_le(val);
        } else if (!strcmp(key, "command")) {
            cur.command = hex_le(val);
        } else if (!strcmp(key, "frequency")) {
            cur.frequency = strtoul(val, NULL, 10);
        } else if (!strcmp(key, "duty_cycle")) {
            cur.duty_cycle = strtof(val, NULL);
        } else if (!strcmp(key, "data")) {
            err = add_timings(&cur, val);
        }
    }
    if (!err)
        err = finish(remote, &cur, proto);
    else
        free(cur.timings);
    free(copy);

    if (err || !remote->count) {
        IR_FreeRemote(remote);
        return -1;
    }
    return 0;
}

int IR_LoadRemote(const char *path, ir_remote *remote)
{
    if (remote)
        memset(remote, 0, sizeof *remote);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return -1;
    char *text = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) &&
        (text = malloc(size + 1)) && fread(text, 1, size, f) == (size_t)size)
        text[size] = 0;
    else
        size = -1;
    fclose(f);
    int ret = size < 0 ? -1 : IR_ParseRemote(text, remote);
    free(text);
    return ret;
}

void IR_FreeRemote(ir_remote *remote)
{
    if (!remote)
        return;
    for (size_t i = 0; i < remote->count; i++)
        free(remote->signals[i].timings);
    free(remote->signals);
    remote->signals = NULL;
    remote->count = 0;
}

const ir_signal *IR_FindSignal(const ir_remote *remote, const char *name)
{
    for (size_t i = 0; remote && name && i < remote->count; i++)
        if (!strcasecmp(remote->signals[i].name, name))
            return &remote->signals[i];
    return NULL;
}

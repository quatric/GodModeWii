/*
 * Host test of the encoders and the .ir reader: encodes codes, decodes the
 * timings back and checks them against known frames.
 *   make test                          the checks below
 *   build/ir_test path/to/Flipper-IRDB also loads and encodes every .ir file
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>

#include "ir_platform.h"

static uint32_t sent_freq;
static size_t sent_count;
int IR_SendTimings(uint32_t frequency, float duty_cycle, const uint32_t *timings, size_t count)
{
    (void)duty_cycle;
    (void)timings;
    sent_freq = frequency;
    sent_count = count;
    return 0;
}

static uint32_t t[IR_MAX_TIMINGS], freq;
static float duty;

static size_t enc(ir_protocol p, uint32_t a, uint32_t c)
{
    ir_signal s = {.protocol = p, .address = a, .command = c};
    return IR_Encode(&s, t, IR_MAX_TIMINGS, &freq, &duty);
}

/* Pulse distance (by space) or pulse width (by mark) bits after a header at t[at]. */
static uint64_t pd_bits(size_t at, int len, uint32_t threshold, bool by_mark)
{
    uint64_t v = 0;
    for (int i = 0; i < len; i++) {
        uint32_t x = by_mark ? t[at + 2 + 2 * i] : t[at + 3 + 2 * i];
        if (x > threshold)
            v |= 1ull << i;
    }
    return v;
}

/* Expands timings to half-bit levels; RC5's dropped leading space comes back. */
static int halves(size_t from, size_t n, uint32_t half, bool lead_space, bool *lv, int max)
{
    int k = 0;
    memset(lv, 0, max * sizeof *lv);
    if (lead_space)
        lv[k++] = false;
    for (size_t i = from; i < n && k < max; i++) {
        int units = (t[i] + half / 2) / half;
        for (int u = 0; u < units && k < max; u++)
            lv[k++] = i % 2 == 0;
    }
    return k;
}

static uint32_t frame_len(size_t from, size_t to)
{
    uint32_t sum = 0;
    for (size_t i = from; i < to; i++)
        sum += t[i];
    return sum;
}

static int files, signals, failed, dark;
static int each_file(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st, (void)ftw;
    size_t len = strlen(path);
    if (type != FTW_F || len < 3 || strcmp(path + len - 3, ".ir"))
        return 0;
    ir_remote r;
    if (IR_LoadRemote(path, &r))
        return 0;
    files++;
    for (size_t i = 0; i < r.count; i++) {
        const ir_signal *s = &r.signals[i];
        uint32_t big[4096], marks = 0;
        signals++;
        for (size_t j = 0; s->protocol == IR_PROTO_RAW && j < s->count; j += 2)
            marks += s->timings[j];
        if (s->protocol == IR_PROTO_RAW && !marks) {
            dark++; /* broken capture in the database: every mark is 0 */
        } else if (!IR_Encode(s, big, 4096, NULL, NULL)) {
            failed++;
            printf("cannot encode %s: %s (%s)\n", path, r.signals[i].name,
                   IR_ProtocolName(r.signals[i].protocol));
        }
    }
    IR_FreeRemote(&r);
    return 0;
}

int main(int argc, char **argv)
{
    size_t n;

    /* NEC: address, ~address, command, ~command; 110 ms frame period */
    n = enc(IR_PROTO_NEC, 0x04, 0x08);
    assert(n == 68 && t[0] == 9000 && t[1] == 4500 && freq == 38000);
    assert(pd_bits(0, 32, 1000, false) == 0xF708FB04);
    assert(frame_len(0, n) == 110000);

    n = enc(IR_PROTO_NECEXT, 0x7F00, 0xEA15);
    assert(pd_bits(0, 32, 1000, false) == 0xEA157F00);

    n = enc(IR_PROTO_NEC42, 0x1ABC, 0x5A);
    assert(n == 88);
    assert(pd_bits(0, 42, 1000, false) ==
           (0x1ABC | (uint64_t)(~0x1ABC & 0x1FFF) << 13 | 0x5Aull << 26 | (uint64_t)0xA5 << 34));

    n = enc(IR_PROTO_SAMSUNG32, 0x07, 0x02);
    assert(t[0] == 4500 && t[1] == 4500);
    assert(pd_bits(0, 32, 1000, false) == 0xFD020707);

    /* Panasonic TV power from Flipper-IRDB is the well-known 02 20 80 00 3D BD frame */
    n = enc(IR_PROTO_KASEIKYO, 0x00200280, 0x03D0);
    assert(t[0] == 3456 && t[1] == 1728 && n == 100);
    assert(pd_bits(0, 48, 800, false) == 0xBD3D00802002ull);

    n = enc(IR_PROTO_RCA, 0x5, 0x9C);
    assert(t[0] == 4000 && pd_bits(0, 24, 1500, false) == (0x5 | 0x9C << 4 | 0xA << 12 | 0x63 << 16));

    /* Sony: 3 frames, 45 ms apart, LSB first, mark length carries the bit */
    n = enc(IR_PROTO_SIRC, 0x01, 0x15);
    assert(freq == 40000 && n == 3 * 26);
    assert(pd_bits(0, 12, 900, true) == (0x15 | 1 << 7));
    assert(frame_len(0, 26) == 45000 && frame_len(26, 52) == 45000);
    n = enc(IR_PROTO_SIRC20, 0x1234, 0x7F);
    assert(pd_bits(0, 20, 900, true) == (0x7F | 0x1234 << 7));

    n = enc(IR_PROTO_PIONEER, 0xAA, 0x38);
    assert(freq == 40000 && n == 2 * 68 && t[0] == 8500 && t[68] == 8500);
    assert(pd_bits(0, 32, 1000, false) == 0xC73855AAull);

    n = enc(IR_PROTO_JVC, 0x03, 0x17);
    assert(pd_bits(0, 16, 1000, false) == (0x03 | 0x17 << 8));

    /* RC5: 1, field, toggle, 5 address, 6 command; space-mark is a one */
    bool lv[128];
    for (int press = 0; press < 2; press++) {
        n = enc(IR_PROTO_RC5, 0x00, 0x0C);
        assert(freq == 36000);
        int k = halves(0, n - 1, 889, true, lv, 128);
        assert(k >= 27);
        uint32_t v = 0;
        for (int i = 0; i < 14; i++)
            v = v << 1 | (!lv[2 * i] && lv[2 * i + 1]);
        assert(v == (0x3000u | (press ? 0x800u : 0) | 0x0C));
    }
    n = enc(IR_PROTO_RC5X, 0x08, 0x13); /* field cleared: command 0x53 */
    int k = halves(0, n - 1, 889, true, lv, 128);
    assert(k >= 27);
    uint32_t v = 0;
    for (int i = 0; i < 14; i++)
        v = v << 1 | (!lv[2 * i] && lv[2 * i + 1]);
    assert((v & 0x2000) && !(v & 0x1000) && ((v >> 6) & 0x1F) == 0x08 && (v & 0x3F) == 0x13);

    /* RC6 mode 0: leader, 1, 000, toggle (double), address, command; mark-space is a one */
    n = enc(IR_PROTO_RC6, 0x00, 0x0C);
    assert(t[0] == 2666 && t[1] == 889 && t[2] == 444);
    k = halves(0, n - 1, 444, false, lv, 128);
    /* leader = 6 mark + 2 space units, start bit, 3 mode bits, 4-unit toggle */
    assert(lv[8] && !lv[9]);
    v = 0;
    for (int i = 0; i < 16; i++)
        v = v << 1 | (lv[20 + 2 * i] && !lv[21 + 2 * i]);
    assert(v == 0x000C);

    /* raw: copied, equal levels merged */
    uint32_t raw[] = {9000, 4500, 560, 0, 560, 560};
    ir_signal rs = {.protocol = IR_PROTO_RAW, .timings = raw, .count = 6, .frequency = 36000};
    n = IR_Encode(&rs, t, IR_MAX_TIMINGS, &freq, &duty);
    assert(n == 4 && t[2] == 1120 && t[3] == 560 && freq == 36000 && duty > 0.32f);
    assert(IR_Encode(&rs, t, 3, NULL, NULL) == 0); /* does not fit */

    /* Pronto learned code */
    uint16_t pronto[] = {0x0000, 0x006C, 0x0002, 0x0000, 0x0157, 0x00AC, 0x0015, 0x0016};
    n = IR_ProntoToTimings(pronto, 8, t, IR_MAX_TIMINGS, &freq);
    assert(n == 4 && freq > 38300 && freq < 38450 && t[0] > 8900 && t[0] < 8960);
    assert(IR_ProntoToTimings(pronto, 6, t, IR_MAX_TIMINGS, &freq) == 0);

    /* .ir reader: headers, comments, CRLF, multi-line data, unknown protocol skipped */
    const char *file =
        "Filetype: IR signals file\r\nVersion: 1\r\n#\r\n"
        "name: Power\r\ntype: parsed\r\nprotocol: NECext\r\naddress: 00 7F 00 00\r\ncommand: 15 EA 00 00\r\n"
        "# \nname: Vol_up\ntype: raw\nfrequency: 38000\nduty_cycle: 0.330000\ndata: 9000 4500 560\ndata: 560 560\n"
        "#\nname: Odd\ntype: parsed\nprotocol: Unknown\naddress: 01 00 00 00\ncommand: 01 00 00 00\n"
        "#\nname: Mute\ntype: parsed\nprotocol: Samsung32\naddress: 07 00 00 00\ncommand: 0F 00 00 00\n";
    ir_remote r;
    assert(IR_ParseRemote(file, &r) == 0 && r.count == 3);
    const ir_signal *s = IR_FindSignal(&r, "power");
    assert(s && s->protocol == IR_PROTO_NECEXT && s->address == 0x7F00 && s->command == 0xEA15);
    s = IR_FindSignal(&r, "Vol_up");
    assert(s && s->protocol == IR_PROTO_RAW && s->count == 5 && s->timings[4] == 560 && s->frequency == 38000);
    assert(!IR_FindSignal(&r, "Odd") && IR_FindSignal(&r, "MUTE"));
    assert(IR_Send(IR_FindSignal(&r, "Mute")) == 0 && sent_freq == 38000 && sent_count == 68);
    assert(IR_Send(s) == 0 && sent_count == 5);
    IR_FreeRemote(&r);
    assert(IR_ParseRemote("Filetype: IR signals file\n", &r) == -1);
    assert(IR_LoadRemote("/nonexistent.ir", &r) == -1);

    assert(IR_ProtocolFromName("necext") == IR_PROTO_NECEXT && IR_ProtocolFromName("x") == -1);
    assert(IR_SendCode(IR_PROTO_SIRC15, 0x1A, 0x15) == 0 && sent_freq == 40000);

    /* RC5 held down: the same toggle bit as the press before it */
    ir_signal rc5 = {.protocol = IR_PROTO_RC5, .command = 0x0C};
    n = IR_Encode(&rc5, t, IR_MAX_TIMINGS, NULL, NULL);
    halves(0, n - 1, 889, true, lv, 128);
    bool first = !lv[4] && lv[5];
    assert(IR_SendRepeat(&rc5) == 0 && IR_SendRepeat(&rc5) == 0);
    n = IR_Encode(&rc5, t, IR_MAX_TIMINGS, NULL, NULL); /* a new press flips it */
    halves(0, n - 1, 889, true, lv, 128);
    assert((!lv[4] && lv[5]) != first); /* press, repeat, repeat (same toggle), then a new press flips it */

    /* irdb.xml: WiiIR's schema, the three <Data> forms, entities, comments, a second file */
    const char *xml =
        "<?xml version=\"1.0\"?>\n<!-- a comment with <DeviceEntry> in it -->\n<Manufacturers>\n"
        " <Manufacturer name=\"B&amp;O\" deviceCategories=\"TVs\"><DeviceList>\n"
        "  <DeviceEntry name=\"Beo &lt;4&gt;\" category=\"TVs\">\n"
        "   <ButtonEntry name=\"Power\"><Maps/><Data>NECext:0x7F00,0xEA15</Data></ButtonEntry>\n"
        "   <ButtonEntry name='Vol_up'><Data frequency=\"36000\" duty=\"0.25\">9000 4500\n 560</Data></ButtonEntry>\n"
        "   <ButtonEntry name=\"Mute\"><Data>RAW:0000 006C 0002 0000 0157 00AC 0015 0016</Data></ButtonEntry>\n"
        "   <ButtonEntry name=\"Old\"><Data>NEC:4,8</Data></ButtonEntry>\n"
        "   <ButtonEntry name=\"Bad\"><Data>Nope:1,2</Data></ButtonEntry>\n"
        "  </DeviceEntry>\n"
        "  <DeviceEntry name=\"Empty\" category=\"TVs\"/>\n"
        " </DeviceList></Manufacturer>\n"
        " <Manufacturer name=\"Sony\"><DeviceList><DeviceEntry name=\"RM\" category=\"tvs\">"
        "<ButtonEntry name=\"Power\"><Data>SIRC:1,21</Data></ButtonEntry></DeviceEntry></DeviceList></Manufacturer>\n"
        "</Manufacturers>\n";
    const char *custom = "<Manufacturers><Manufacturer name=\"Mine\"><DeviceList>"
                         "<DeviceEntry name=\"Lamp\" category=\"LED_Lighting\">"
                         "<ButtonEntry name=\"On\"><Data>NEC:0,64</Data></ButtonEntry>"
                         "</DeviceEntry></DeviceList></Manufacturer></Manufacturers>";
    FILE *xf = fopen("build/test_irdb.xml", "w");
    fputs(xml, xf);
    fclose(xf);
    xf = fopen("build/test_custom.xml", "w");
    fputs(custom, xf);
    fclose(xf);

    ir_db db = {0};
    assert(IR_DbAdd(&db, "build/test_irdb.xml") == 0 && db.count == 2);
    assert(IR_DbAdd(&db, "build/test_custom.xml") == 0 && db.count == 3 && db.file_count == 2);
    assert(IR_DbAdd(&db, "build/nonexistent.xml") == -1 && db.count == 3);
    assert(!strcmp(db.devices[0].manufacturer, "B&O") && !strcmp(db.devices[0].name, "Beo <4>"));
    assert(db.devices[0].buttons == 5 && db.devices[0].category == db.devices[1].category); /* "TVs" = "tvs" */
    int beo = IR_DbFind(&db, "tvs", "b&o", "BEO <4>");
    assert(beo == 0 && IR_DbFind(&db, NULL, NULL, "Lamp") == 2 && IR_DbFind(&db, "ACs", NULL, "Lamp") == -1);

    assert(IR_DbLoad(&db, beo, &r) == 0 && r.count == 4); /* "Bad" is left out */
    s = IR_FindSignal(&r, "Power");
    assert(s->protocol == IR_PROTO_NECEXT && s->address == 0x7F00 && s->command == 0xEA15);
    s = IR_FindSignal(&r, "Vol_up");
    assert(s->protocol == IR_PROTO_RAW && s->count == 3 && s->timings[2] == 560 && s->frequency == 36000 &&
           s->duty_cycle == 0.25f);
    s = IR_FindSignal(&r, "Mute");
    assert(s->protocol == IR_PROTO_RAW && s->count == 4 && s->frequency > 38300 && s->timings[0] > 8900);
    s = IR_FindSignal(&r, "Old");
    assert(s->protocol == IR_PROTO_NEC && s->address == 4 && s->command == 8);
    IR_FreeRemote(&r);
    assert(IR_DbLoad(&db, 2, &r) == 0 && r.count == 1 && r.signals[0].command == 64);
    IR_FreeRemote(&r);
    assert(IR_DbLoad(&db, 3, &r) == -1);
    IR_DbFree(&db);

    /* build/ir_test irdb.xml [more.xml]: load every device, button counts must match */
    if (argc > 1 && strstr(argv[1], ".xml")) {
        ir_db all = {0};
        size_t buttons = 0, bad = 0;
        for (int i = 1; i < argc; i++)
            assert(IR_DbAdd(&all, argv[i]) == 0);
        for (size_t i = 0; i < all.count; i++) {
            if (IR_DbLoad(&all, i, &r) || r.count != all.devices[i].buttons)
                bad++;
            buttons += r.count;
            IR_FreeRemote(&r);
        }
        printf("%zu devices, %zu buttons, %zu not matching\n", all.count, buttons, bad);
        IR_DbFree(&all);
        return bad != 0;
    }

    if (argc > 1) {
        nftw(argv[1], each_file, 16, FTW_PHYS);
        printf("%d files, %d signals, %d not encodable, %d with no marks\n", files, signals, failed, dark);
        if (failed)
            return 1;
    }
    puts("ok");
    return 0;
}

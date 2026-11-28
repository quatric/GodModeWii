/*
 * GodModeWii - set the clock from the internet (SNTP, RFC 4330), like the SNTP homebrew app.
 * Copyright (C) 2026 quatric. GPL-3.0-or-later, see LICENSE.
 *
 * The Wii keeps its RTC as it is and shows RTC + a counter bias, which Wii Settings changes when the clock is set:
 * SYSCONF's IPL.CB (seconds, 2000-based). So the time server's UTC, plus the time zone, minus the RTC, goes into
 * IPL.CB, and the time this program shows is moved along with it. The Wii has no time zone setting; the one in use
 * is worked out from how far the clock is from UTC now (to the nearest quarter hour), and can be changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <gccore.h>
#include <network.h>
#include <ogc/isfs.h>
#include <ogc/lwp_watchdog.h>

#include "sntp.h"
#include "ftp.h"
#include "ui.h"
#include "vfs.h"

extern u32 __SYS_GetRTC(u32 *gctime);
extern void __SYS_SetTime(s64 time);
extern void __SYS_SetPosixTime(s64 time);

#define T "SET THE CLOCK"
#define NTP_1900_2000 3155673600u          /* seconds from 1900 (NTP) to 2000 (the Wii) */
#define UNIX_2000 946684800u
#define SYSCONF "/shared2/sys/SYSCONF"

/* UTC from pool.ntp.org, in seconds since 2000; false when no server answered. */
static bool ask(u32 *utc2000)
{
    static const char *const hosts[] = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" };
    for (int h = 0; h < 3; h++) {
        struct hostent *he = net_gethostbyname(hosts[h]);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) continue;
        s32 s = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) continue;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(123);
        memcpy(&a.sin_addr, he->h_addr_list[0], 4);
        u8 pkt[48] = { 0x1B };                  /* LI 0, version 3, client */
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            if (net_sendto(s, pkt, 48, 0, (struct sockaddr *)&a, sizeof(a)) != 48) break;
            struct pollsd p = { s, POLLIN, 0 };
            if (net_poll(&p, 1, 2000) <= 0) continue;
            u8 r[48];
            if (net_recv(s, r, 48, 0) < 48 || (r[0] & 7) != 4 || !r[1]) continue;   /* a server's reply, synced */
            u32 secs = (u32)r[40] << 24 | r[41] << 16 | r[42] << 8 | r[43];
            *utc2000 = secs - NTP_1900_2000;
            ok = true;
        }
        net_close(s);
        if (ok) return true;
    }
    return false;
}

/* IPL.CB in the SYSCONF image: a "long" item (type 4) */
static u8 *find_cb(u8 *b)
{
    u16 n = b[4] << 8 | b[5];
    if (memcmp(b, "SCv0", 4) || n > 0x400) return NULL;
    for (u16 i = 0; i < n; i++) {
        u16 off = b[6 + i * 2] << 8 | b[7 + i * 2];
        if (off < 6 || off > 0x4000 - 12) continue;
        u8 t = b[off], len = (t & 0x1F) + 1;
        if (t >> 5 == 4 && len == 6 && !memcmp(b + off + 1, "IPL.CB", 6)) return b + off + 7;
    }
    return NULL;
}

static void fmt_time(char *out, size_t n, u32 t2000)
{
    time_t t = (time_t)t2000 + UNIX_2000;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void sntp_run(void)
{
    u32 rtc;
    if (!__SYS_GetRTC(&rtc)) { ui_message(T, C_RED, "The clock chip couldn't be read."); return; }
    u8 *b = memalign(32, 0x4000);
    s32 fd = b ? ISFS_Open(SYSCONF, ISFS_OPEN_READ) : -1;
    bool rd = fd >= 0 && ISFS_Read(fd, b, 0x4000) == 0x4000;
    if (fd >= 0) ISFS_Close(fd);
    u8 *cb = rd ? find_cb(b) : NULL;
    if (!cb) { free(b); ui_message(T, C_RED, "The system settings (SYSCONF) couldn't be read."); return; }
    u32 bias = (u32)cb[0] << 24 | cb[1] << 16 | cb[2] << 8 | cb[3];

    if (!ftp_net_start()) { free(b); return; }
    ui_splash("Asking the time server...");
    u32 utc;
    if (!ask(&utc)) {
        free(b);
        ui_message(T, C_RED, "No time server answered (pool.ntp.org, time.google.com, time.cloudflare.com).");
        return;
    }
    /* the zone in use: how far the Wii's clock is from UTC, to the nearest 15 minutes */
    s32 diff = (s32)(rtc + bias - utc);
    s32 zone = (diff >= 0 ? diff + 450 : diff - 450) / 900 * 900;
    if (zone < -12 * 3600 || zone > 14 * 3600) zone = 0;

    for (;;) {
        char now[32], want[32], msg[400];
        fmt_time(now, sizeof(now), rtc + bias);
        fmt_time(want, sizeof(want), utc + zone);
        snprintf(msg, sizeof(msg), "The Wii's clock:   %s\nThe internet time: %s   (UTC%+d:%02d)\n\nSet the clock? "
                 "Only the clock setting in SYSCONF changes, as Wii Settings does it.",
                 now, want, zone / 3600, abs(zone) % 3600 / 60);
        ui_message(T, C_CYAN, msg);
        const char *opts[] = { "Set the clock", "A different time zone...", "Cancel" };
        int c = ui_menu(T, opts, NULL, 3, 0);
        if (c == 1) {
            static char names[57][16];
            const char *zp[57];
            int pick = 0;
            for (int i = 0; i < 57; i++) {
                s32 z = (i - 24) * 1800;
                snprintf(names[i], sizeof(names[i]), "UTC%+d:%02d", z / 3600, abs(z) % 3600 / 60);
                zp[i] = names[i];
                if (z == zone - zone % 1800) pick = i;
            }
            int z = ui_menu("TIME ZONE", zp, NULL, 57, pick);
            if (z >= 0) zone = (z - 24) * 1800;
            continue;
        }
        if (c != 0) { free(b); return; }
        break;
    }
    /* the old file next to the program's settings, then the new bias in place */
    FILE *bk = fopen("sd:/GodModeWii/SYSCONF.bak", "wb");
    if (bk) { fwrite(b, 1, 0x4000, bk); fclose(bk); }
    u32 nb = utc + zone - rtc;
    cb[0] = nb >> 24, cb[1] = nb >> 16, cb[2] = nb >> 8, cb[3] = nb;
    fd = ISFS_Open(SYSCONF, ISFS_OPEN_RW);
    bool ok = fd >= 0 && ISFS_Write(fd, b, 0x4000) == 0x4000;
    if (fd >= 0) ISFS_Close(fd);
    free(b);
    if (!ok) { ui_message(T, C_RED, "SYSCONF couldn't be written (NAND permissions: start from the Homebrew Channel)."); return; }
    u32 local = rtc + nb;
    __SYS_SetTime(secs_to_ticks((u64)local));
    __SYS_SetPosixTime(secs_to_ticks((u64)local) + secs_to_ticks((u64)UNIX_2000));
    char done[32], m[120];
    fmt_time(done, sizeof(done), local);
    snprintf(m, sizeof(m), "The clock is set: %s.", done);
    ui_message(T, C_GREEN, m);
}

/*
 * GodModeWii - disc dumper. See discdump.h.
 *
 * Reading a damaged disc follows CleanRip (emu_kidid; GPL-2.0-or-later): a block that fails is tried again
 * after a drive reset, then sector by sector, and a sector only counts once two reads of it agree (the second
 * made after reading elsewhere, so it can't come from the drive's cache). Sectors that still fail are
 * filled with 0x55 (redumper's mark for an unreadable sector) and listed, the way spycrab's CleanRip gets past the unreadable areas of Datel discs; or the
 * dump stops there, as CleanRip itself does. The drive's error codes are YAGCD's. The code is GodModeWii's own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <zlib.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <di/di.h>
#ifdef HAVE_DVDCSS
#include <dvdcss/dvdcss.h>
#endif

#include "discdump.h"
#include "virt.h"
#include "vfs.h"
#include "ui.h"
#include "input.h"
#include "md5.h"
#include "sha1.h"
#include "iospatch.h"
#include "risk.h"

#define T         "DUMP A DISC"
#define SEC       2048u
#define CHUNK     16u               /* sectors per read: one DVD ECC block */
#define BATCH     (CHUNK * 16)      /* sectors per write */
#define FAT_PART  0xFFFF8000ull     /* biggest part a FAT32 file can hold, in whole ECC blocks */
#define BAD_FILL  0x55              /* what redumper writes for a sector it couldn't read */
#define GC_SECS   712880u
#define WII_SECS  2294912u
#define WII9_SECS 4155840u

typedef struct {
    int usb;                        /* USB slot, or -1 for the Wii's drive */
    bool dvd;                       /* the drive's DVD read (non-Nintendo discs) */
    u32 sectors;
    char kind[48];
    char name[80];                  /* file name, without .iso */
    char label[72];                 /* game title / volume label, for the .txt */
} source_t;

typedef struct { u32 lba, n, err; } bad_t;

static source_t S;
static u8 *bb, *bb2;                /* 32-byte aligned, CHUNK sectors each */
static bad_t *bads;
static u32 bad_n, bad_cap, bad_secs, unconfirmed;
static u32 consec;                  /* unreadable sectors in a row: fewer retries on a dead stretch */
static bool quiet;                  /* reads made to find DVD keys, not part of the image */
static bool stop_on_bad, hit_bad;
#ifdef GMW_AUTOTEST
static u32 fail_lo, fail_hi;        /* test builds: these sectors never read */
#endif

/* ---------------------------------------------------------------- reading */

static bool rd(u32 lba, u32 n, u8 *buf)
{
#ifdef GMW_AUTOTEST
    if (lba < fail_hi && lba + n > fail_lo) return false;
#endif
    if (S.usb >= 0) return vfs_usb_raw_read(S.usb, (u64)lba * SEC, buf, n * SEC);
    return virt_disc_raw_read(lba, n, buf, S.dvd);
}

static void reset(void)
{
    if (S.usb < 0) virt_disc_raw_reset();
}

static u32 drive_error(void)
{
    return S.usb < 0 ? virt_disc_raw_error() : 0;
}

static void bad_add(u32 lba, u32 err)
{
    bad_secs++;
    if (bad_n && bads[bad_n - 1].lba + bads[bad_n - 1].n == lba && bads[bad_n - 1].err == err) {
        bads[bad_n - 1].n++;
        return;
    }
    if (bad_n == bad_cap) {
        u32 nc = bad_cap ? bad_cap * 2 : 64;
        bad_t *nb = realloc(bads, nc * sizeof(*nb));
        if (!nb) { if (bad_n) bads[bad_n - 1].n++; return; }    /* out of memory: counted in the last range */
        bads = nb;
        bad_cap = nc;
    }
    bads[bad_n++] = (bad_t){ lba, 1, err };
}

/* Read again after reading elsewhere, so the drive's cache can't answer; true when it matches buf. */
static bool confirmed(u32 lba, u32 n, const u8 *buf)
{
    u32 away = lba >= 0x1000 ? lba - 0x800 : lba + 0x800;
    if (away < S.sectors) rd(away, 1, bb2);
    return rd(lba, n, bb2) && !memcmp(buf, bb2, n * SEC);
}

/* n <= CHUNK sectors into out. Every sector ends up read, or filled with BAD_FILL and listed. */
static void read_sectors(u32 lba, u32 n, u8 *out)
{
    if (rd(lba, n, bb)) {
        memcpy(out, bb, n * SEC);
        consec = 0;
        return;
    }
    for (int t = 0; t < 2 && consec < 8; t++) {      /* the whole block again, after a reset */
        reset();
        if (rd(lba, n, bb) && confirmed(lba, n, bb)) {
            memcpy(out, bb, n * SEC);
            consec = 0;
            return;
        }
    }
    for (u32 i = 0; i < n; i++) {                     /* then sector by sector, until two reads agree */
        u8 *o = out + i * SEC;
        bool have = false, ok = false, failed = false;
        int tries = consec > 8 ? 1 : 4;
        for (int t = 0; t < tries && !ok; t++) {
            if (failed && (consec < 8 || (consec & 15) == 0)) reset();
            failed = !rd(lba + i, 1, bb);
            if (failed) continue;
            if (have && !memcmp(o, bb, SEC)) ok = true;
            else { memcpy(o, bb, SEC); have = true; }
        }
        if (have) {                                   /* one good read with its EDC is still the best there is */
            if (!ok && !quiet) unconfirmed++;
            consec = 0;
            continue;
        }
        memset(o, BAD_FILL, SEC);
        consec++;
        if (!quiet) {
            bad_add(lba + i, drive_error());
            hit_bad = true;
        }
    }
}

/* YAGCD's drive status (top byte) and error codes. */
static void error_text(u32 e, char *out, size_t n)
{
    static const struct { u32 code; const char *text; } errs[] = {
        { 0x020400, "motor stopped" }, { 0x020401, "disc ID not read" }, { 0x023A00, "no disc / cover open" },
        { 0x030200, "no seek complete" }, { 0x031100, "unrecovered read error" },
        { 0x040800, "transfer protocol error" }, { 0x052000, "invalid command" },
        { 0x052100, "block address out of range" }, { 0x052400, "invalid field in command" },
        { 0x056300, "end of user area" }, { 0x062800, "disc may have changed" },
    };
    if (S.usb >= 0) { snprintf(out, n, "read failed"); return; }
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
        if ((e & 0xFFFFFF) == errs[i].code) { snprintf(out, n, "%s (%08X)", errs[i].text, (unsigned)e); return; }
    snprintf(out, n, "drive error %08X", (unsigned)e);
}

/* ---------------------------------------------------------------- what's in the drive */

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* A name that is safe on FAT: letters, digits, " -_.()" */
static void safe_name(char *out, size_t n, const char *in, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && in[i] && o + 1 < n; i++) {
        char c = in[i];
        out[o++] = isalnum((unsigned char)c) || strchr(" -_.()", c) ? c : '_';
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '_' || out[o - 1] == '.')) o--;
    out[o] = 0;
}

/* An ISO 9660 primary volume descriptor at sector 16: its size and label. */
static bool read_pvd(u8 *b)
{
    if (!rd(16, 1, b) || memcmp(b, "\1CD001", 6)) return false;
    S.sectors = le32(b + 80);
    safe_name(S.label, sizeof(S.label), (const char *)b + 40, 32);
    return S.sectors > 16;
}

static bool identify(char *err, size_t en)
{
    u8 *b = bb;
    /* A GameCube or Wii disc answers the unencrypted read at sector 0 */
    /* (a USB drive with OmniDrive firmware hands them out as plain 2048-byte sectors too) */
    if (rd(0, 1, b) && (be32(b + 0x18) == 0x5D1C9EA3 || be32(b + 0x1C) == 0xC2339F3D)) {
        bool gc = be32(b + 0x1C) == 0xC2339F3D;
        char id[8];
        safe_name(id, sizeof(id), (const char *)b, 6);
        if (b[6]) snprintf(S.name, sizeof(S.name), "%s_disc%d", id, b[6] + 1);
        else snprintf(S.name, sizeof(S.name), "%s", id);
        safe_name(S.label, sizeof(S.label), (const char *)b + 0x20, 64);
        if (gc) {
            S.sectors = GC_SECS;
            snprintf(S.kind, sizeof(S.kind), "GameCube");
        } else {
            /* dual-layer when the first sector past a single layer reads (CleanRip asks the same way) */
            bool dl = rd(WII_SECS, 1, b) || (reset(), rd(WII_SECS, 1, b));
            S.sectors = dl ? WII9_SECS : WII_SECS;
            snprintf(S.kind, sizeof(S.kind), dl ? "Wii (dual layer)" : "Wii");
        }
        return true;
    }
    /* Anything else: a DVD (the drive's DVD read) or CD with an ISO 9660 / UDF bridge file system */
    S.dvd = S.usb < 0;
    if (!read_pvd(b) && S.usb < 0) {
        S.dvd = false;                  /* a homebrew disc the unencrypted read handles */
        if (!read_pvd(b)) S.sectors = 0;
    }
    if (S.usb >= 0) {
        u64 cap = vfs_usb_raw_size(S.usb) / SEC;
        if (!S.sectors || S.sectors > cap) S.sectors = (u32)cap;      /* no PVD: what the drive reports */
    }
    if (!S.sectors) {
        snprintf(err, en, S.usb < 0
                 ? "The drive can't read this disc, or can't tell its size.\n\nThe Wii's drive reads GameCube and Wii "
                   "discs; DVDs only on Wiis made before mid-2008 (a newer drive refuses them: use a USB DVD drive)."
                 : "The drive doesn't report a disc, or can't read it. Audio CDs are not supported.");
        return false;
    }
    snprintf(S.name, sizeof(S.name), "%s", S.label[0] ? S.label : "disc");
    u64 bytes = (u64)S.sectors * SEC;
    snprintf(S.kind, sizeof(S.kind), "%s", bytes > 900ull << 20 ? "DVD" : "CD (data)");
    return true;
}

/* ---------------------------------------------------------------- DVD-Video: CSS */

typedef struct { u32 start, end; bool key; } vob_t;
static vob_t vobs[64];
static int vob_n;

#ifdef HAVE_DVDCSS
/* The VOB files in VIDEO_TS, from the ISO 9660 directory. key: a title key starts here (VIDEO_TS.VOB,
 * VTS_nn_0.VOB and VTS_nn_1.VOB); VTS_nn_2.VOB and on carry on with the key of _1. */
static void find_vobs(void)
{
    vob_n = 0;
    u8 *b = memalign(32, SEC);
    if (!b) return;
    quiet = true;
    read_sectors(16, 1, b);
    u32 dir = le32(b + 156 + 2), dsize = le32(b + 156 + 10), vts = 0, vsize = 0;
    for (u32 s = 0; s * SEC < dsize && s < 16 && !vts; s++) {
        read_sectors(dir + s, 1, b);
        for (u32 o = 0; o < SEC && b[o];) {
            const u8 *r = b + o;
            if (r[0] < 34 || o + r[0] > SEC) break;
            if ((r[25] & 2) && r[32] == 8 && !memcmp(r + 33, "VIDEO_TS", 8)) { vts = le32(r + 2); vsize = le32(r + 10); }
            o += r[0];
        }
    }
    for (u32 s = 0; vts && s * SEC < vsize && s < 16; s++) {
        read_sectors(vts + s, 1, b);
        for (u32 o = 0; o < SEC && b[o] && vob_n < 64;) {
            const u8 *r = b + o;
            if (r[0] < 34 || o + r[0] > SEC) break;
            const char *nm = (const char *)r + 33;
            int nl = r[32];
            if (nl >= 12 && !strncasecmp(nm + 8, ".VOB", 4) && le32(r + 10)) {
                u32 st = le32(r + 2);
                vobs[vob_n++] = (vob_t){ st, st + (le32(r + 10) + SEC - 1) / SEC,
                                         !strncasecmp(nm, "VIDEO_TS", 8) || nm[7] == '0' || nm[7] == '1' };
            }
            o += r[0];
        }
    }
    quiet = false;
    free(b);
    for (int i = 1; i < vob_n; i++)                   /* by position on the disc */
        for (int j = i; j > 0 && vobs[j].start < vobs[j - 1].start; j--) {
            vob_t t = vobs[j]; vobs[j] = vobs[j - 1]; vobs[j - 1] = t;
        }
}

static u32 css_pos;
static int css_seek(void *p, uint64_t pos) { (void)p; css_pos = (u32)(pos / SEC); return 0; }
static int css_read(void *p, void *buf, int n)
{
    (void)p;
    for (int done = 0; done < n;) {
        u32 k = n - done > (int)CHUNK ? CHUNK : (u32)(n - done);
        read_sectors(css_pos, k, (u8 *)buf + done * SEC);
        css_pos += k;
        done += k;
    }
    return n;
}
static int css_readv(void *p, const void *iov, int n)
{
    const struct { void *base; size_t len; } *v = iov;
    int total = 0;
    for (int i = 0; i < n; i++) total += css_read(p, v[i].base, (int)(v[i].len / SEC));
    return total;
}
static dvdcss_stream_cb css_cb = { css_seek, css_read, css_readv };
#endif

/* ---------------------------------------------------------------- writing */

typedef struct {
    FILE *f;
    int part;
    u64 in_part, part_size;         /* part_size 0: one file */
    char base[VFS_PATH_MAX + 96];
    char path[VFS_PATH_MAX + 112];
} out_t;

static bool out_open(out_t *o)
{
    if (o->part_size) snprintf(o->path, sizeof(o->path), "%s.part%d.iso", o->base, o->part);
    else snprintf(o->path, sizeof(o->path), "%s.iso", o->base);
    o->f = fopen(o->path, "wb");
    o->in_part = 0;
    return o->f != NULL;
}

static bool out_write(out_t *o, const u8 *p, u64 n)
{
    while (n) {
        u64 room = o->part_size ? o->part_size - o->in_part : n;
        if (!room) {
            if (fclose(o->f) != 0) { o->f = NULL; return false; }
            o->part++;
            if (!out_open(o)) return false;
            continue;
        }
        u64 k = n < room ? n : room;
        if (fwrite(p, 1, k, o->f) != k) return false;
        o->in_part += k;
        p += k;
        n -= k;
    }
    return true;
}

static bool mkdir_p(const char *path)
{
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
        if (s > p && s[-1] == ':') continue;
        *s = 0;
        if (mkdir(p, 0777) < 0 && errno != EEXIST) return false;
        *s = '/';
    }
    return mkdir(p, 0777) == 0 || errno == EEXIST;
}

static void hex(char *out, const u8 *h, int n)
{
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", h[i]);
}

/* ---------------------------------------------------------------- the dump */

typedef struct {
    u32 crc;
    char md5[33], sha1[41];
    u32 secs_done;
    u64 ms;
    bool stopped, cancelled, wfail;
    int keys_missing;               /* DVD-Video title keys not found: those VOBs stay scrambled */
    bool decrypted;
} result_t;

static void progress(u32 done, u64 began)
{
    u64 ms = ticks_to_millisecs(gettime()) - began;
    u64 rate = ms ? (u64)done * SEC * 1000 / ms : 0;
    u32 eta = rate ? (u32)((u64)(S.sectors - done) * SEC / rate) : 0;
    char l1[96], l2[96];
    snprintf(l1, sizeof(l1), "%u / %u MB   %u.%u MB/s   %u:%02u left   (hold B to stop)",
             (unsigned)((u64)done * SEC >> 20), (unsigned)((u64)S.sectors * SEC >> 20),
             (unsigned)(rate >> 20), (unsigned)((rate % (1 << 20)) * 10 >> 20), eta / 60, eta % 60);
    snprintf(l2, sizeof(l2), "unreadable sectors: %u", (unsigned)bad_secs);
    ui_splash(T);
    int r = ui_rows() / 2 + 1, cols = ui_cols();
    ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(l1)) / 2, "", l1);
    ui_field(r + 1, 0, cols - 1, "%*s%s", (cols - (int)strlen(l2)) / 2, "", l2);
    ui_bar(r + 2, (cols - 50) / 2, 50, S.sectors ? (int)((u64)done * 100 / S.sectors) : 0, bad_secs ? C_YELLOW : C_GREEN);
}

static void dump(out_t *o, result_t *res, bool ask_cancel)
{
    u8 *buf = memalign(32, BATCH * SEC);
    if (!buf) { res->wfail = true; return; }
    md5_t m;
    sha1_t sh;
    md5_init(&m);
    sha1_init(&sh);
    uLong crc = crc32(0, NULL, 0);
    u64 began = ticks_to_millisecs(gettime()), last = 0;

#ifdef HAVE_DVDCSS
    dvdcss_t css = NULL;
    if (S.dvd || S.usb >= 0) {
        find_vobs();
        if (vob_n) {
            setenv("DVDCSS_CACHE", "off", 1);
            setenv("DVDCSS_VERBOSE", "0", 1);
            css = dvdcss_open_stream(NULL, &css_cb);
            res->decrypted = css != NULL;
        }
    }
    bool have_key = false;
#endif

    u32 lba = 0;
    int vi = 0;
    while (lba < S.sectors && !res->stopped && !res->wfail) {
        u32 n = S.sectors - lba < BATCH ? S.sectors - lba : BATCH;
        bool in_vob = false;
        while (vi < vob_n && vobs[vi].end <= lba) vi++;
        if (vi < vob_n) {
            if (lba < vobs[vi].start) { if (vobs[vi].start - lba < n) n = vobs[vi].start - lba; }
            else { in_vob = true; if (vobs[vi].end - lba < n) n = vobs[vi].end - lba; }
        }
#ifdef HAVE_DVDCSS
        if (in_vob && css) {
            if (lba == vobs[vi].start && vobs[vi].key) {
                quiet = true;
                have_key = dvdcss_seek(css, (int)lba, DVDCSS_SEEK_KEY) >= 0;
                quiet = false;
                if (!have_key) res->keys_missing++;
            }
            if (have_key) {
                dvdcss_seek(css, (int)lba, DVDCSS_NOFLAGS);
                if (dvdcss_read(css, buf, (int)n, DVDCSS_READ_DECRYPT) != (int)n) res->wfail = true;
            } else {
                in_vob = false;
            }
        }
        if (!(in_vob && css && have_key))
#endif
            for (u32 k = 0; k < n; k += CHUNK)
                read_sectors(lba + k, n - k < CHUNK ? n - k : CHUNK, buf + k * SEC);
        (void)in_vob;
        if (hit_bad && stop_on_bad) res->stopped = true;
        md5_update(&m, buf, n * SEC);
        sha1_update(&sh, buf, n * SEC);
        crc = crc32(crc, buf, n * SEC);
        if (!out_write(o, buf, (u64)n * SEC)) res->wfail = true;
        lba += n;
        u64 t = ticks_to_millisecs(gettime());
        if (t - last >= 500 || lba == S.sectors) { last = t; progress(lba, began); }
#ifdef GMW_AUTOTEST
        if (!(lba & 0x7FFF) || lba == S.sectors) {           /* where a stuck test run got to */
            FILE *pf = fopen("sd:/gmwprog.txt", "w");
            if (pf) { fprintf(pf, "lba %u of %u bad %u t %u\n", (unsigned)lba, (unsigned)S.sectors, (unsigned)bad_secs, (unsigned)(t - began)); fclose(pf); }
        }
#endif
        if (ask_cancel && input_cancel_held() &&
            ui_confirm(T, C_YELLOW, "Stop the dump? What was read so far is kept, with a .txt saying it is incomplete.")) {
            res->stopped = res->cancelled = true;
        }
    }
#ifdef HAVE_DVDCSS
    if (css) dvdcss_close(css);
#endif
    free(buf);
    u8 h[20];
    md5_final(&m, h);
    hex(res->md5, h, 16);
    sha1_final(&sh, h);
    hex(res->sha1, h, 20);
    res->crc = (u32)crc;
    res->secs_done = lba;
    res->ms = ticks_to_millisecs(gettime()) - began;
}

static void write_info(const out_t *o, const result_t *r, const char *src_name)
{
    char path[VFS_PATH_MAX + 112];
    snprintf(path, sizeof(path), "%s.txt", o->base);
    FILE *f = fopen(path, "w");
    if (!f) return;
    time_t now = time(NULL);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "Dumped with GodModeWii on %s\n\n", date);
    fprintf(f, "Disc:      %s%s\n", S.kind, r->decrypted ? " (DVD-Video, CSS decrypted with libdvdcss)" : "");
    if (S.label[0]) fprintf(f, "Title:     %s\n", S.label);
    fprintf(f, "Drive:     %s\n", src_name);
    const char *base = strrchr(o->base, '/') + 1;
    if (o->part_size) fprintf(f, "Files:     %s.part0.iso to .part%d.iso (join them in order)\n", base, o->part);
    else fprintf(f, "File:      %s.iso\n", base);
    fprintf(f, "Size:      %llu bytes (%u sectors)\n", (unsigned long long)S.sectors * SEC, (unsigned)S.sectors);
    if (r->secs_done < S.sectors)
        fprintf(f, "INCOMPLETE: stopped after %u of %u sectors%s\n", (unsigned)r->secs_done, (unsigned)S.sectors,
                r->cancelled ? " (stopped by hand)" : r->wfail ? " (write error)" : " (unreadable sector)");
    fprintf(f, "Time:      %u:%02u\n\n", (unsigned)(r->ms / 60000), (unsigned)(r->ms / 1000 % 60));
    fprintf(f, "CRC32:     %08x\nMD5:       %s\nSHA-1:     %s\n\n", (unsigned)r->crc, r->md5, r->sha1);
    if (r->keys_missing)
        fprintf(f, "%d DVD title key(s) weren't found: those VOB files are left scrambled.\n\n", r->keys_missing);
    fprintf(f, "Unreadable sectors: %u (filled with 0x55, listed in %s.log)\n", (unsigned)bad_secs, base);
    if (unconfirmed) fprintf(f, "Sectors read once only, after errors: %u\n", (unsigned)unconfirmed);
    for (u32 i = 0; i < bad_n; i++) {
        char e[64];
        error_text(bads[i].err, e, sizeof(e));
        if (bads[i].n == 1) fprintf(f, "  sector %u: %s\n", (unsigned)bads[i].lba, e);
        else fprintf(f, "  sectors %u-%u: %s\n", (unsigned)bads[i].lba, (unsigned)(bads[i].lba + bads[i].n - 1), e);
    }
    fclose(f);
    /* <name>.log: one line per unreadable sector, its byte offset and the drive's error */
    snprintf(path, sizeof(path), "%s.log", o->base);
    if (bad_secs && (f = fopen(path, "w"))) {
        fprintf(f, "# sector  offset  error   (each filled with 0x55 in the ISO)\n");
        for (u32 i = 0; i < bad_n; i++) {
            char e[64];
            error_text(bads[i].err, e, sizeof(e));
            for (u32 k = 0; k < bads[i].n; k++)
                fprintf(f, "%u  0x%llx  %s\n", (unsigned)(bads[i].lba + k), (unsigned long long)(bads[i].lba + k) * SEC, e);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s.txt", o->base);
    if (!(f = fopen(path, "a"))) return;
    fprintf(f, "\nA good dump has no unreadable sectors: compare its hashes with the redump.info database\n"
               "(https://redump.info/).\n");
    fclose(f);
}

/* ---------------------------------------------------------------- choosing */

static int pick_source(char *name, size_t n)
{
    const char *items[1 + USB_SLOTS];
    int code[1 + USB_SLOTS], k = 0;
    static char labels[USB_SLOTS][40];
    if (!g_is_vwii && AHBPROT_DISABLED) { items[k] = "The Wii's disc drive"; code[k++] = -1; }
    for (int s = 0; s < USB_SLOTS; s++)
        if (vfs_usb_sector_size(s) == SEC) {
            snprintf(labels[s], sizeof(labels[s]), "USB CD / DVD drive %d", s + 1);
            items[k] = labels[s];
            code[k++] = s;
        }
    if (!k) {
        ui_message(T, C_RED, g_is_vwii ? "The disc drive can't be read in vWii, and there is no USB CD / DVD drive with a disc "
                                         "in it (put one in, then Remount drives)."
                                       : "There is no drive to dump from. A USB CD / DVD drive shows up once it has a disc "
                                         "in it and the drives are remounted.");
        return -2;
    }
    int r = k == 1 ? 0 : ui_menu("DUMP FROM", items, NULL, k, 0);
    if (r < 0) return -2;
    snprintf(name, n, "%s", items[r]);
    return code[r];
}

static int pick_target(void)
{
    const char *items[1 + USB_SLOTS];
    int devs[1 + USB_SLOTS], n = 0;
    for (int d = DEV_SD; d <= DEV_USB7; d++) {
        if (!g_devs[d].mounted || vfs_is_readonly(g_devs[d].root)) continue;
        items[n] = g_devs[d].name;
        devs[n++] = d;
    }
    if (!n) {
        ui_message(T, C_RED, "There is no writable SD card or USB drive to save the ISO to.");
        return -1;
    }
    if (n == 1) return devs[0];
    int r = ui_menu("SAVE THE ISO ON", items, NULL, n, 0);
    return r < 0 ? -1 : devs[r];
}

static void reset_state(void)
{
    free(bads);
    bads = NULL;
    bad_n = bad_cap = bad_secs = unconfirmed = consec = 0;
    quiet = hit_bad = false;
    vob_n = 0;
    memset(&S, 0, sizeof(S));
}

/* Everything after the choices: the output files, the dump, the .txt. msg gets the summary. */
static bool run_dump(int dev, out_t *o, result_t *r, const char *src_name, bool ask_cancel, char *msg, size_t mn)
{
    char dir[VFS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%sGodModeWii/dumps", g_devs[dev].root);
    if (!mkdir_p(dir)) { snprintf(msg, mn, "Couldn't create %s", dir); return false; }
    struct stat st;
    snprintf(o->base, sizeof(o->base), "%s/%s", dir, S.name);
    for (int i = 2; i < 100; i++) {                   /* never over an earlier dump */
        char t[VFS_PATH_MAX + 112];
        snprintf(t, sizeof(t), "%s.txt", o->base);
        if (stat(t, &st) < 0) break;
        snprintf(o->base, sizeof(o->base), "%s/%s (%d)", dir, S.name, i);
    }
    u64 bytes = (u64)S.sectors * SEC;
    bool fat = !g_devs[dev].exfat && !g_devs[dev].ntfs && !g_devs[dev].ext;
    if (!o->part_size && fat && bytes > FAT_PART) o->part_size = FAT_PART;
    struct statvfs fs;
    if (statvfs(g_devs[dev].root, &fs) == 0 && (u64)fs.f_bsize * fs.f_bfree < bytes + (1 << 20)) {
        snprintf(msg, mn, "%s needs %u MB free for this disc.", g_devs[dev].name, (unsigned)(bytes >> 20) + 1);
        return false;
    }
    if (!out_open(o)) { snprintf(msg, mn, "Couldn't create %s", o->path); return false; }
    dump(o, r, ask_cancel);
    bool closed = o->f && fclose(o->f) == 0;
    o->f = NULL;
    if (!closed) r->wfail = true;
    write_info(o, r, src_name);
    snprintf(msg, mn, "%s%s\n\n%s\n\nCRC32 %08x\nMD5   %s\nSHA-1 %s\n\nUnreadable sectors: %u%s\n%s",
             r->wfail ? "WRITE ERROR (is the drive full?)\n\n" : r->secs_done < S.sectors ? "STOPPED: the ISO is incomplete.\n\n" : "",
             o->part_size ? "Saved in 4 GB parts (FAT32):" : "Saved:", o->path, (unsigned)r->crc, r->md5, r->sha1,
             (unsigned)bad_secs, bad_secs ? " (filled with 0x55, listed in the .log)" : "",
             bad_secs || r->secs_done < S.sectors ? "" : "Compare the hashes with redump.info.");
    return !r->wfail && r->secs_done == S.sectors && !bad_secs;
}

void discdump_run(void)
{
    reset_state();
    char src_name[48], err[400] = "";
    S.usb = pick_source(src_name, sizeof(src_name));
    if (S.usb == -2) return;
    ui_splash("Reading the disc...");
    if (S.usb < 0 && !virt_disc_raw_begin()) {
        ui_message(T, C_RED, "There is no disc in the drive, or the drive can't be used.");
        return;
    }
    if (!bb) bb = memalign(32, CHUNK * SEC);
    if (!bb2) bb2 = memalign(32, CHUNK * SEC);
    if (!bb || !bb2) { ui_message(T, C_RED, "Out of memory."); return; }
    if (!identify(err, sizeof(err))) { ui_message(T, C_RED, err); return; }

    int dev = pick_target();
    if (dev < 0) return;
    u64 bytes = (u64)S.sectors * SEC;
    bool fat = !g_devs[dev].exfat && !g_devs[dev].ntfs && !g_devs[dev].ext;
    char q[600];
    snprintf(q, sizeof(q), "%s%s%s\n%u MB, %u sectors\n\nSave it as GodModeWii/dumps/%s.iso on %s?%s%s\n\nWhat to do with a sector that "
             "can't be read even after retries:",
             S.kind, S.label[0] ? ": " : "", S.label, (unsigned)(bytes >> 20), (unsigned)S.sectors, S.name,
             g_devs[dev].name, fat && bytes > FAT_PART ? "\n(FAT32: split into 4 GB parts, .part0.iso, .part1.iso...)" : "",
#ifdef HAVE_DVDCSS
             S.dvd || S.usb >= 0 ? "\nA DVD-Video is decrypted (CSS) with libdvdcss." : ""
#else
             S.dvd || S.usb >= 0 ? "\nA DVD-Video stays CSS-scrambled (built without libdvdcss)." : ""
#endif
             );
    ui_message(T, C_CYAN, q);
    const char *modes[] = { "Fill it with 0x55, log it and go on", "Stop the dump there (CleanRip's way)" };
    int m = ui_menu("UNREADABLE SECTORS", modes, NULL, 2, 0);
    if (m < 0) return;
    stop_on_bad = m == 1;

    out_t o = { 0 };
    result_t r = { 0 };
    char msg[VFS_PATH_MAX + 600];
    bool good = run_dump(dev, &o, &r, src_name, true, msg, sizeof(msg));
    ui_message(good ? "DISC DUMPED" : T, good ? C_GREEN : r.wfail ? C_RED : C_YELLOW, msg);
}

/* ---------------------------------------------------------------- drive stress test */

static u32 rng = 0x12345678;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Reads all over the disc for a while: random seeks, full-stroke seeks and the odd stop / spin-up, timing each
 * read and counting the drive's errors. secs 0: until B. */
static void stress(u32 secs, FILE *lg)
{
    u64 began = ticks_to_millisecs(gettime()), last = 0, sum = 0;
    u32 reads = 0, errs = 0, spins = 0, worst = 0, last_err = 0;
    rng ^= (u32)began;
    for (u32 i = 0;; i++) {
        u64 now = ticks_to_millisecs(gettime());
        if (secs ? now - began >= (u64)secs * 1000 : input_cancel_held()) break;
        if (i % 200 == 199) { virt_disc_raw_stop(); virt_disc_raw_reset(); spins++; }
        u32 lba = (i & 7) == 0 ? ((i & 8) ? S.sectors - CHUNK : 0) : rnd() % (S.sectors - CHUNK);
        u64 t0 = ticks_to_microsecs(gettime());
        bool ok = rd(lba & ~(CHUNK - 1), CHUNK, bb);
        u32 us = (u32)(ticks_to_microsecs(gettime()) - t0);
        reads++;
        sum += us;
        if (us > worst) worst = us;
        if (!ok) { errs++; last_err = drive_error(); reset(); }
        if (!lg && now - last >= 500) {
            last = now;
            char l1[96], l2[96], e[64] = "none";
            if (errs) error_text(last_err, e, sizeof(e));
            snprintf(l1, sizeof(l1), "%u reads, %u errors, %u spin-ups   %u:%02u   (hold B to stop)",
                     (unsigned)reads, (unsigned)errs, (unsigned)spins, (unsigned)((now - began) / 60000),
                     (unsigned)((now - began) / 1000 % 60));
            snprintf(l2, sizeof(l2), "average %u ms, slowest %u ms; last error: %s", (unsigned)(sum / reads / 1000),
                     (unsigned)(worst / 1000), e);
            ui_splash("DISC DRIVE STRESS TEST");
            int r = ui_rows() / 2 + 1, cols = ui_cols();
            ui_field(r, 0, cols - 1, "%*s%s", (cols - (int)strlen(l1)) / 2, "", l1);
            ui_field(r + 1, 0, cols - 1, "%*s%s", (cols - (int)strlen(l2)) / 2, "", l2);
        }
    }
    virt_disc_raw_stop();
    char msg[300], e[64] = "none";
    if (errs) error_text(last_err, e, sizeof(e));
    snprintf(msg, sizeof(msg), "%u reads of 32 KB over %u MB, %u spin-ups.\n\nErrors: %u (last: %s)\nAverage read: %u ms, "
             "slowest: %u ms\n\n%s", (unsigned)reads, (unsigned)((u64)S.sectors * SEC >> 20), (unsigned)spins,
             (unsigned)errs, e, reads ? (unsigned)(sum / reads / 1000) : 0, (unsigned)(worst / 1000),
             errs ? "Errors on a clean disc point at a worn laser or a dirty lens." : "The drive read everything it was asked.");
    if (lg) fprintf(lg, "stress: reads %u errors %u spins %u\n", (unsigned)reads, (unsigned)errs, (unsigned)spins);
    else ui_message("DISC DRIVE STRESS TEST", errs ? C_YELLOW : C_GREEN, msg);
}

void discdump_stress_test(void)
{
    const char *T2 = "DISC DRIVE STRESS TEST";
    if (g_is_vwii || !AHBPROT_DISABLED) { ui_message(T2, C_RED, "The disc drive can't be used here."); return; }
    if (!risk_ensure_unlocked()) return;
    if (!ui_confirm(T2, C_YELLOW, "Read all over the disc in the drive for a while: random and full-stroke seeks, and a "
                    "stop and spin-up every 200 reads. It works the laser sled and the motor hard, which is the point: "
                    "use it to find a failing drive, not every day. The disc is only read.\n\nPut in a disc you don't mind "
                    "spinning, then go on.")) return;
    const char *lens[] = { "1 minute", "5 minutes", "15 minutes", "Until I hold B" };
    int l = ui_menu("HOW LONG", lens, NULL, 4, 0);
    if (l < 0) return;
    reset_state();
    S.usb = -1;
    if (!bb) bb = memalign(32, CHUNK * SEC);
    if (!bb2) bb2 = memalign(32, CHUNK * SEC);
    char err[400] = "";
    ui_splash("Reading the disc...");
    if (!bb || !bb2 || !virt_disc_raw_begin() || !identify(err, sizeof(err))) {
        ui_message(T2, C_RED, err[0] ? err : "There is no disc in the drive.");
        return;
    }
    static const u32 len_s[] = { 60, 300, 900, 0 };
    stress(len_s[l], NULL);
}

#ifdef GMW_AUTOTEST
/* "@discdump [part MB] [bad lba] [bad count] [stop]": the disc in the drive dumped to the SD card answering every
 * question; part MB splits like FAT32 does at a smaller size, bad sectors are made to fail. */
void discdump_autotest(const char *args)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    unsigned part = 0, lo = 0, cnt = 0, stop = 0;
    sscanf(args, "%u %u %u %u", &part, &lo, &cnt, &stop);
    reset_state();
    S.usb = -1;
    fail_lo = lo;
    fail_hi = lo + cnt;
    char err[400] = "", msg[VFS_PATH_MAX + 600];
    if (!bb) bb = memalign(32, CHUNK * SEC);
    if (!bb2) bb2 = memalign(32, CHUNK * SEC);
    if (!virt_disc_raw_begin() || !identify(err, sizeof(err))) {
        fprintf(lg, "discdump: no disc (%s)\n", err);
        fclose(lg);
        return;
    }
    stop_on_bad = stop;
    out_t o = { 0 };
    o.part_size = (u64)part << 20;
    result_t r = { 0 };
    bool good = run_dump(DEV_SD, &o, &r, "The Wii's disc drive", false, msg, sizeof(msg));
    fprintf(lg, "discdump: kind %s name %s sectors %u good %d parts %d done %u bad %u ranges %u crc %08x md5 %s sha1 %s\n",
            S.kind, S.name, (unsigned)S.sectors, good, o.part + 1, (unsigned)r.secs_done, (unsigned)bad_secs,
            (unsigned)bad_n, (unsigned)r.crc, r.md5, r.sha1);
    fail_lo = fail_hi = 0;
    fclose(lg);
}

/* "@discstress <seconds>": the stress test, logged */
void discdump_stress_autotest(const char *args)
{
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (!lg) return;
    reset_state();
    S.usb = -1;
    char err[400] = "";
    if (!bb) bb = memalign(32, CHUNK * SEC);
    if (!bb2) bb2 = memalign(32, CHUNK * SEC);
    if (!virt_disc_raw_begin() || !identify(err, sizeof(err))) fprintf(lg, "stress: no disc\n");
    else stress((u32)atoi(args), lg);
    fclose(lg);
}
#endif

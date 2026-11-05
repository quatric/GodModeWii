/*
 * wiiload receiver. See wiiload.h.
 *
 * The protocol (wiiload 0.5, as the Homebrew Channel takes it): a 16-byte header
 *     "HAXX", u8 major (0), u8 minor (5), u16 argument bytes, u32 bytes sent, u32 bytes unpacked
 * (all big endian; "bytes unpacked" is 0 when the data isn't zlib-compressed), then the data,
 * then the arguments: NUL-separated strings, the first being the file's name. Version 0.4
 * clients send the same header without compression.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <network.h>
#include <zlib.h>
#include <ogc/lwp_watchdog.h>

#include "wiiload.h"
#include "ftp.h"
#include "app.h"
#include "ui.h"
#include "input.h"
#include <input_wii.h>
#include "vfs.h"
#include "install.h"
#include "ftype.h"
#include "wadinstall.h"

#define WIILOAD_PORT  4299
#define IOS_NONBLOCK  0x04
#define IDLE_MS       (15 * 1000)          /* a client that stops sending is dropped */
#define CHUNK         (32 * 1024)
#define SIZE_MAX_IN   (512u << 20)
#define LOG_LINES     8

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static char logbuf[LOG_LINES][96];
static int log_n;

static void wl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wl_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logbuf[log_n % LOG_LINES], sizeof(logbuf[0]), fmt, ap);
    va_end(ap);
#ifdef GMW_AUTOTEST
    FILE *lg = fopen("sd:/gmwtest.log", "a");
    if (lg) {
        fprintf(lg, "wiiload: %s\n", logbuf[log_n % LOG_LINES]);
        fclose(lg);
    }
#endif
    log_n++;
}

static bool lan_peer(u32 ip_net)
{
    u32 ip = ntohl(ip_net);
    u32 a = ip >> 24, b = (ip >> 16) & 0xFF;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || a == 127;
}

static s32 make_listener(void)
{
    s32 s = net_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    net_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons(WIILOAD_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (net_bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || net_listen(s, 2) < 0) {
        net_close(s);
        return -1;
    }
    net_fcntl(s, F_SETFL, IOS_NONBLOCK);
    return s;
}

/* ---------------------------------------------------------------- screen */

static void draw(u32 ip, int dev, bool full, const char *busy, u64 done, u64 total)
{
    int cols = ui_cols(), rows = ui_rows();
    if (full) ui_clear();
    ui_bg(C_GREEN);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " GodModeWii v%s  -  WIILOAD", GMW_VERSION);
    ui_reset();
    ui_color(C_GREEN, true);
    ui_field(2, 1, cols - 2, busy ? "Receiving" : "Waiting for wiiload");
    ui_color(C_WHITE, true);

    ui_field(4, 1, cols - 2, "Address :  %u.%u.%u.%u  (port %d)", ip >> 24, (ip >> 16) & 255,
             (ip >> 8) & 255, ip & 255, WIILOAD_PORT);
    ui_field(5, 1, cols - 2, "Send    :  WIILOAD=tcp:%u.%u.%u.%u wiiload MyApp.zip", ip >> 24,
             (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    ui_color(C_YELLOW, true);
    ui_field(6, 1, cols - 2, "Install :  %s",
             dev < 0         ? "no drive to install onto!"
             : dev == DEV_SD ? "SD card"
                             : g_devs[dev].tag);
    ui_color(C_WHITE, false);
    ui_field(7, 1, cols - 2, "Apps    :  .zip / .7z / .rar install into apps/; a .dol / .elf can run.");
    ui_color(C_CYAN, true);
    ui_field(8, 1, cols - 2, "WADs can be installed here, to the NAND or to the SD card (wad2bin:");
    ui_field(9, 1, cols - 2, "  a content.bin made with this Wii's keys, run from the SD card menu).");
    ui_color(C_WHITE, false);
    ui_field(10, 1, cols - 2, "  A .wad, or an archive of WADs, asks which, one WAD at a time.");

    if (busy) {
        char a[24], b[24];
        ui_fmt_size(a, sizeof(a), done);
        ui_fmt_size(b, sizeof(b), total);
        ui_color(C_WHITE, true);
        ui_field(12, 2, cols - 4, "%s", busy);
        ui_bar(13, 2, cols - 4, total ? (int)(done * 100 / total) : 0, C_GREEN);
        ui_color(C_WHITE, false);
        ui_field(14, 2, cols - 4, "%s / %s", a, b);
    } else {
        ui_field(12, 2, cols - 4, "%s", "");
        ui_field(13, 2, cols - 4, "%s", "");
        ui_field(14, 2, cols - 4, "%s", "");
    }

    int logrow = rows - 3 - LOG_LINES;
    ui_color(C_CYAN, true);
    ui_field(logrow - 1, 1, cols - 2, "Log");
    ui_color(C_WHITE, false);
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = log_n - LOG_LINES + i;
        ui_field(logrow + i, 2, cols - 3, "%s", idx >= 0 ? logbuf[idx % LOG_LINES] : "");
    }

    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2,
             busy ? "Hold B: cancel" : "1: SD card / USB drive    B / HOME: stop");
    ui_reset();
}

/* ---------------------------------------------------------------- receiving */

typedef struct {
    s32 sock;
    u64 last;
    u32 ip;
    int dev;
} conn_t;

/* Exactly n bytes, or false (closed, timed out, canceled). */
static bool recv_all(conn_t *c, void *buf, u32 n, const char *what, u64 done, u64 total)
{
    u8 *p = buf;
    u64 drawn = 0;
    while (n) {
        s32 r = net_recv(c->sock, p, n > CHUNK ? CHUNK : n, 0);
        if (r > 0) {
            p += r;
            n -= r;
            done += r;
            c->last = now_ms();
        } else if (r == 0)
            return false;
        else if (r != -EAGAIN)
            return false;
        else {
            if (now_ms() - c->last > IDLE_MS) return false;
            usleep(2000);
        }
        if (input_cancel_held() || g_quit_request) return false;
        if (what && now_ms() - drawn > 250) {
            drawn = now_ms();
            draw(net_gethostip(), c->dev, false, what, done, total);
        }
    }
    return true;
}

static void safe_name(char *out, size_t sz, const char *in)
{
    const char *b = strrchr(in, '/');
    if (b) in = b + 1;
    b = strrchr(in, '\\');
    if (b) in = b + 1;
    size_t k = 0;
    for (; *in && k + 1 < sz; in++) {
        unsigned char ch = *in;
        out[k++] = (ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                    ch == '>' || ch == '|')
                       ? '_'
                       : ch;
    }
    out[k] = 0;
    if (!k || !strcmp(out, ".") || !strcmp(out, "..")) snprintf(out, sz, "wiiload.bin");
}

static bool mkdirs(const char *dir)
{
    bool is_dir;
    if (vfs_stat(dir, &is_dir, NULL)) return is_dir;
    char parent[VFS_PATH_MAX];
    if (!vfs_parent(dir, parent, sizeof(parent))) return false;
    if (!vfs_is_root(parent) && !mkdirs(parent)) return false;
    return vfs_mkdir(dir) || (vfs_stat(dir, &is_dir, NULL) && is_dir);
}

/* Receives one upload into `path` (inflating it when it was sent compressed). The first bytes of
 * what was written go to head. */
static bool receive(conn_t *c, char *path, size_t pathsz, u8 *head, u32 *headn)
{
    u8 h[16];
    c->last = now_ms();
    if (!recv_all(c, h, 16, NULL, 0, 0)) {
        wl_log("No header: not a wiiload client?");
        return false;
    }
    if (memcmp(h, "HAXX", 4)) {
        wl_log("Not a wiiload upload");
        return false;
    }
    if (h[4] != 0 || h[5] < 4) {
        wl_log("wiiload %u.%u isn't supported: update the client", h[4], h[5]);
        return false;
    }
    u32 argn = h[6] << 8 | h[7];
    u32 sent = (u32)h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11];
    u32 full = h[5] >= 5 ? ((u32)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15]) : 0;
    u32 size = full ? full : sent;
    if (!sent || sent > SIZE_MAX_IN || size > SIZE_MAX_IN) {
        wl_log("Bad size (%u bytes)", (unsigned)size);
        return false;
    }

    char root[VFS_PATH_MAX], dir[VFS_PATH_MAX], tmp[VFS_PATH_MAX];
    snprintf(root, sizeof(root), "%s", vfs_dev_root(c->dev));
    if (!vfs_join(dir, sizeof(dir), root, "GodModeWii/wiiload") || !mkdirs(dir) ||
        !vfs_join(tmp, sizeof(tmp), dir, ".receiving")) {
        wl_log("Can't make GodModeWii/wiiload on the %s", g_devs[c->dev].tag);
        return false;
    }
    vfs_file f;
    if (!vfs_open_write(&f, tmp)) {
        wl_log("Can't write to the %s", g_devs[c->dev].tag);
        return false;
    }
    vfs_unbuffered(&f);

    u8 *in = memalign(32, CHUNK), *out = memalign(32, CHUNK);
    z_stream z;
    memset(&z, 0, sizeof(z));

    bool ok = in && out && (!full || inflateInit(&z) == Z_OK);
    u32 got = 0, wrote = 0;
    *headn = 0;

    while (ok && got < sent) {
        u32 want = sent - got > CHUNK ? CHUNK : sent - got;
        if (!recv_all(c, in, want, "Receiving...", got, sent)) {
            wl_log("Connection lost or canceled");
            ok = false;
            break;
        }
        got += want;

        const u8 *data = in;
        u32 dn = want;

        if (full) {
            z.next_in = in;
            z.avail_in = want;
            do {
                z.next_out = out;
                z.avail_out = CHUNK;
                int zr = inflate(&z, Z_NO_FLUSH);
                if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) {
                    wl_log("Bad compressed data");
                    ok = false;
                    break;
                }
                dn = CHUNK - z.avail_out;
                if (dn && vfs_write(&f, out, dn) != (s32)dn) {
                    wl_log("Write failed (drive full?)");
                    ok = false;
                    break;
                }
                if (dn && *headn < 0x100) {
                    u32 k = 0x100 - *headn < dn ? 0x100 - *headn : dn;
                    memcpy(head + *headn, out, k);
                    *headn += k;
                }
                wrote += dn;
            } while (ok && z.avail_out == 0);
            continue;
        }

        if (vfs_write(&f, data, dn) != (s32)dn) {
            wl_log("Write failed (drive full?)");
            ok = false;
            break;
        }
        if (*headn < 0x100) {
            u32 k = 0x100 - *headn < dn ? 0x100 - *headn : dn;
            memcpy(head + *headn, data, k);
            *headn += k;
        }
        wrote += dn;
    }

    if (full) inflateEnd(&z);
    if (ok && wrote != size) {
        wl_log("Got %u bytes, expected %u", (unsigned)wrote, (unsigned)size);
        ok = false;
    }

    /* the arguments: the first is the file's name */
    char name[128] = "";
    if (ok && argn) {
        char *args = calloc(1, argn + 1);
        if (args && recv_all(c, args, argn, NULL, 0, 0)) safe_name(name, sizeof(name), args);
        free(args);
    }
    free(in);
    free(out);
    if (!vfs_close_checked(&f)) ok = false;
    if (!ok) {
        vfs_remove(tmp, false);
        return false;
    }
    if (!name[0]) safe_name(name, sizeof(name), "");

    /* the name the archive reader / launcher will go by: make its extension match what it is */
    const char *ext = NULL;
    if (*headn >= 4 && !memcmp(head, "PK\3\4", 4)) ext = ".zip";
    else if (*headn >= 6 && !memcmp(head, "7z\xBC\xAF\x27\x1C", 6)) ext = ".7z";
    else if (*headn >= 4 && !memcmp(head, "Rar!", 4)) ext = ".rar";
    else if (ftype_sniff(head, *headn, size, NULL) == FT_PROGRAM)
        ext = *headn >= 4 && !memcmp(head, "\x7F" "ELF", 4) ? ".elf" : ".dol";
    if (ext) {
        char *dot = strrchr(name, '.');
        if (!dot || strcasecmp(dot, ext)) {
            if (strlen(name) + strlen(ext) < sizeof(name)) strcat(name, ext);
        }
    }
    if (!vfs_join(path, pathsz, dir, name)) {
        vfs_remove(tmp, false);
        return false;
    }
    vfs_remove(path, false);
    if (!vfs_rename(tmp, path)) {
        wl_log("Can't rename the upload");
        vfs_remove(tmp, false);
        return false;
    }
    char sz[24];
    ui_fmt_size(sz, sizeof(sz), size);
    wl_log("Received %s (%s) from %u.%u.%u.%u", name, sz, c->ip >> 24, (c->ip >> 16) & 255,
           (c->ip >> 8) & 255, c->ip & 255);
    return true;
}

/* ---------------------------------------------------------------- after a file arrived */

static const char *g_inst_name;
static int g_inst_dev;

static bool install_progress(const char *file, u64 done, u64 total)
{
    static u64 last;
    if (input_cancel_held()) return false;
    if (now_ms() - last < 250 && done != total) return true;
    last = now_ms();
    char what[200];
    snprintf(what, sizeof(what), "Installing %s: %s", g_inst_name, file);
    draw(net_gethostip(), g_inst_dev, false, what, done, total);
    return true;
}

/* The WADs an archive unpacked, to offer them for installing. */
typedef struct {
    char **paths;
    int n, cap;
} wadlist_t;

static void note_wad(const char *path, void *ctx)
{
    wadlist_t *w = ctx;
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".wad")) return;
    if (w->n == w->cap) {
        int nc = w->cap ? w->cap * 2 : 8;
        char **np = realloc(w->paths, (size_t)nc * sizeof(char *));
        if (!np) return;
        w->paths = np;
        w->cap = nc;
    }
    if ((w->paths[w->n] = strdup(path))) w->n++;
}

/* Each WAD: to the NAND, to the SD card (wad2bin), or left where it is. The installers ask for
 * confirmation and the Konami code themselves. Returns how many went in somewhere. */
static int install_wads(char **paths, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        char title[96];
        snprintf(title, sizeof(title), "%s", vfs_basename(paths[i]));
#ifdef GMW_AUTOTEST
        wl_log("Would ask: %s to the NAND, to the SD card (wad2bin), or keep it?", title);
        continue;
#endif
        const char *items[] = { "Install to the NAND", "Install to the SD card (wad2bin: content.bin)",
                                "Don't install it (keep the file)" };
        int r = ui_menu(title, items, NULL, 3, 0);
        int got = 0;
        if (r == 0) got = wad_install_paths(&paths[i], 1);
        else if (r == 1) got = wad_install_sd(&paths[i], 1);
        if (got) {
            ok++;
            wl_log("Installed %s to the %s", title, r == 0 ? "NAND" : "SD card");
        } else
            wl_log("%s not installed", title);
    }
    return ok;
}

/* true when the user chose to start the program now */
static bool handle(const char *path, const u8 *head, u32 headn, int dev, char *launch,
                   size_t launchsz)
{
    const char *name = vfs_basename(path);

    if (install_is_archive_name(name)) {
        char err[96] = "";
        g_inst_name = name;
        g_inst_dev = dev;
        wadlist_t wads = { 0 };
        install_on_written(note_wad, &wads);
        int n = install_archive_apps(path, dev, install_progress, err, sizeof(err));
        install_on_written(NULL, NULL);
        if (n >= 0) {
            vfs_remove(path, false);
            wl_log("Installed %d file%s to the %s", n, n == 1 ? "" : "s", g_devs[dev].tag);
            if (wads.n) install_wads(wads.paths, wads.n);       /* they stay on the drive either way */
        } else {
            wl_log("Install failed: %s", err);
            wl_log("Kept it as GodModeWii/wiiload/%s", name);
        }
        for (int i = 0; i < wads.n; i++) free(wads.paths[i]);
        free(wads.paths);
        vfs_update_space(dev);
        return false;
    }

    if (ftype_sniff(head, headn, 0x100, name) == FT_WAD) {
        char *one = (char *)path;
        if (install_wads(&one, 1) == 1) {
            vfs_remove(path, false);                    /* it's on the NAND now */
            vfs_update_space(dev);
        } else
            wl_log("Kept GodModeWii/wiiload/%s", name);
        return false;
    }

    if (ftype_sniff(head, headn, 0x100, name) == FT_PROGRAM) {
        char q[300];
        snprintf(q, sizeof(q),
                 "Start \"%s\" now?\n\nGodModeWii closes and starts this program. It stays in "
                 "GodModeWii/wiiload on the %s either way.",
                 name, g_devs[dev].tag);
#ifdef GMW_AUTOTEST
        wl_log("Would ask: start %s?", name);
        return false;
#endif
        if (ui_confirm("WIILOAD", C_CYAN, q)) {
            snprintf(launch, launchsz, "%s", path);
            return true;
        }
        wl_log("Kept GodModeWii/wiiload/%s", name);
        return false;
    }

    wl_log("Not an archive or a program: kept GodModeWii/wiiload/%s", name);
    vfs_update_space(dev);
    return false;
}

/* ---------------------------------------------------------------- entry */

bool wiiload_run(char *launch, size_t launchsz)
{
    if (!ftp_net_start()) return false;
    u32 ip = net_gethostip();
    if (!ip) {
        ui_message("NETWORK", C_RED, "The console has no IP address.");
        return false;
    }
    s32 ls = make_listener();
    if (ls < 0) {
        ui_message("WIILOAD", C_RED, "Couldn't open port 4299.");
        return false;
    }

    bool usb = false;
    int dev = install_pick_device(false);
    if (dev < 0) {
        dev = install_pick_device(true);
        usb = dev >= 0;
    }

    log_n = 0;
    wl_log("Listening on port %d", WIILOAD_PORT);
    bool launching = false, full = true;
    u64 last_draw = 0;

    while (!g_quit_request) {
        struct sockaddr_in a;
        u32 al = sizeof(a);
        s32 s = net_accept(ls, (struct sockaddr *)&a, &al);
        if (s >= 0) {
            u32 pip = ntohl(a.sin_addr.s_addr);
            if (!lan_peer(a.sin_addr.s_addr)) {
                wl_log("Refused %u.%u.%u.%u: not on the local network", pip >> 24,
                       (pip >> 16) & 255, (pip >> 8) & 255, pip & 255);
                net_close(s);
            } else if (dev < 0) {
                wl_log("Nowhere to put it: insert an SD card or a USB drive");
                net_close(s);
            } else {
                net_fcntl(s, F_SETFL, IOS_NONBLOCK);
                conn_t c = { s, now_ms(), pip, dev };
                char path[VFS_PATH_MAX];
                u8 head[0x100];
                u32 headn = 0;
                bool got = receive(&c, path, sizeof(path), head, &headn);
                net_close(s);

                if (got && handle(path, head, headn, dev, launch, launchsz)) {
                    launching = true;
                    break;
                }
                full = true;
                last_draw = 0;
#ifdef GMW_AUTOTEST
                draw(ip, dev, true, NULL, 0, 0);
                ui_test_dump();
                break;                              /* test builds: one upload */
#endif
            }
        }

        u32 btn = DetectInput(IN_DOWN);
        if (btn & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) break;
        if (btn & WPAD_BUTTON_1) {
            int d = install_pick_device(!usb);
            if (d >= 0) {
                usb = !usb;
                dev = d;
                wl_log("Installing onto the %s", usb ? g_devs[dev].tag : "SD card");
            } else
                wl_log("No %s found", usb ? "SD card" : "USB drive");
            last_draw = 0;
        }

        if (!last_draw || now_ms() - last_draw > 500) {
            draw(ip, dev, full, NULL, 0, 0);
            full = false;
            last_draw = now_ms();
        }
    }

    net_close(ls);
    return launching;
}

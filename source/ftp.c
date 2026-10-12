/*
 * GodModeWii FTP server.
 *
 * Inspired by ftpii (Joseph Jordan and contributors), rewritten around a
 * single-threaded, non-blocking state machine with a lot of guard rails:
 *
 *  - login is always required; the password is random per run and shown on
 *    the console screen, failed logins are throttled and lock the door
 *  - only private-network / link-local peers are accepted
 *  - PORT / PASV data connections must come from / go to the control peer
 *    (no FTP bounce, no data-connection hijacking)
 *  - every path is canonicalised inside a virtual root ("/sd", "/usb1",
 *    "/nand", ...) so nothing can escape or reach an unmounted device
 *  - read-only by default; SEEPROM and Disc are always read-only
 *  - NAND writes need the Konami code, typed on the console itself
 *  - uploads go to a temporary file and replace the target only when they
 *    finished, so a dropped connection never leaves a half-written file
 *  - bounded command lines, session count, buffers and timeouts
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdarg.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <network.h>
#include <poll.h>
#include <ogc/lwp_watchdog.h>

#include "ftp.h"
#include "app.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "risk.h"

#define FTP_PORT        21
#define FTP_PORT_ALT    2121
#define MAX_SESS        4
#define CMD_MAX         512
#define OUT_MAX         4096
#define IO_CHUNK        (32 * 1024)
#define CTL_IDLE_MS     (5 * 60 * 1000)
#define DATA_IDLE_MS    (30 * 1000)
#define DATA_WAIT_MS    (20 * 1000)
#define LOGIN_BAD_MAX   3
#define GLOBAL_BAD_MAX  8
#define GLOBAL_WINDOW   (60 * 1000)
#define GLOBAL_LOCK     (60 * 1000)
#define IOS_NONBLOCK    0x04
#define FTP_USER        "godmode"

enum { T_NONE, T_ACCEPT, T_CONNECT, T_SEND_FILE, T_RECV_FILE, T_SEND_LIST };
enum { OP_RETR, OP_STOR, OP_LIST, OP_NLST, OP_MLSD };

typedef struct {
    bool used, dead, quit;
    s32 ctl;
    u32 peer_ip;                      /* network order */
    char cmd[CMD_MAX];
    int clen;
    bool overflow;
    char out[OUT_MAX];
    int outlen;
    bool user_ok, authed;
    int bad;
    u64 last;
    char cwd[VFS_PATH_MAX];           /* virtual, canonical */

    s32 pasv;                         /* passive listener */
    bool port_set;
    struct sockaddr_in port_addr;
    u64 rest;
    bool have_rnfr;
    char rnfr[VFS_PATH_MAX];          /* real path */

    /* transfer */
    int ts, op;
    s32 ds;
    u64 t_start, t_last, t_bytes, t_total;
    char t_name[48];
    char t_real[VFS_PATH_MAX], t_tmp[VFS_PATH_MAX];
    vfs_file f;
    bool f_open;
    u8 *buf;
    u32 blen, bpos;
    bool eof;
    vfs_entry *ents;
    int nent, eidx;
    bool ro_list;
} sess_t;

static sess_t S[MAX_SESS];
static s32 listener = -1;
static int g_port;
static bool g_rw;
static char g_pass[10];
static bool net_ready;

static int g_fail_count;
static u64 g_fail_start, g_block_until;

/* ---------------------------------------------------------------- log */

#define LOG_LINES 7
static char logbuf[LOG_LINES][72];
static int log_n;
static bool log_dirty;

static void ftp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ftp_log(const char *fmt, ...)
{
    char *dst = logbuf[log_n % LOG_LINES];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, 72, fmt, ap);
    va_end(ap);
    log_n++;
    log_dirty = true;
}

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

/* ---------------------------------------------------------------- sockets */

static void set_nonblock(s32 s)
{
    net_fcntl(s, F_SETFL, IOS_NONBLOCK);
}

static bool would_block(s32 r)
{
    return r == -EAGAIN || r == -EINPROGRESS || r == -EALREADY;
}

static s32 make_listener(int port)
{
    s32 s = net_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int one = 1;
    net_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (net_bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || net_listen(s, 4) < 0) {
        net_close(s);
        return -1;
    }
    set_nonblock(s);
    return s;
}

/* Only RFC1918 / link-local peers may talk to the server. */
static bool lan_peer(u32 ip_net)
{
    u32 ip = ntohl(ip_net);
    u32 a = ip >> 24, b = (ip >> 16) & 0xFF;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || a == 127;
}

/* ---------------------------------------------------------------- replies */

static void reply(sess_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(sess_t *s, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    if (s->outlen + n > OUT_MAX) {      /* client isn't reading: drop it */
        s->dead = true;
        return;
    }
    memcpy(s->out + s->outlen, line, n);
    s->outlen += n;
}

static void flush_out(sess_t *s)
{
    if (!s->outlen || s->ctl < 0)
        return;
    s32 n = net_send(s->ctl, s->out, s->outlen, 0);
    if (n > 0) {
        memmove(s->out, s->out + n, s->outlen - n);
        s->outlen -= n;
    } else if (n < 0 && !would_block(n)) {
        s->dead = true;
    }
}

/* ---------------------------------------------------------------- paths */

static bool bad_char(unsigned char c)
{
    return c < 0x20 || c == 0x7F || strchr("\\:*?\"<>|", c) != NULL;
}

/* Resolves `arg` against `cwd` into a canonical virtual path ("/sd/a/b"). */
static bool canon(const char *cwd, const char *arg, char *out, size_t outsz)
{
    char tmp[VFS_PATH_MAX * 2];
    if (arg[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", arg);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);
    if (strlen(tmp) >= sizeof(tmp) - 1)
        return false;

    out[0] = '\0';
    size_t len = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t l = strcspn(p, "/");
        if (l == 1 && p[0] == '.') {
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            char *slash = strrchr(out, '/');
            if (slash) { *slash = '\0'; len = slash - out; }
        } else {
            if (l > 255 || len + l + 2 > outsz)
                return false;
            for (size_t i = 0; i < l; i++)
                if (bad_char((unsigned char)p[i]))
                    return false;
            out[len++] = '/';
            memcpy(out + len, p, l);
            len += l;
            out[len] = '\0';
        }
        p += l;
    }
    if (!len) { out[0] = '/'; out[1] = '\0'; }
    return true;
}

/* Virtual -> real path. *dev = -1 for the virtual root. */
static bool to_real(const char *v, char *real, size_t sz, int *dev)
{
    if (!strcmp(v, "/")) {
        *dev = -1;
        real[0] = '\0';
        return true;
    }
    const char *name = v + 1;
    size_t nl = strcspn(name, "/");
    for (int i = 0; i < DEV_COUNT; i++) {
        size_t pl = strlen(g_devs[i].prefix) - 1;       /* without ':' */
        if (nl == pl && !strncasecmp(name, g_devs[i].prefix, pl)) {
            if (!g_devs[i].mounted)
                return false;
            *dev = i;
            const char *rest = name + nl;
            if (*rest == '/') rest++;
            int n = snprintf(real, sz, "%s/%s", g_devs[i].prefix, rest);
            return n > 0 && (size_t)n < sz;
        }
    }
    return false;
}

/* Write permission with the same rules as the file browser. */
static bool can_write(const char *real, int dev, const char **why)
{
    if (dev < 0 || vfs_is_virtual(dev)) { *why = "Read-only location"; return false; }
    if (!g_rw)                          { *why = "Server is read-only"; return false; }
    if (dev == DEV_NAND) {
        /* The code is typed on the console itself; the FTP command waits. */
        ftp_log("NAND change asked: Konami code on the Wii");
        if (!risk_authorize(real, "let an FTP client modify")) {
            *why = "Not confirmed on the console";
            return false;
        }
    }
    return true;
}

static bool name_ok(int dev, const char *real)
{
    return dev != DEV_NAND || strlen(vfs_basename(real)) <= NAND_NAME_MAX;
}

/* ---------------------------------------------------------------- transfers */

static void data_close(sess_t *s)
{
    if (s->ds >= 0) { net_close(s->ds); s->ds = -1; }
}

static void pasv_close(sess_t *s)
{
    if (s->pasv >= 0) { net_close(s->pasv); s->pasv = -1; }
    s->port_set = false;
}

static void transfer_cleanup(sess_t *s, bool remove_tmp)
{
    data_close(s);
    if (s->f_open) { vfs_close(&s->f); s->f_open = false; }
    if (remove_tmp && s->op == OP_STOR && s->t_tmp[0])
        vfs_remove(s->t_tmp, false);
    s->t_tmp[0] = '\0';
    free(s->buf); s->buf = NULL;
    free(s->ents); s->ents = NULL;
    s->ts = T_NONE;
}

/* Finishes the transfer and sends the final reply. */
static void transfer_end(sess_t *s, bool ok, const char *err)
{
    bool stor = s->op == OP_STOR;
    char real[VFS_PATH_MAX];
    snprintf(real, sizeof(real), "%s", s->t_real);

    if (s->f_open) { vfs_close(&s->f); s->f_open = false; }
    data_close(s);

    if (ok && stor) {
        /* Replace the target only now that the whole upload is on disk. */
        char bak[VFS_PATH_MAX] = "";
        bool had_old = vfs_exists(real);
        bool good = true;
        if (had_old) {
            char dir[VFS_PATH_MAX];
            vfs_parent(real, dir, sizeof(dir));
            char name[16];
            snprintf(name, sizeof(name), ".gmw%04x.bak", (unsigned)(rand() & 0xFFFF));
            vfs_join(bak, sizeof(bak), dir, name);
            good = vfs_rename(real, bak);
        }
        if (good) {
            good = vfs_rename(s->t_tmp, real);
            if (!good && had_old)
                vfs_rename(bak, real);              /* put the old file back */
            else if (good && had_old)
                vfs_remove(bak, false);
        }
        if (!good) {
            ok = false;
            err = "Could not finalise the upload";
        } else {
            s->t_tmp[0] = '\0';
        }
    }

    if (ok) {
        reply(s, "226 Transfer complete");
        ftp_log("%s: done %s", stor ? "STOR" : "RETR", s->t_name);
    } else {
        reply(s, "%s", err ? err : "426 Transfer aborted");
        ftp_log("Aborted %s: %s", s->t_name, err ? err + 4 : "aborted");
    }
    if (stor)
        for (int i = 0; i < DEV_COUNT; i++)
            vfs_update_space(i);
    transfer_cleanup(s, !ok);
}

static void begin_data(sess_t *s, int op, const char *real)
{
    s->op = op;
    s->t_start = s->t_last = now_ms();
    s->t_bytes = 0;
    s->blen = s->bpos = 0;
    s->eof = false;
    snprintf(s->t_real, sizeof(s->t_real), "%s", real);

    if (s->pasv >= 0) {
        s->ts = T_ACCEPT;
    } else if (s->port_set) {
        s->ds = net_socket(AF_INET, SOCK_STREAM, 0);
        if (s->ds < 0) {
            reply(s, "425 Can't open data connection");
            transfer_cleanup(s, true);
            return;
        }
        set_nonblock(s->ds);
        s32 r = net_connect(s->ds, (struct sockaddr *)&s->port_addr, sizeof(s->port_addr));
        if (r < 0 && !would_block(r) && r != -EISCONN) {
            reply(s, "425 Can't open data connection");
            transfer_cleanup(s, true);
            return;
        }
        s->ts = T_CONNECT;
        s->port_set = false;
    }
    reply(s, "150 Opening data connection");
}

static void start_stream(sess_t *s)
{
    s->t_last = now_ms();
    if (s->op == OP_RETR) s->ts = T_SEND_FILE;
    else if (s->op == OP_STOR) s->ts = T_RECV_FILE;
    else s->ts = T_SEND_LIST;
}

static int fill_list(sess_t *s)
{
    s->blen = s->bpos = 0;
    while (s->eidx < s->nent && s->blen + 400 < IO_CHUNK) {
        vfs_entry *e = &s->ents[s->eidx++];
        char *dst = (char *)s->buf + s->blen;
        int n;
        if (s->op == OP_NLST)
            n = snprintf(dst, 400, "%s\r\n", e->name);
        else if (s->op == OP_MLSD)
            n = snprintf(dst, 400, "type=%s;size=%llu;modify=20000101000000;perm=%s; %s\r\n",
                         e->is_dir ? "dir" : "file", (unsigned long long)e->size,
                         e->is_dir ? (s->ro_list ? "el" : "cdeflmp") : (s->ro_list ? "r" : "radfw"), e->name);
        else
            n = snprintf(dst, 400, "%s 1 wii wii %llu Jan 01  2000 %s\r\n",
                         e->is_dir ? (s->ro_list ? "dr-xr-xr-x" : "drwxr-xr-x")
                                   : (s->ro_list ? "-r--r--r--" : "-rw-r--r--"),
                         (unsigned long long)e->size, e->name);
        if (n > 0)
            s->blen += n > 399 ? 399 : n;
    }
    return s->blen;
}

/* Returns true if it made progress. */
static bool pump_transfer(sess_t *s)
{
    u64 t = now_ms();
    switch (s->ts) {
    case T_ACCEPT: {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        memset(&a, 0, sizeof(a));
        s32 c = net_accept(s->pasv, (struct sockaddr *)&a, &al);
        if (c >= 0) {
            if (a.sin_addr.s_addr != s->peer_ip) {      /* not our client */
                net_close(c);
                ftp_log("Rejected foreign data connection");
                return true;
            }
            set_nonblock(c);
            s->ds = c;
            net_close(s->pasv);
            s->pasv = -1;
            start_stream(s);
            return true;
        }
        if (!would_block(c) || t - s->t_start > DATA_WAIT_MS)
            transfer_end(s, false, "425 Can't open data connection");
        return false;
    }
    case T_CONNECT: {
        struct pollsd sd = { s->ds, POLLOUT, 0 };
        s32 r = net_poll(&sd, 1, 0);
        if (r > 0 && (sd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            transfer_end(s, false, "425 Can't open data connection");
            return true;
        }
        if (r > 0 && (sd.revents & POLLOUT)) {
            start_stream(s);
            return true;
        }
        if (t - s->t_start > DATA_WAIT_MS)
            transfer_end(s, false, "425 Can't open data connection");
        return false;
    }
    case T_SEND_FILE:
    case T_SEND_LIST: {
        if (s->bpos == s->blen) {
            if (s->ts == T_SEND_FILE) {
                s32 n = vfs_read(&s->f, s->buf, IO_CHUNK);
                if (n < 0) { transfer_end(s, false, "451 Read error"); return true; }
                if (n == 0) { transfer_end(s, true, NULL); return true; }
                s->blen = n;
                s->bpos = 0;
            } else if (fill_list(s) == 0) {
                transfer_end(s, true, NULL);
                return true;
            }
        }
        s32 n = net_send(s->ds, s->buf + s->bpos, s->blen - s->bpos, 0);
        if (n > 0) {
            s->bpos += n;
            s->t_bytes += n;
            s->t_last = t;
            return true;
        }
        if (n == 0 || !would_block(n)) { transfer_end(s, false, "426 Connection closed"); return true; }
        if (t - s->t_last > DATA_IDLE_MS) transfer_end(s, false, "426 Data connection timed out");
        return false;
    }
    case T_RECV_FILE: {
        s32 n = net_recv(s->ds, s->buf, IO_CHUNK, 0);
        if (n > 0) {
            if (vfs_write(&s->f, s->buf, n) != n) {
                transfer_end(s, false, "552 Write failed (disk full?)");
                return true;
            }
            s->t_bytes += n;
            s->t_last = t;
            return true;
        }
        if (n == 0) { transfer_end(s, true, NULL); return true; }
        if (!would_block(n)) { transfer_end(s, false, "426 Connection error"); return true; }
        if (t - s->t_last > DATA_IDLE_MS) transfer_end(s, false, "426 Data connection timed out");
        return false;
    }
    }
    return false;
}

/* ---------------------------------------------------------------- commands */

static void do_list(sess_t *s, int op, const char *arg)
{
    while (*arg == '-') {                       /* ignore ls style flags */
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    char v[VFS_PATH_MAX], real[VFS_PATH_MAX];
    int dev;
    if (!canon(s->cwd, *arg ? arg : ".", v, sizeof(v)) || !to_real(v, real, sizeof(real), &dev)) {
        reply(s, "550 No such directory");
        return;
    }
    if (s->ts != T_NONE) { reply(s, "425 A transfer is already running"); return; }
    if (s->pasv < 0 && !s->port_set) { reply(s, "425 Use PASV or PORT first"); return; }

    vfs_entry *ents = NULL;
    int n = 0;
    if (dev < 0) {
        ents = calloc(DEV_COUNT, sizeof(vfs_entry));
        if (!ents) { reply(s, "451 Out of memory"); return; }
        for (int i = 0; i < DEV_COUNT; i++)
            if (g_devs[i].mounted) {
                snprintf(ents[n].name, sizeof(ents[n].name), "%s", g_devs[i].prefix);
                ents[n].name[strlen(ents[n].name) - 1] = '\0';   /* "sd:" -> "sd" */
                ents[n].is_dir = true;
                n++;
            }
        s->ro_list = false;
    } else {
        bool is_dir;
        u64 size;
        if (!vfs_stat(real, &is_dir, &size)) { reply(s, "550 No such file or directory"); return; }
        if (is_dir) {
            if (!vfs_list_visible(real, &ents, &n)) { reply(s, "550 Can't read directory"); return; }
        } else {
            ents = calloc(1, sizeof(vfs_entry));
            if (!ents) { reply(s, "451 Out of memory"); return; }
            snprintf(ents[0].name, sizeof(ents[0].name), "%s", vfs_basename(real));
            ents[0].size = size;
            n = 1;
        }
        s->ro_list = vfs_is_virtual(dev) || !g_rw || (dev == DEV_NAND && !risk_unlocked());
    }
    s->buf = malloc(IO_CHUNK);
    if (!s->buf) { free(ents); reply(s, "451 Out of memory"); return; }
    s->ents = ents;
    s->nent = n;
    s->eidx = 0;
    snprintf(s->t_name, sizeof(s->t_name), "%s", v);
    ftp_log("listing %s: %d entries", v, n);
    begin_data(s, op, real);
}

static void do_retr(sess_t *s, const char *arg)
{
    char v[VFS_PATH_MAX], real[VFS_PATH_MAX];
    int dev;
    if (!*arg || !canon(s->cwd, arg, v, sizeof(v)) || !to_real(v, real, sizeof(real), &dev) || dev < 0) {
        reply(s, "550 No such file");
        return;
    }
    bool is_dir;
    u64 size;
    if (!vfs_stat(real, &is_dir, &size) || is_dir) { reply(s, "550 Not a file"); return; }
    if (s->ts != T_NONE) { reply(s, "425 A transfer is already running"); return; }
    if (s->pasv < 0 && !s->port_set) { reply(s, "425 Use PASV or PORT first"); return; }
    s->buf = memalign(32, IO_CHUNK);
    if (!s->buf) { reply(s, "451 Out of memory"); return; }
    if (!vfs_open_read(&s->f, real)) {
        free(s->buf); s->buf = NULL;
        reply(s, "550 Can't open file");
        return;
    }
    s->f_open = true;
    if (s->rest && !vfs_seek(&s->f, s->rest)) {
        transfer_cleanup(s, false);
        reply(s, "554 Can't seek");
        s->rest = 0;
        return;
    }
    s->rest = 0;
    s->t_total = size;
    snprintf(s->t_name, sizeof(s->t_name), "%s", vfs_basename(real));
    begin_data(s, OP_RETR, real);
}

static void do_stor(sess_t *s, const char *arg)
{
    char v[VFS_PATH_MAX], real[VFS_PATH_MAX];
    int dev;
    if (!*arg || !canon(s->cwd, arg, v, sizeof(v)) || !to_real(v, real, sizeof(real), &dev) || dev < 0) {
        reply(s, "553 Bad file name");
        return;
    }
    const char *why;
    if (s->rest) { reply(s, "501 Resuming uploads is not supported"); s->rest = 0; return; }
    if (!can_write(real, dev, &why)) { reply(s, "550 %s", why); return; }
    if (!name_ok(dev, real)) { reply(s, "553 NAND file names are limited to 12 characters"); return; }
    bool is_dir;
    if (vfs_stat(real, &is_dir, NULL) && is_dir) { reply(s, "553 A folder with that name exists"); return; }
    char dir[VFS_PATH_MAX];
    if (!vfs_parent(real, dir, sizeof(dir)) || !vfs_stat(dir, &is_dir, NULL) || !is_dir) {
        reply(s, "550 Target folder doesn't exist");
        return;
    }
    if (s->ts != T_NONE) { reply(s, "425 A transfer is already running"); return; }
    if (s->pasv < 0 && !s->port_set) { reply(s, "425 Use PASV or PORT first"); return; }

    char name[16], tmp[VFS_PATH_MAX];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), ".gmw%04x.tmp", (unsigned)(rand() & 0xFFFF));
        vfs_join(tmp, sizeof(tmp), dir, name);
        if (!vfs_exists(tmp)) break;
    }
    s->buf = memalign(32, IO_CHUNK);
    if (!s->buf) { reply(s, "451 Out of memory"); return; }
    s->op = OP_STOR;
    if (!vfs_open_write(&s->f, tmp)) {
        free(s->buf); s->buf = NULL;
        reply(s, "550 Can't create file");
        return;
    }
    s->f_open = true;
    snprintf(s->t_tmp, sizeof(s->t_tmp), "%s", tmp);
    snprintf(s->t_name, sizeof(s->t_name), "%s", vfs_basename(real));
    begin_data(s, OP_STOR, real);
}

static void do_pasv(sess_t *s, bool ext)
{
    if (s->ts != T_NONE) { reply(s, "425 A transfer is already running"); return; }
    pasv_close(s);
    for (int i = 0; i < 12; i++) {
        int port = 49152 + (rand() % 16000);
        s32 l = make_listener(port);
        if (l < 0) continue;
        s->pasv = l;
        u32 ip = net_gethostip();
        if (ext)
            reply(s, "229 Entering Extended Passive Mode (|||%d|)", port);
        else
            reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%d,%d)", (ip >> 24) & 255, (ip >> 16) & 255,
                  (ip >> 8) & 255, ip & 255, port >> 8, port & 255);
        return;
    }
    reply(s, "425 Can't open passive port");
}

static void do_port(sess_t *s, const char *arg)
{
    unsigned a[6];
    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        reply(s, "501 Bad PORT arguments");
        return;
    }
    for (int i = 0; i < 6; i++)
        if (a[i] > 255) { reply(s, "501 Bad PORT arguments"); return; }
    u32 ip = htonl((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]);
    int port = (a[4] << 8) | a[5];
    if (ip != s->peer_ip || port < 1024) {           /* no FTP bounce */
        reply(s, "504 PORT must point back at you (port >= 1024)");
        return;
    }
    pasv_close(s);
    memset(&s->port_addr, 0, sizeof(s->port_addr));
    s->port_addr.sin_len = sizeof(s->port_addr);
    s->port_addr.sin_family = AF_INET;
    s->port_addr.sin_port = htons(port);
    s->port_addr.sin_addr.s_addr = ip;
    s->port_set = true;
    reply(s, "200 PORT command successful");
}

static bool pass_ok(const char *given)
{
    size_t gl = strlen(given), pl = strlen(g_pass);
    unsigned diff = gl ^ pl;
    for (size_t i = 0; i < pl; i++)
        diff |= (unsigned char)g_pass[i] ^ (unsigned char)(i < gl ? given[i] : 0);
    return diff == 0;
}

static void note_login_failure(sess_t *s)
{
    u64 t = now_ms();
    s->bad++;
    if (t - g_fail_start > GLOBAL_WINDOW) { g_fail_start = t; g_fail_count = 0; }
    if (++g_fail_count >= GLOBAL_BAD_MAX) {
        g_block_until = t + GLOBAL_LOCK;
        ftp_log("Too many bad logins - refusing clients for 60s");
    }
    ftp_log("Bad login from %u.%u.%u.%u", s->peer_ip & 255, (s->peer_ip >> 8) & 255,
            (s->peer_ip >> 16) & 255, s->peer_ip >> 24);
}

static void run_command(sess_t *s, char *line)
{
    char *arg = line;
    while (*arg && *arg != ' ') { *arg = toupper((unsigned char)*arg); arg++; }
    if (*arg) *arg++ = '\0';
    while (*arg == ' ') arg++;
    const char *cmd = line;
    s->last = now_ms();
    if (strcmp(cmd, "PASS") && strcmp(cmd, "NOOP"))
        ftp_log("> %s %.40s", cmd, strcmp(cmd, "USER") ? arg : "");

    if (!strcmp(cmd, "QUIT")) { reply(s, "221 Goodbye"); s->quit = true; return; }
    if (!strcmp(cmd, "NOOP")) { reply(s, "200 OK"); return; }
    if (!strcmp(cmd, "FEAT")) {
        reply(s, "211-Features:");
        reply(s, " SIZE");
        reply(s, " REST STREAM");
        reply(s, " MLST type*;size*;modify*;perm*;");
        reply(s, " UTF8");
        reply(s, "211 End");
        return;
    }
    if (!strcmp(cmd, "AUTH") || !strcmp(cmd, "PBSZ") || !strcmp(cmd, "PROT")) {
        reply(s, "502 TLS isn't supported (plain FTP on a trusted network only)");
        return;
    }
    if (!strcmp(cmd, "SYST")) { reply(s, "215 UNIX Type: L8"); return; }
    if (!strcmp(cmd, "OPTS")) {
        if (!strncasecmp(arg, "MLST", 4))
            reply(s, "200 MLST OPTS type;size;modify;perm;");
        else
            reply(s, "200 OK");
        return;
    }

    if (!strcmp(cmd, "USER")) {
        s->authed = false;
        s->user_ok = !strcmp(arg, FTP_USER);
        reply(s, "331 Password required");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        if (!s->user_ok) { reply(s, "503 Send USER first"); return; }
        if (pass_ok(arg)) {
            s->authed = true;
            ftp_log("Login ok");
            reply(s, "230 Logged in. %s", g_rw ? "Read/write." : "Read-only.");
        } else {
            note_login_failure(s);
            s->user_ok = false;
            reply(s, "530 Login incorrect");
            if (s->bad >= LOGIN_BAD_MAX) {
                reply(s, "421 Too many failed logins");
                s->quit = true;
            }
        }
        return;
    }
    if (!s->authed) { reply(s, "530 Please log in with USER and PASS"); return; }

    char v[VFS_PATH_MAX], real[VFS_PATH_MAX];
    int dev;
    const char *why;

    if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        char q[VFS_PATH_MAX * 2];
        int k = 0;
        for (const char *c = s->cwd; *c && k < (int)sizeof(q) - 2; c++) {
            if (*c == '"') q[k++] = '"';
            q[k++] = *c;
        }
        q[k] = '\0';
        reply(s, "257 \"%s\" is the current directory", q);
        return;
    }
    if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD") || !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
        bool up = !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP");
        const char *target = up ? ".." : arg;
        if (!canon(s->cwd, target, v, sizeof(v)) || !to_real(v, real, sizeof(real), &dev)) {
            reply(s, "550 No such directory");
            return;
        }
        bool is_dir = true;
        if (dev >= 0 && !vfs_stat(real, &is_dir, NULL)) is_dir = false;
        if (!is_dir) { reply(s, "550 Not a directory"); return; }
        snprintf(s->cwd, sizeof(s->cwd), "%s", v);
        reply(s, "250 Directory changed");
        return;
    }
    if (!strcmp(cmd, "TYPE")) { reply(s, "200 Type set (files are always binary)"); return; }
    if (!strcmp(cmd, "MODE")) { reply(s, !strcasecmp(arg, "S") ? "200 Stream mode" : "504 Only stream mode"); return; }
    if (!strcmp(cmd, "STRU")) { reply(s, !strcasecmp(arg, "F") ? "200 File structure" : "504 Only file structure"); return; }
    if (!strcmp(cmd, "ALLO")) { reply(s, "202 Not needed"); return; }
    if (!strcmp(cmd, "PASV")) { do_pasv(s, false); return; }
    if (!strcmp(cmd, "EPSV")) { do_pasv(s, true); return; }
    if (!strcmp(cmd, "PORT")) { do_port(s, arg); return; }
    if (!strcmp(cmd, "EPRT")) { reply(s, "502 Use PASV / EPSV"); return; }
    if (!strcmp(cmd, "LIST")) { do_list(s, OP_LIST, arg); return; }
    if (!strcmp(cmd, "NLST")) { do_list(s, OP_NLST, arg); return; }
    if (!strcmp(cmd, "MLSD")) { do_list(s, OP_MLSD, arg); return; }
    if (!strcmp(cmd, "RETR")) { do_retr(s, arg); return; }
    if (!strcmp(cmd, "STOR")) { do_stor(s, arg); return; }
    if (!strcmp(cmd, "APPE")) { reply(s, "502 APPE isn't supported"); return; }
    if (!strcmp(cmd, "REST")) {
        char *end;
        unsigned long long r = strtoull(arg, &end, 10);
        if (end == arg || *end) { reply(s, "501 Bad offset"); return; }
        s->rest = r;
        reply(s, "350 Restarting at %llu", r);
        return;
    }
    if (!strcmp(cmd, "ABOR")) {
        if (s->ts != T_NONE) {
            transfer_end(s, false, "426 Transfer aborted");
            reply(s, "226 ABOR done");
        } else {
            reply(s, "226 Nothing to abort");
        }
        return;
    }
    if (!strcmp(cmd, "STAT")) { reply(s, "211 GodModeWii FTP, %s", g_rw ? "read/write" : "read-only"); return; }
    if (!strcmp(cmd, "MDTM")) { reply(s, "550 Not available"); return; }

    /* everything below takes a path */
    if (!strcmp(cmd, "SIZE") || !strcmp(cmd, "DELE") || !strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD") ||
        !strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD") || !strcmp(cmd, "RNFR") || !strcmp(cmd, "RNTO")) {
        if (!*arg || !canon(s->cwd, arg, v, sizeof(v)) || !to_real(v, real, sizeof(real), &dev)) {
            reply(s, "550 Bad or unavailable path");
            return;
        }
        bool is_dir = false;
        u64 size = 0;
        bool exists = dev >= 0 && vfs_stat(real, &is_dir, &size);

        if (!strcmp(cmd, "SIZE")) {
            if (!exists || is_dir) reply(s, "550 Not a file");
            else reply(s, "213 %llu", (unsigned long long)size);
            return;
        }
        if (dev < 0 || vfs_is_root(real)) { reply(s, "550 Not allowed here"); return; }
        if (!strcmp(cmd, "RNTO")) {
            if (!s->have_rnfr) { reply(s, "503 Send RNFR first"); return; }
            s->have_rnfr = false;
            if (vfs_dev_of(s->rnfr) != dev) { reply(s, "553 Can't move between drives"); return; }
            if (!can_write(real, dev, &why)) { reply(s, "550 %s", why); return; }
            if (!name_ok(dev, real)) { reply(s, "553 NAND file names are limited to 12 characters"); return; }
            if (exists) { reply(s, "553 Target already exists"); return; }
            if (vfs_is_within(real, s->rnfr)) { reply(s, "553 Can't move a folder into itself"); return; }
            reply(s, vfs_rename(s->rnfr, real) ? "250 Renamed" : "550 Rename failed");
            return;
        }
        if (!strcmp(cmd, "RNFR")) {
            if (!exists) { reply(s, "550 No such file"); return; }
            snprintf(s->rnfr, sizeof(s->rnfr), "%s", real);
            s->have_rnfr = true;
            reply(s, "350 Ready for RNTO");
            return;
        }
        s->have_rnfr = false;
        if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
            if (!can_write(real, dev, &why)) { reply(s, "550 %s", why); return; }
            if (!name_ok(dev, real)) { reply(s, "553 NAND names are limited to 12 characters"); return; }
            if (exists) { reply(s, "550 Already exists"); return; }
            reply(s, vfs_mkdir(real) ? "257 Folder created" : "550 Can't create folder");
            return;
        }
        if (!exists) { reply(s, "550 No such file or directory"); return; }
        if (!strcmp(cmd, "DELE")) {
            if (is_dir) { reply(s, "550 That's a folder (use RMD)"); return; }
            if (!can_write(real, dev, &why)) { reply(s, "550 %s", why); return; }
            reply(s, vfs_remove(real, false) ? "250 Deleted" : "550 Can't delete");
            for (int i = 0; i < DEV_COUNT; i++) vfs_update_space(i);
            return;
        }
        /* RMD: empty folders only; never recursive over the network */
        if (!is_dir) { reply(s, "550 Not a folder"); return; }
        if (!can_write(real, dev, &why)) { reply(s, "550 %s", why); return; }
        vfs_entry *l;
        int n;
        if (!vfs_list(real, &l, &n)) { reply(s, "550 Can't read folder"); return; }
        free(l);
        if (n > 0) { reply(s, "550 Folder isn't empty"); return; }
        reply(s, vfs_remove(real, true) ? "250 Removed" : "550 Can't remove folder");
        return;
    }
    if (!strcmp(cmd, "SITE") || !strcmp(cmd, "CHMOD")) { reply(s, "502 Not supported"); return; }
    reply(s, "500 Unknown command");
}

/* ---------------------------------------------------------------- sessions */

static void sess_close(sess_t *s)
{
    if (s->ts != T_NONE || s->f_open)
        transfer_cleanup(s, true);
    pasv_close(s);
    data_close(s);
    if (s->ctl >= 0) net_close(s->ctl);
    memset(s, 0, sizeof(*s));
    s->ctl = s->pasv = s->ds = -1;
}

static void sess_open(sess_t *s, s32 c, u32 ip)
{
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->ctl = c;
    s->pasv = s->ds = -1;
    s->peer_ip = ip;
    s->last = now_ms();
    snprintf(s->cwd, sizeof(s->cwd), "/");
    reply(s, "220 GodModeWii FTP ready. Log in with USER %s and the password shown on the Wii.", FTP_USER);
}

static void accept_new(void)
{
    for (;;) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        memset(&a, 0, sizeof(a));
        s32 c = net_accept(listener, (struct sockaddr *)&a, &al);
        if (c < 0)
            return;
        set_nonblock(c);

        const char *deny = NULL;
        if (!lan_peer(a.sin_addr.s_addr)) deny = "421 Local network clients only\r\n";
        else if (now_ms() < g_block_until) deny = "421 Temporarily locked after failed logins\r\n";
        sess_t *slot = NULL;
        for (int i = 0; i < MAX_SESS && !deny; i++)
            if (!S[i].used) { slot = &S[i]; break; }
        if (!deny && !slot) deny = "421 Too many connections\r\n";

        if (deny) {
            net_send(c, deny, strlen(deny), 0);
            net_close(c);
            continue;
        }
        sess_open(slot, c, a.sin_addr.s_addr);
        ftp_log("Client connected: %u.%u.%u.%u", a.sin_addr.s_addr & 255, (a.sin_addr.s_addr >> 8) & 255,
                (a.sin_addr.s_addr >> 16) & 255, a.sin_addr.s_addr >> 24);
    }
}

static bool pump_session(sess_t *s)
{
    bool progress = false;

    /* control input */
    if (!s->dead) {
        char in[256];
        s32 n = net_recv(s->ctl, in, sizeof(in), 0);
        if (n == 0) s->dead = true;
        else if (n < 0 && !would_block(n)) s->dead = true;
        for (s32 i = 0; i < n && !s->dead; i++) {
            char c = in[i];
            if (c == '\n') {
                if (s->overflow) reply(s, "500 Line too long");
                else {
                    s->cmd[s->clen] = '\0';
                    run_command(s, s->cmd);
                }
                s->clen = 0;
                s->overflow = false;
                progress = true;
            } else if (c != '\r' && c != '\0') {
                if (s->clen < CMD_MAX - 1) s->cmd[s->clen++] = c;
                else s->overflow = true;
            }
        }
    }
    if (s->ts != T_NONE && !s->dead)
        progress |= pump_transfer(s);

    flush_out(s);
    if (s->quit && !s->outlen)
        s->dead = true;
    u64 t = now_ms();           /* taken after the commands: s->last may have moved */
    if (!s->dead && s->ts == T_NONE && t > s->last && t - s->last > CTL_IDLE_MS) {
        reply(s, "421 Idle timeout");
        s->quit = true;
    }
    if (s->dead) {
        flush_out(s);
        ftp_log("Client disconnected");
        sess_close(s);
        progress = true;
    }
    return progress;
}

/* ---------------------------------------------------------------- screen */

static void draw(u32 ip)
{
    int cols = ui_cols(), rows = ui_rows();
    ui_clear();
    ui_bg(risk_unlocked() ? C_RED : C_BLUE);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " GodModeWii v%s  -  FTP SERVER", GMW_VERSION);
    ui_reset();

    ui_color(C_GREEN, true);
    ui_field(2, 1, cols - 2, "Server running");
    ui_color(C_WHITE, true);
    ui_field(4, 1, cols - 2, "Address :  ftp://%u.%u.%u.%u%s", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
             g_port == 21 ? "" : ":2121");
    ui_field(5, 1, cols - 2, "User    :  %s", FTP_USER);
    ui_field(6, 1, cols - 2, "Password:  %s", g_pass);
    ui_color(g_rw ? C_YELLOW : C_CYAN, true);
    ui_field(7, 1, cols - 2, "Mode    :  %s", g_rw ? "READ / WRITE (NAND still needs the Konami unlock)" : "READ-ONLY");
    ui_color(C_WHITE, false);
    ui_field(8, 1, cols - 2, "Plain FTP: use it on a trusted network only.");

    ui_color(C_CYAN, true);
    ui_field(10, 1, cols - 2, "Sessions");
    ui_reset();
    int row = 11;
    u64 t = now_ms();
    for (int i = 0; i < MAX_SESS; i++) {
        sess_t *s = &S[i];
        if (!s->used) continue;
        char ip_s[24];
        snprintf(ip_s, sizeof(ip_s), "%u.%u.%u.%u", s->peer_ip & 255, (s->peer_ip >> 8) & 255,
                 (s->peer_ip >> 16) & 255, s->peer_ip >> 24);
        char tr[48] = "";
        if (s->ts >= T_SEND_FILE) {
            char sz[24];
            u64 el = t - s->t_start + 1;
            ui_fmt_size(sz, sizeof(sz), s->t_bytes * 1000 / el);
            snprintf(tr, sizeof(tr), "%s %s/s", s->op == OP_STOR ? "up" : "down", sz);
        }
        ui_color(s->authed ? C_WHITE : C_YELLOW, s->authed);
        ui_field(row++, 2, cols - 3, "%-15s %-9s %-14s %s", ip_s, s->authed ? "logged in" : "login...", tr, s->ts >= T_SEND_FILE ? s->t_name : "");
        ui_reset();
    }
    int logrow = rows - 3 - LOG_LINES;
    ui_color(C_CYAN, true);
    ui_field(logrow - 1, 1, cols - 2, "Log");
    ui_color(C_WHITE, false);
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = log_n - LOG_LINES + i;
        if (idx >= 0)
            ui_field(logrow + i, 2, cols - 3, "%s", logbuf[idx % LOG_LINES]);
    }
    ui_color(C_CYAN, true);
    ui_field(rows - 2, 1, cols - 2, "B / HOME: stop the server");
    ui_reset();
}

/* ---------------------------------------------------------------- entry */

static void gen_password(void)
{
    static const char set[] = "abcdefghjkmnpqrstuvwxyz23456789";
    srand((unsigned)(gettime() ^ (gettime() >> 32)));
    for (int i = 0; i < 8; i++)
        g_pass[i] = set[rand() % (sizeof(set) - 1)];
    g_pass[8] = '\0';
}

bool ftp_run(bool read_write)
{
    ui_splash("Connecting to the network...");
    if (!net_ready) {
        if (net_init() < 0) {
            ui_message("NETWORK", C_RED, "Couldn't connect to the network.\n\nCheck the connection in Wii Settings > Internet and try again.");
            return false;
        }
        net_ready = true;
    }
    u32 ip = net_gethostip();
    if (!ip) {
        ui_message("NETWORK", C_RED, "The console has no IP address.");
        return false;
    }

    g_port = FTP_PORT;
    listener = make_listener(g_port);
    if (listener < 0) {
        g_port = FTP_PORT_ALT;
        listener = make_listener(g_port);
    }
    if (listener < 0) {
        ui_message("FTP", C_RED, "Couldn't open the FTP port.");
        return false;
    }

    g_rw = read_write;
    gen_password();
    g_fail_count = 0;
    g_block_until = 0;
    log_n = 0;
    for (int i = 0; i < MAX_SESS; i++) { memset(&S[i], 0, sizeof(S[i])); S[i].ctl = S[i].pasv = S[i].ds = -1; }
    ftp_log("Listening on port %d", g_port);

    u64 last_draw = 0;
    bool stop = false;
    draw(ip);

    while (!stop && !g_quit_request) {
        accept_new();
        bool progress = false;
        for (int round = 0; round < 6; round++) {
            bool p = false;
            for (int i = 0; i < MAX_SESS; i++)
                if (S[i].used)
                    p |= pump_session(&S[i]);
            progress |= p;
            if (!p) break;
        }

        u32 btn = progress ? input_poll_nowait() : DetectInput(IN_DOWN);
        if (btn & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) {
            bool busy = false;
            for (int i = 0; i < MAX_SESS; i++)
                if (S[i].used && S[i].ts != T_NONE) busy = true;
            if (!busy || ui_confirm("STOP SERVER", C_YELLOW, "A transfer is running. Stop the server anyway?"))
                stop = true;
            else
                last_draw = 0;
        }
        u64 t = now_ms();
        if (log_dirty || t - last_draw > 400) {
            draw(ip);
            last_draw = t;
            log_dirty = false;
        }
    }

    for (int i = 0; i < MAX_SESS; i++)
        if (S[i].used)
            sess_close(&S[i]);
    net_close(listener);
    listener = -1;
    return true;
}

void ftp_net_shutdown(void)
{
    if (net_ready) {
        net_deinit();
        net_ready = false;
    }
}

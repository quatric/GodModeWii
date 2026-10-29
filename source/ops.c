#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "ops.h"
#include "ui.h"
#include "vfs.h"
#include "risk.h"
#include "input.h"
#include "virt.h"

#define CHUNK (128 * 1024)

#define MAX_ERRS 64

enum { POL_ASK, POL_OVERWRITE, POL_SKIP };

static struct {
    const char *title;
    u64 total_bytes, done_bytes;
    u64 file_total, file_done;
    u32 files_total, files_done;
    u64 start_ms, last_ms;
    int policy;
    bool cancelled;
    bool redraw;
    int fails;
    int disc_err_files;
    u64 disc_err_bytes;
    char errs[MAX_ERRS][VFS_PATH_MAX + 64];
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    u8 *buf;
} X;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static void fail(const char *what, const char *path)
{
    if (X.fails < MAX_ERRS)
        snprintf(X.errs[X.fails], sizeof(X.errs[0]), "%s:\n%s", what, path);
    X.fails++;
}

/* ---------------------------------------------------------------- progress */

/* From the average so far; byte counts when there are any, else file counts (deletes). */
static void time_left(char *out, size_t n, u64 el_ms)
{
    u64 done = X.total_bytes ? X.done_bytes : X.files_done;
    u64 total = X.total_bytes ? X.total_bytes : X.files_total;
    out[0] = '\0';
    if (el_ms < 2000 || !done || done >= total)
        return;
    u64 rate = done * 1000 / el_ms;
    if (!rate)
        return;
    u64 sec = (total - done) / rate;
    if (sec >= 3600)
        snprintf(out, n, "Time left: %u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    else
        snprintf(out, n, "Time left: %u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

static void prog_paint(bool full)
{
    int cols = ui_cols();
    int w = cols - 2;
    if (full) {
        ui_clear();
        ui_bg(C_BLUE);
        ui_color(C_WHITE, true);
        ui_field(0, 0, cols - 1, " %s", X.title);
        ui_reset();
        ui_color(C_CYAN, true);
        ui_field(2, 1, 6, "From:");
        ui_field(3, 1, 6, "To:");
        ui_field(9, 1, w, "Overall");
        ui_field(5, 1, w, "Current file");
        ui_color(C_CYAN, true);
        ui_field(ui_rows() - 2, 1, w, "Hold B to cancel");
        ui_reset();
    }
    char a[32], b[32], line[96];
    ui_color(C_WHITE, false);
    ui_field(2, 8, cols - 10, "%s", X.src);
    if (X.dst[0])
        ui_field(3, 8, cols - 10, "%s", X.dst);

    int fp = X.file_total ? (int)(X.file_done * 100 / X.file_total) : 100;
    ui_bar(6, 1, w - 8, fp, C_CYAN);
    ui_color(C_WHITE, true);
    ui_field(6, w - 6, 6, "%3d%%", fp);

    int op = X.total_bytes ? (int)(X.done_bytes * 100 / X.total_bytes) : 100;
    if (!X.total_bytes && X.files_total)
        op = X.files_done * 100 / X.files_total;
    ui_bar(10, 1, w - 8, op, C_GREEN);
    ui_color(C_WHITE, true);
    ui_field(10, w - 6, 6, "%3d%%", op);

    u64 el = now_ms() - X.start_ms;
    ui_fmt_size(a, sizeof(a), X.done_bytes);
    ui_fmt_size(b, sizeof(b), X.total_bytes);
    char sp[32] = "";
    if (el > 500 && X.done_bytes) {
        ui_fmt_size(sp, sizeof(sp), X.done_bytes * 1000 / el);
        strncat(sp, "/s", sizeof(sp) - strlen(sp) - 1);
    }
    snprintf(line, sizeof(line), "%s / %s   %s", a, b, sp);
    ui_color(C_WHITE, false);
    ui_field(12, 1, w, "%s", line);
    ui_field(13, 1, w, "Files: %u / %u", (unsigned)X.files_done, (unsigned)X.files_total);
    char eta[32];
    time_left(eta, sizeof(eta), el);
    ui_field(14, 1, w, "%s", eta);
    ui_reset();
}

/* Returns true if the user asked to stop. */
static bool prog_tick(bool force)
{
    u64 t = now_ms();
    if (!force && t - X.last_ms < 120)
        return X.cancelled;
    X.last_ms = t;
    if (X.redraw) {
        prog_paint(true);
        X.redraw = false;
    } else {
        prog_paint(false);
    }
    if (input_cancel_held()) {
        X.cancelled = true;
    }
    return X.cancelled;
}

static void begin(const char *title, u64 bytes, u32 files)
{
    memset(&X, 0, sizeof(X));
    X.title = title;
    X.total_bytes = bytes;
    X.files_total = files;
    X.start_ms = X.last_ms = now_ms();
    X.buf = memalign(32, CHUNK);
    prog_paint(true);
}

static void finish(void)
{
    free(X.buf);
    X.buf = NULL;
}

/* ---------------------------------------------------------------- scanning */

bool ops_tree_info(const char *path, u64 *bytes, u32 *files, u32 *dirs)
{
    bool is_dir;
    u64 size;
    if (!vfs_stat(path, &is_dir, &size))
        return false;
    if (!is_dir) {
        *bytes += size;
        (*files)++;
        return true;
    }
    (*dirs)++;
    vfs_entry *list;
    int n;
    if (!vfs_list(path, &list, &n))
        return false;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        char child[VFS_PATH_MAX];
        if (!vfs_join(child, sizeof(child), path, list[i].name))
            continue;
        if (list[i].is_dir)
            ok &= ops_tree_info(child, bytes, files, dirs);
        else {
            *bytes += list[i].size;
            (*files)++;
        }
    }
    free(list);
    return ok;
}

/* ---------------------------------------------------------------- copy */

/* "<copy>.errors.txt" next to a file copied off a disc with unreadable parts */
static bool write_error_log(const char *src, const char *dst, u64 size)
{
    char logp[VFS_PATH_MAX];
    if (snprintf(logp, sizeof(logp), "%s.errors.txt", dst) >= (int)sizeof(logp))
        return false;
    vfs_file lf;
    if (!vfs_open_write(&lf, logp))
        return false;
    const size_t cap = 4096;
    char *buf = memalign(32, cap);
    if (!buf) { vfs_close(&lf); return false; }
    size_t len = 0;
    bool ok = true;
    u32 n = virt_disc_error_count();
    len += snprintf(buf + len, cap - len, "Disc read errors\n\nSource: %s\nCopy: %s\nFile size: %llu bytes\n", src, dst, (unsigned long long)size);
    len += snprintf(buf + len, cap - len, "Unreadable ranges: %u%s (%llu bytes in total)\n", (unsigned)n,
                    virt_disc_error_overflow() ? " (the last one is a merge of many more)" : "",
                    (unsigned long long)virt_disc_error_bytes());
    len += snprintf(buf + len, cap - len, "Those bytes were written as zeros. Offsets are inside the copied file; the\n"
                    "disc offset is where the drive failed to read (data area for Wii discs).\n\n");
    for (u32 i = 0; i <= n && ok; i++) {
        if (i < n) {
            u64 fo, dof, ln;
            virt_disc_error_get(i, &fo, &dof, &ln);
            len += snprintf(buf + len, cap - len, "0x%llX - 0x%llX  (%llu bytes)  disc offset 0x%llX\n",
                            (unsigned long long)fo, (unsigned long long)(fo + ln - 1), (unsigned long long)ln,
                            (unsigned long long)dof);
        }
        if (len > cap - 160 || i == n) {
            ok = vfs_write(&lf, buf, (u32)len) == (s32)len;
            len = 0;
        }
    }
    free(buf);
    return vfs_close_checked(&lf) && ok;
}

/* Where in the file and why, so a report tells a failing card from a filesystem problem. */
static const char *io_error(const char *what, u64 at)
{
    static char msg[128];
    int err = errno;
    snprintf(msg, sizeof(msg), "%s at byte %llu (%s)", what, (unsigned long long)at, err ? strerror(err) : "no error code");
    return msg;
}

static bool copy_file(const char *src, const char *dst)
{
    vfs_file in, out;
    snprintf(X.src, sizeof(X.src), "%s", src);
    snprintf(X.dst, sizeof(X.dst), "%s", dst);
    X.file_done = 0;

    if (!vfs_open_read(&in, src)) {
        fail("Cannot read", src);
        return false;
    }
    X.file_total = in.size;
    int dd = vfs_dev_of(dst);
    if (dd < DEV_NAND && !g_devs[dd].exfat && !g_devs[dd].ntfs && in.size > 0xFFFFFFFFULL) {
        vfs_close(&in);
        fail("Too big for FAT (4 GiB file limit)", dst);
        return false;
    }
    if (!vfs_open_write(&out, dst)) {
        vfs_close(&in);
        fail("Cannot create", dst);
        return false;
    }
    vfs_unbuffered(&in);
    vfs_unbuffered(&out);

    bool ok = true;
    /* Discs: a bad sector doesn't stop the copy; it is zero-filled and written to a log. */
    bool disc_src = vfs_dev_of(src) == DEV_DISC && !vfs_is_archive(src);
    if (disc_src)
        virt_disc_tolerate(true, input_cancel_held);
    for (;;) {
        errno = 0;
        s32 r = vfs_read(&in, X.buf, CHUNK);
        if (r < 0) {
            if (disc_src && input_cancel_held())
                X.cancelled = true;
            else
                fail(io_error("Read error", X.file_done), src);
            ok = false;
            break;
        }
        if (r == 0)
            break;
        errno = 0;
        if (vfs_write(&out, X.buf, r) != r) {
            fail(g_write_err ? g_write_err : io_error("Write error (disk full?)", X.file_done), dst);
            ok = false;
            break;
        }
        X.file_done += r;
        X.done_bytes += r;
        if (prog_tick(false)) {
            ok = false;
            break;
        }
    }
    u32 disc_errs = 0;
    if (disc_src) {
        disc_errs = virt_disc_error_count();
        if (ok && disc_errs) {
            X.disc_err_files++;
            X.disc_err_bytes += virt_disc_error_bytes();
        }
    }
    vfs_close(&in);
    if (!vfs_close_checked(&out) && ok) {
        fail(g_write_err ? g_write_err : "Write error", dst);
        ok = false;
    }
    if (disc_src && ok && disc_errs && !write_error_log(src, dst, X.file_total))
        fail("Could not write the read-error log", dst);
    if (disc_src)
        virt_disc_tolerate(false, NULL);

    if (!ok) {
        vfs_remove(dst, false);         /* never leave half a file behind */
        return false;
    }
    vfs_clone_attrs(src, dst);
    X.files_done++;
    prog_tick(true);
    return true;
}

static bool name_fits(const char *dst_dir, const char *name)
{
    return vfs_dev_of(dst_dir) != DEV_NAND || strlen(name) <= NAND_NAME_MAX;
}

static bool ask_overwrite(const char *dst)
{
    if (X.policy == POL_OVERWRITE) return true;
    if (X.policy == POL_SKIP) return false;
    static const char *items[] = { "Overwrite", "Overwrite all", "Skip", "Skip all", "Cancel operation" };
    char title[VFS_NAME_MAX + 16];
    snprintf(title, sizeof(title), "Exists: %s", vfs_basename(dst));
    int r = ui_menu(title, items, NULL, 5, 2);
    X.redraw = true;
    switch (r) {
    case 0: return true;
    case 1: X.policy = POL_OVERWRITE; return true;
    case 3: X.policy = POL_SKIP; return false;
    case 4: X.cancelled = true; return false;
    default: return false;
    }
}

static bool copy_tree(const char *src, const char *dst_dir, const char *name)
{
    if (X.cancelled)
        return false;

    char dst[VFS_PATH_MAX];
    bool is_dir;
    u64 size;

    if (!vfs_join(dst, sizeof(dst), dst_dir, name) || !vfs_stat(src, &is_dir, &size)) {
        fail("Cannot access", src);
        return false;
    }
    if (!name_fits(dst_dir, name)) {
        fail("NAND names are limited to 12 characters", name);
        return false;
    }

    if (!is_dir) {
        bool dir2;
        if (vfs_stat(dst, &dir2, NULL)) {
            if (dir2) {
                fail("A folder with that name exists", dst);
                return false;
            }
            if (!ask_overwrite(dst)) {
                X.total_bytes -= size < X.total_bytes ? size : X.total_bytes;
                if (X.files_total) X.files_total--;
                return !X.cancelled;
            }
        }
        return copy_file(src, dst);
    }

    bool dir2;
    if (vfs_stat(dst, &dir2, NULL)) {
        if (!dir2) {
            fail("A file with that name exists", dst);
            return false;
        }
    } else if (!vfs_mkdir(dst)) {
        fail("Cannot create folder", dst);
        return false;
    }

    vfs_entry *list;
    int n;
    if (!vfs_list(src, &list, &n)) {
        fail("Cannot list", src);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && !X.cancelled; i++) {
        char child[VFS_PATH_MAX];
        if (!vfs_join(child, sizeof(child), src, list[i].name))
            continue;
        ok &= copy_tree(child, dst, list[i].name);
    }
    free(list);
    return ok && !X.cancelled;
}

/* ---------------------------------------------------------------- delete */

static bool delete_tree(const char *path)
{
    if (X.cancelled)
        return false;
    bool is_dir;
    if (!vfs_stat(path, &is_dir, NULL)) {
        fail("Cannot access", path);
        return false;
    }
    snprintf(X.src, sizeof(X.src), "%s", path);

    bool ok = true;
    if (is_dir) {
        vfs_entry *list;
        int n;
        if (!vfs_list(path, &list, &n)) {
            fail("Cannot list", path);
            return false;
        }
        for (int i = 0; i < n && !X.cancelled; i++) {
            char child[VFS_PATH_MAX];
            if (!vfs_join(child, sizeof(child), path, list[i].name))
                continue;
            ok &= delete_tree(child);
        }
        free(list);
        if (X.cancelled)
            return false;
    }
    if (!vfs_remove(path, is_dir)) {
        fail("Cannot delete", path);
        return false;
    }
    X.files_done++;
    prog_tick(false);
    return ok;
}

/* ---------------------------------------------------------------- public */

static void unique_name(char *out, size_t outsz, const char *dir, const char *name)
{
    char base[VFS_NAME_MAX], ext[32] = "";
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = '\0';
    }
    char path[VFS_PATH_MAX];
    for (int i = 1; i < 1000; i++) {
        if (i == 1) snprintf(out, outsz, "%s - Copy%s", base, ext);
        else        snprintf(out, outsz, "%s - Copy (%d)%s", base, i, ext);
        if (!vfs_join(path, sizeof(path), dir, out) || !vfs_exists(path))
            return;
    }
}

/* Page 1 is the result line (with the time taken), then one page per problem. */
static void summary_pages(const char *title, const char *msg)
{
    int shown = X.fails < MAX_ERRS ? X.fails : MAX_ERRS;
    const char **pages = malloc(sizeof(char *) * (shown + 1));
    char (*more)[VFS_PATH_MAX + 96] = malloc(sizeof(*more) * (shown ? shown : 1));
    if (!pages || !more) {
        free(pages);
        free(more);
        ui_message(title, C_RED, msg);
        return;
    }
    pages[0] = msg;
    for (int i = 0; i < shown; i++) {
        snprintf(more[i], sizeof(more[0]), "Problem %d of %d%s\n\n%s", i + 1, X.fails,
                 X.fails > shown ? " (only the first ones are kept)" : "", X.errs[i]);
        pages[i + 1] = more[i];
    }
    ui_message_pages(title, C_RED, pages, shown + 1);
    free(pages);
    free(more);
}

static void summary(const char *verb, bool ok)
{
    char msg[512];
    u64 el = (now_ms() - X.start_ms) / 1000;
    if (X.cancelled)
        snprintf(msg, sizeof(msg), "Cancelled after %u file(s).", (unsigned)X.files_done);
    else if (X.fails) {
        char sz[32];
        ui_fmt_size(sz, sizeof(sz), X.done_bytes);
        snprintf(msg, sizeof(msg), "%s %u file(s)%s%s in %us.\n\n%d problem(s). Press R to browse them.",
                 verb, (unsigned)X.files_done, X.done_bytes ? ", " : "", X.done_bytes ? sz : "", (unsigned)el, X.fails);
    } else {
        char sz[32];
        ui_fmt_size(sz, sizeof(sz), X.done_bytes);
        snprintf(msg, sizeof(msg), "%s %u file(s)%s%s in %us.", verb, (unsigned)X.files_done,
                 X.done_bytes ? ", " : "", X.done_bytes ? sz : "", (unsigned)el);
    }
    if (X.disc_err_files && !X.cancelled) {
        char sz[32], extra[200];
        ui_fmt_size(sz, sizeof(sz), X.disc_err_bytes);
        snprintf(extra, sizeof(extra), "\n\n%d file(s) had unreadable disc areas (%s written as zeros). A .errors.txt next to each lists the byte ranges.",
                 X.disc_err_files, sz);
        strncat(msg, extra, sizeof(msg) - strlen(msg) - 1);
        if (X.fails) {
            ui_alert_window(true);
            summary_pages("FINISHED WITH ERRORS", msg);
            ui_alert_window(false);
            return;
        }
        ui_message("DONE - DISC READ ERRORS", C_YELLOW, msg);
    } else if (X.fails && !X.cancelled) {
        ui_alert_window(true);
        summary_pages("FINISHED WITH ERRORS", msg);
        ui_alert_window(false);
    } else if (!ok || X.cancelled || X.fails)
        ui_message(X.cancelled ? "CANCELLED" : "FINISHED WITH ERRORS", C_YELLOW, msg);
    else
        ui_message("DONE", C_GREEN, msg);
}

bool ops_paste(char **srcs, int n, bool cut, const char *dst_dir)
{
    if (vfs_is_readonly(dst_dir) || (cut && vfs_is_readonly(srcs[0]))) {
        ui_message("READ-ONLY", C_YELLOW, "That location is read-only (SEEPROM, disc, keys and archives can only be copied from).");
        return false;
    }
    if (!risk_authorize(dst_dir, cut ? "move files into" : "copy files into"))
        return false;
    if (vfs_is_card(vfs_dev_of(dst_dir)) &&
        !ui_confirm("MEMORY CARD", C_YELLOW, "Write these saves to the memory card? Saves already on the card are not replaced. Never remove the card while writing."))
        return false;
    if (cut && vfs_is_card(vfs_dev_of(srcs[0])) &&
        !ui_confirm("MEMORY CARD", C_YELLOW, "Moving takes the saves off the card once they are copied. Continue?"))
        return false;
    if (cut)
        for (int i = 0; i < n; i++)
            if (!risk_authorize(srcs[i], "move (and remove) from"))
                return false;

    /* sanity: never copy a folder into itself */
    for (int i = 0; i < n; i++) {
        bool is_dir;
        if (vfs_stat(srcs[i], &is_dir, NULL) && is_dir && vfs_is_within(dst_dir, srcs[i])) {
            ui_message("NOT ALLOWED", C_RED, "You can't paste a folder into itself or one of its own subfolders.");
            return false;
        }
    }

    u64 bytes = 0;
    u32 files = 0, dirs = 0;
    begin(cut ? "MOVING" : "COPYING", 0, 0);
    snprintf(X.src, sizeof(X.src), "Scanning...");
    prog_paint(true);
    for (int i = 0; i < n; i++)
        ops_tree_info(srcs[i], &bytes, &files, &dirs);
    X.total_bytes = bytes;
    X.files_total = files;

    char parent[VFS_PATH_MAX];
    bool all_ok = true;
    for (int i = 0; i < n && !X.cancelled; i++) {
        const char *name = vfs_basename(srcs[i]);
        char newname[VFS_NAME_MAX];
        snprintf(newname, sizeof(newname), "%s", name);

        bool same_dir = vfs_parent(srcs[i], parent, sizeof(parent)) && strcasecmp(parent, dst_dir) == 0;
        if (same_dir) {
            if (cut)
                continue;                       /* moving onto itself is a no-op */
            unique_name(newname, sizeof(newname), dst_dir, name);
        }

        char dst[VFS_PATH_MAX];
        vfs_join(dst, sizeof(dst), dst_dir, newname);

        /* same device and free destination: a plain rename is instant */
        if (cut && vfs_dev_of(srcs[i]) == vfs_dev_of(dst) && !vfs_exists(dst) &&
            name_fits(dst_dir, newname) && vfs_rename(srcs[i], dst)) {
            u64 b = 0; u32 f = 0, d = 0;
            (void)b; (void)f; (void)d;
            continue;
        }

        bool ok = copy_tree(srcs[i], dst_dir, newname);
        if (ok && cut)
            ok = delete_tree(srcs[i]);
        all_ok &= ok;
    }
    summary(cut ? "Moved" : "Copied", all_ok);
    finish();

    for (int i = 0; i < DEV_COUNT; i++)
        vfs_update_space(i);
    return all_ok && !X.cancelled;
}

bool ops_delete(char **srcs, int n)
{
    for (int i = 0; i < n; i++)
        if (vfs_is_readonly(srcs[i])) {
            ui_message("READ-ONLY", C_YELLOW, "That location is read-only (SEEPROM, disc, keys and archives can only be copied from).");
            return false;
        }
    for (int i = 0; i < n; i++)
        if (!risk_authorize(srcs[i], "delete"))
            return false;

    u64 bytes = 0;
    u32 files = 0, dirs = 0;
    for (int i = 0; i < n; i++)
        ops_tree_info(srcs[i], &bytes, &files, &dirs);

    begin("DELETING", 0, files + dirs);
    bool ok = true;
    for (int i = 0; i < n && !X.cancelled; i++)
        ok &= delete_tree(srcs[i]);
    summary("Deleted", ok);
    finish();

    for (int i = 0; i < DEV_COUNT; i++)
        vfs_update_space(i);
    return ok && !X.cancelled;
}

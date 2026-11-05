#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "usage.h"
#include "ui.h"
#include "input.h"
#include "vfs.h"
#include "ops.h"

#define USAGE_DEPTH 64

typedef struct {
    char *name;
    bool is_dir;
    u64 size;
    u32 files;
} urow;

static bool s_cancel;
static bool s_armed;   /* B was let go since the scan began: the press that opened it isn't a stop */
static bool s_by_name;

/* Size of everything below path. Nothing is kept: only the totals, so a big drive costs time, not
 * memory. B stops it. */
static void walk(const char *path, u64 *bytes, u32 *files)
{
    vfs_entry *list;
    int n;
    if (s_cancel || !vfs_list(path, &list, &n))
        return;
    for (int i = 0; i < n && !s_cancel; i++) {
        ui_busy_tick();
        if (!input_cancel_held())
            s_armed = true;
        else if (s_armed) {
            s_cancel = true;
            break;
        }
        char child[VFS_PATH_MAX];
        if (!vfs_join(child, sizeof(child), path, list[i].name))
            continue;
        if (list[i].is_dir) {
            walk(child, bytes, files);
        } else {
            *bytes += list[i].size;
            (*files)++;
        }
    }
    free(list);
}

static void rows_free(urow *r, int n)
{
    for (int i = 0; i < n; i++)
        free(r[i].name);
    free(r);
}

static int cmp_size(const void *a, const void *b)
{
    const urow *x = a, *y = b;
    if (x->is_dir != y->is_dir && x->size == y->size)
        return x->is_dir ? -1 : 1;
    if (x->size != y->size)
        return x->size > y->size ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int cmp_name(const void *a, const void *b)
{
    const urow *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void sort_rows(urow *r, int n)
{
    if (n > 1)
        qsort(r, n, sizeof(urow), s_by_name ? cmp_name : cmp_size);
}

/* The entries of dir with their recursive sizes. false if dir can't be read or B stopped the scan
 * (the rows are freed then). */
static bool scan(const char *dir, urow **out, int *count, u64 *total, u32 *files)
{
    vfs_entry *list;
    int n;
    *out = NULL;
    *count = 0;
    *total = 0;
    *files = 0;
    if (!vfs_list(dir, &list, &n))
        return false;
    urow *r = n ? calloc(n, sizeof(urow)) : NULL;
    if (n && !r) {
        free(list);
        return false;
    }

    s_cancel = false;
    s_armed = false;
    ui_busy_begin("Measuring (B stops)");
    int k = 0;
    for (int i = 0; i < n && !s_cancel; i++) {
        ui_busy_tick();
        char child[VFS_PATH_MAX];
        if (!vfs_join(child, sizeof(child), dir, list[i].name))
            continue;
        r[k].name = strdup(list[i].name);
        if (!r[k].name)
            break;
        r[k].is_dir = list[i].is_dir;
        if (list[i].is_dir) {
            walk(child, &r[k].size, &r[k].files);
        } else {
            r[k].size = list[i].size;
            r[k].files = 1;
        }
        *total += r[k].size;
        *files += r[k].files;
        k++;
    }
    ui_busy_end();
    free(list);
    input_flush();

    if (s_cancel) {
        rows_free(r, k);
        return false;
    }
    sort_rows(r, k);
    *out = r;
    *count = k;
    return true;
}

static void draw(const char *dir, const urow *r, int n, u64 total, u32 files, int cursor, int top)
{
    int cols = ui_cols(), rows = ui_rows();
    int L = rows - 6;

    ui_clear();
    ui_bg(C_BLUE);
    ui_color(C_WHITE, true);
    ui_field(0, 0, cols - 1, " DISK USAGE  sorted by %s", s_by_name ? "name" : "size");
    ui_reset();

    char shown[VFS_PATH_MAX];
    vfs_shown_path(dir, shown, sizeof(shown));
    int pl = strlen(shown);
    ui_color(C_CYAN, true);
    if (pl > cols - 3)
        ui_field(1, 1, cols - 2, "...%s", shown + pl - (cols - 6));
    else
        ui_field(1, 1, cols - 2, "%s", shown);
    ui_reset();
    ui_color(C_WHITE, false);
    ui_fill(2, 0, cols - 1, '-');
    ui_reset();

    if (!n) {
        ui_color(C_WHITE, false);
        ui_field(3, 2, cols - 3, "(empty)");
        ui_reset();
    }

    for (int i = 0; i < L && top + i < n; i++) {
        const urow *e = &r[top + i];
        int row = 3 + i;
        bool sel = top + i == cursor;
        int pm = total ? (int)(e->size * 1000 / total) : 0;
        char bar[11], sz[24];
        int fill = (pm * 10 + 500) / 1000;
        for (int b = 0; b < 10; b++)
            bar[b] = b < fill ? '#' : '.';
        bar[10] = '\0';
        ui_fmt_size(sz, sizeof(sz), e->size);

        if (sel) {
            ui_bg(C_CYAN);
            ui_color(C_BLACK, false);
        } else {
            ui_color(C_WHITE, e->is_dir);
        }
        ui_field(row, 0, cols - 1, " %10s [%s] %3d.%d%%  %s%s", sz, bar, pm / 10, pm % 10, e->name,
                 e->is_dir ? "/" : "");
        ui_reset();
    }

    char ts[24];
    ui_fmt_size(ts, sizeof(ts), total);
    ui_color(C_CYAN, true);
    ui_field(rows - 3, 1, cols - 2, "Total %s in %u file(s), %d item(s)", ts, (unsigned)files, n);
    ui_color(C_WHITE, false);
    ui_field(rows - 2, 1, cols - 2, "A Open  B Up  1 Sort  2 Delete  HOME Close");
    ui_reset();
}

void usage_browse(const char *start)
{
    char dir[VFS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", start);

    int saved[USAGE_DEPTH];
    int depth = 0;

    urow *r = NULL;
    int n = 0, cursor = 0, top = 0;
    u64 total = 0;
    u32 files = 0;

    if (!scan(dir, &r, &n, &total, &files))
        return;

    bool redraw = true;
    for (;;) {
        int L = ui_rows() - 6;
        if (cursor >= n)
            cursor = n ? n - 1 : 0;
        if (cursor < top)
            top = cursor;
        if (cursor >= top + L)
            top = cursor - L + 1;
        if (redraw) {
            draw(dir, r, n, total, files, cursor, top);
            redraw = false;
        }

        u32 p = input_pressed();
        if (p & WPAD_BUTTON_DOWN) {
            if (cursor + 1 < n) {
                cursor++;
                redraw = true;
            }
        }
        if (p & WPAD_BUTTON_UP) {
            if (cursor > 0) {
                cursor--;
                redraw = true;
            }
        }
        if (p & WPAD_BUTTON_RIGHT) {
            cursor = cursor + L - 1 < n ? cursor + L - 1 : (n ? n - 1 : 0);
            redraw = true;
        }
        if (p & WPAD_BUTTON_LEFT) {
            cursor = cursor > L - 1 ? cursor - (L - 1) : 0;
            redraw = true;
        }

        if (p & WPAD_BUTTON_HOME)
            break;

        if (p & WPAD_BUTTON_1) {
            char keep[VFS_NAME_MAX] = "";
            if (n)
                snprintf(keep, sizeof(keep), "%s", r[cursor].name);
            s_by_name = !s_by_name;
            sort_rows(r, n);
            for (int i = 0; i < n; i++)
                if (!strcmp(r[i].name, keep))
                    cursor = i;
            redraw = true;
        }

        if ((p & WPAD_BUTTON_A) && n && r[cursor].is_dir && depth < USAGE_DEPTH) {
            char child[VFS_PATH_MAX];
            if (vfs_join(child, sizeof(child), dir, r[cursor].name)) {
                urow *nr;
                int nn;
                u64 nt;
                u32 nf;
                if (scan(child, &nr, &nn, &nt, &nf)) {
                    rows_free(r, n);
                    r = nr;
                    n = nn;
                    total = nt;
                    files = nf;
                    saved[depth++] = cursor;
                    snprintf(dir, sizeof(dir), "%s", child);
                    cursor = top = 0;
                }
                redraw = true;
            }
        }

        if (p & WPAD_BUTTON_B) {
            char parent[VFS_PATH_MAX];
            if (!depth || !vfs_parent(dir, parent, sizeof(parent)))
                break;
            urow *nr;
            int nn;
            u64 nt;
            u32 nf;
            if (scan(parent, &nr, &nn, &nt, &nf)) {
                rows_free(r, n);
                r = nr;
                n = nn;
                total = nt;
                files = nf;
                snprintf(dir, sizeof(dir), "%s", parent);
                cursor = saved[--depth];
                top = 0;
            }
            redraw = true;
        }

        if ((p & WPAD_BUTTON_2) && n) {
            char child[VFS_PATH_MAX];
            if (vfs_join(child, sizeof(child), dir, r[cursor].name)) {
                char msg[VFS_NAME_MAX + 64];
                snprintf(msg, sizeof(msg), "Delete \"%s\"?\n\nThis cannot be undone.",
                         r[cursor].name);
                if (ui_confirm("DELETE", C_RED, msg)) {
                    char *one[1] = { child };
                    ops_delete(one, 1);
                    int dev = vfs_dev_of(dir);
                    if (dev >= 0)
                        vfs_update_space(dev);
                    int keep = cursor;
                    urow *nr;
                    int nn;
                    u64 nt;
                    u32 nf;
                    if (scan(dir, &nr, &nn, &nt, &nf)) {
                        rows_free(r, n);
                        r = nr;
                        n = nn;
                        total = nt;
                        files = nf;
                        cursor = keep;
                    }
                }
                redraw = true;
            }
        }
    }

    rows_free(r, n);
}

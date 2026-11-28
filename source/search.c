/*
 * Find files: every file and folder under a folder whose name matches, listed to pick one. The text matches
 * anywhere in the name, in any case; * and ? work as wildcards (then the whole name has to match).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "search.h"
#include "ui.h"
#include "vfs.h"

#define SEARCH_MAX 300
#define DEPTH_MAX 24

static char (*found)[VFS_PATH_MAX];
static int nfound;
static bool stopped;

/* * and ? wildcards, any case */
static bool match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++)
                if (match(p + 1, s)) return true;
            return false;
        }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))) return false;
    }
    return !*s;
}

static void walk(const char *dir, const char *pat, int depth)
{
    vfs_entry *e = NULL;
    int n = 0;
    if (stopped || depth > DEPTH_MAX || !vfs_list(dir, &e, &n)) return;
    for (int i = 0; i < n && !stopped; i++) {
        char p[VFS_PATH_MAX];
        if (!vfs_join(p, sizeof(p), dir, e[i].name)) continue;
        if (match(pat, e[i].name)) {
            snprintf(found[nfound++], VFS_PATH_MAX, "%s", p);
            if (nfound >= SEARCH_MAX) stopped = true;
        }
        if (e[i].is_dir) walk(p, pat, depth + 1);
        ui_busy_tick();
        if (input_cancel_held()) stopped = true;
    }
    free(e);
}

bool file_search(const char *root, char *out, size_t outsz)
{
    static char text[64];
    if (!ui_text_input("FIND FILES (part of a name; * and ? work)", text, sizeof(text) - 1) || !text[0]) return false;
    char pat[80];
    snprintf(pat, sizeof(pat), strpbrk(text, "*?") ? "%s" : "*%s*", text);
    found = malloc(sizeof(*found) * SEARCH_MAX);
    if (!found) return false;
    nfound = 0;
    stopped = false;
    ui_busy_begin("Searching (hold B to stop)");
    walk(root, pat, 0);
    ui_busy_end();

    bool picked = false;
    if (!nfound) {
        ui_message("FIND FILES", C_YELLOW, "Nothing here has a name like that.");
    } else {
        const char **items = malloc(sizeof(*items) * nfound);
        if (items) {
            size_t skip = strlen(root);
            for (int i = 0; i < nfound; i++) {
                const char *s = found[i];
                if (!strncmp(s, root, skip)) s += skip;       /* shown from the folder searched */
                while (*s == '/') s++;
                items[i] = s;
            }
            char title[96];
            snprintf(title, sizeof(title), "FOUND %d%s: %s", nfound, nfound >= SEARCH_MAX ? " (the first)" : "", text);
            int r = ui_menu_ex(title, items, NULL, nfound, 0, "A Go there   B Back");
            if (r >= 0) {
                snprintf(out, outsz, "%s", found[r]);
                picked = true;
            }
            free(items);
        }
    }
    free(found);
    found = NULL;
    return picked;
}

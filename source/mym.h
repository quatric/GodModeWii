#ifndef MYM_H
#define MYM_H

/* Builds a csm (a themed System Menu .app) from a .mym theme and the stock System Menu .app, the
 * way ThemeMii and mym-to-csm do it:
 *   [contN] file=...                     a container (.ash / LZ77 / U8) in the menu is unpacked; its
 *                                        files are addressed as <container with '.' -> '_'>_out/...
 *   [sdtaN] file=... source=...          a file from the mym replaces one in the menu (or is added
 *                                        to an unpacked container whose folder exists)
 *   [simgN] file=... source=... width= height= format=
 *                                        a PNG from the mym becomes a TPL (RGB5A3 unless format=
 *                                        says otherwise), resized when width / height say so
 * Edited containers are LZ77-compressed again. Entries whose target isn't in this menu are skipped
 * and listed, so one mym can carry files for several regions. Entries that need the user's input
 * (cimg / cdta in ThemeMii) are skipped too.
 *
 * Portable: no libogc, so it can be built and checked on a PC (see scripts/mym_host_test.sh). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- U8 */

typedef struct {
    bool dir;
    char *name;
    uint32_t a, b;              /* dirs: parent index, end index; files: offset, size (as read) */
    const uint8_t *data;        /* files: their bytes (borrowed, or `owned`) */
    uint8_t *owned;
    uint32_t len;
    uint32_t order;             /* files: where the data goes in the rebuilt archive */
} u8node;

typedef struct {
    u8node *n;
    char **paths;               /* "layout/common/cursor.ash", "" for the root */
    int count, cap;
    uint8_t reserved[16];
} u8arc;

/* Parses a U8 archive; every node and every file's data is checked against `len`. The data stays
 * borrowed from `data`. */
bool u8_parse(u8arc *u, const uint8_t *data, uint32_t len);
void u8_free(u8arc *u);
/* A file node by path ("/a/b" or "a/b"); case-insensitive unless `exact`. -1 if missing. */
int  u8_find(const u8arc *u, const char *path, bool exact);
/* Replaces a file's data (takes ownership of `data`, which must be malloc()ed). */
void u8_set(u8arc *u, int index, uint8_t *data, uint32_t len);
/* The archive written out again, Nintendo's node and data order kept. malloc()ed. */
uint8_t *u8_build(const u8arc *u, uint32_t *out_len);

/* ---------------------------------------------------------------- compression */

/* Nintendo LZ77 type 0x10, with or without a "LZ77" prefix. NULL if the data isn't that. */
uint8_t *lz77_unpack(const uint8_t *in, uint32_t len, uint32_t *out_len);
/* Compresses to "LZ77" + type 0x10 (what the Wii Menu reads in place of ASH0). */
uint8_t *lz77_pack(const uint8_t *in, uint32_t len, uint32_t *out_len);

/* ---------------------------------------------------------------- TPL */

/* An RGBA image (w * h * 4) as a single-texture TPL in `fmt` (0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565,
 * 5 RGB5A3, 6 RGBA8); the template's header (wrap and filter settings) is kept when it is a
 * single-texture TPL without a palette. */
uint8_t *tpl_encode(const uint8_t *rgba, int w, int h, int fmt, const uint8_t *tmpl, uint32_t tmpl_len,
                    uint32_t *out_len);
/* "RGB5A3" -> 5; -1 if unknown. */
int tpl_format_by_name(const char *name);

/* ---------------------------------------------------------------- mym */

/* Hands back a file of the mym (a zip): `member` is lower case, '/' separated, without a leading
 * '/'. The data must be malloc()ed; the caller frees it. */
typedef bool (*mym_fetch_fn)(void *ctx, const char *member, uint8_t **data, uint32_t *len);
/* Called while building (what is being done, step, steps); may be NULL. */
typedef void (*mym_progress_fn)(void *ctx, const char *what, int done, int total);

typedef struct {
    int applied;                /* entries put in the menu */
    int skipped;
    char report[1024];          /* "[sdta3] ... isn't in this menu" lines */
} mym_result;

/* Builds the csm from the stock menu `base` (left as it is). Returns the new archive (malloc()ed)
 * or NULL with `err` filled in. */
uint8_t *mym_build(const uint8_t *base, uint32_t base_len, mym_fetch_fn fetch, void *fctx,
                   mym_progress_fn progress, void *pctx, uint32_t *out_len, mym_result *res,
                   char *err, size_t errsz);

#endif

#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "aes.h"

/* Read-only archive formats, independent of the file system code so they can be
 * built and tested on a PC:
 *   U8 (plus IMET / IMD5 wrappers), LZ10 / LZ11 (and "LZ77"-prefixed), WAD,
 *   content.bin, data.bin (Wii save backups) and VFF.
 * An archive is a tree of files; everything is extracted by reading, nothing is
 * ever written. */

typedef struct src {
    int32_t (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);   /* bytes read, short at EOF, <0 on error */
    void (*close)(void *ctx);                                           /* optional */
    void *ctx;
    uint64_t size;
} src_t;

bool src_read_full(const src_t *s, uint64_t off, void *buf, uint32_t len);
src_t src_from_memory(uint8_t *data, uint64_t size);        /* owns data, free()d on close */

typedef struct tnode {
    char *name;
    int parent;                 /* -1 for the root */
    bool is_dir;
    bool loaded;                /* children present (lazy archives only) */
    uint64_t size;
    uint64_t a, b;              /* format specific */
    uint32_t c;
} tnode;

typedef struct tree {
    tnode *n;
    int count, cap;
} tree_t;

bool tree_init(tree_t *t);                   /* creates the root (index 0) */
void tree_free(tree_t *t);
int  tree_add(tree_t *t, int parent, const char *name, bool is_dir, uint64_t size, uint64_t a, uint64_t b, uint32_t c);
int  tree_add_path(tree_t *t, const char *path, bool is_dir, uint64_t size, uint64_t a, uint64_t b, uint32_t c);
int  tree_find(const tree_t *t, const char *rel);   /* "" or "/" = root, "a/b" = nodes; -1 if missing */
int  tree_find_child(const tree_t *t, int parent, const char *name);
void tree_clean_name(char *name);            /* no '/' '|' or control characters */

typedef struct archive archive_t;
struct archive {
    const char *kind;
    src_t base;                 /* the file the archive was opened from */
    src_t src;                  /* where entries are read from: base, or a decompressed copy */
    tree_t tree;
    int32_t (*read)(archive_t *a, const tnode *node, uint64_t off, void *buf, uint32_t len);
    void (*destroy)(archive_t *a);
    bool (*expand)(archive_t *a, int node);     /* optional: fills in the children of a folder on demand */
    void *priv;
};

/* The PRNG key that content.bin contents are encrypted with is specific to the console. */
typedef bool (*archive_key_fn)(uint8_t key[16]);
void archive_set_prng_key_provider(archive_key_fn fn);

/* Candidate AES keys for WFS volumes (Wii U USB / MLC): index 0, 1, ... until false. */
typedef bool (*archive_wfs_key_fn)(int index, uint8_t key[16]);
void archive_set_wfs_key_provider(archive_wfs_key_fn fn);

extern const char *g_archive_err;           /* last failure, for the user */

/* Looks at the header (and the name's extension) and says whether this is something we open. */
bool archive_probe(const src_t *file, const char *name);
/* On success the archive owns *file (it is closed with the archive, or right away if the
 * contents were decompressed into memory); on failure the caller still owns it. */
archive_t *archive_open(const src_t *file, const char *name);
void archive_close(archive_t *a);

/* Lazy archives: make sure the children of a folder are in the tree / look a path up (expanding on the way). */
bool archive_expand(archive_t *a, int node);
int  archive_find(archive_t *a, const char *rel);

/* Shared helpers for the format files. */
#define ARC_MAX_DECOMPRESSED (24u * 1024 * 1024)
uint32_t be32(const uint8_t *p);
uint64_t be64(const uint8_t *p);
uint16_t be16(const uint8_t *p);
uint32_t le32(const uint8_t *p);
bool arc_name_has_ext(const char *name, const char *ext);
archive_t *archive_alloc(const char *kind);
archive_t *archive_open_memory(uint8_t *data, uint64_t size, const char *name);   /* consumes data; NULL (and frees it) if not an archive */
archive_t *archive_open_single(uint8_t *data, uint64_t size, const char *fname);  /* one-file archive over a decompressed buffer */
int32_t cbc_stream_read(const src_t *src, const aes128_t *ctx, const uint8_t iv0[16], uint64_t enc_off,
                        uint64_t plain_size, uint64_t off, void *buf, uint32_t len);

/* formats that live in their own files */
bool arc_vff_probe(const src_t *file, const char *name);
archive_t *arc_vff_open(const src_t *file, const char *name);
bool arc_wad_probe(const src_t *file, const char *name);
archive_t *arc_wad_open(const src_t *file, const char *name);
bool arc_contentbin_probe(const src_t *file, const char *name);
archive_t *arc_contentbin_open(const src_t *file, const char *name);
bool arc_databin_probe(const src_t *file, const char *name);
archive_t *arc_databin_open(const src_t *file, const char *name);

bool arc_wfs_probe(const src_t *file, const char *name);
archive_t *arc_wfs_open(const src_t *file, const char *name);
bool arc_wfs_key_at(int index, uint8_t key[16]);        /* the provider, used by arc_wfs.c */

/* keys shared by wad / content.bin / data.bin */
extern const uint8_t ARC_SD_KEY[16], ARC_SD_IV[16];
bool arc_prng_key(uint8_t key[16]);
bool arc_common_key(int index, uint8_t key[16]);

#endif

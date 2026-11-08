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
    /* bytes read, short at EOF, <0 on error */
    int32_t (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    void (*close)(void *ctx);                                           /* optional */
    void *ctx;
    uint64_t size;
} src_t;

/* Called between the pieces of src_read_full, for the loading box (set by arcfs). */
extern void (*busy_hook)(void);
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
int  tree_add(tree_t *t, int parent, const char *name, bool is_dir, uint64_t size, uint64_t a,
              uint64_t b, uint32_t c);
int  tree_add_path(tree_t *t, const char *path, bool is_dir, uint64_t size, uint64_t a, uint64_t b,
                   uint32_t c);
/* "" or "/" = root, "a/b" = nodes; -1 if missing */
int  tree_find(const tree_t *t, const char *rel);
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
    /* optional: fills in the children of a folder on demand */
    bool (*expand)(archive_t *a, int node);
    void *priv;
};

typedef bool (*archive_key_fn)(uint8_t key[16]);
/* The PRNG key that content.bin contents are encrypted with is specific to the console: the key of the
 * console with this ID (from its "Bk" header; 0 = any), this one's or out of a BootMii key dump. */
typedef bool (*archive_prng_fn)(uint32_t console_id, uint8_t key[16]);
void archive_set_prng_key_provider(archive_prng_fn fn);

/* Candidate AES keys for WFS volumes (Wii U USB / MLC): index 0, 1, ... until false. */
typedef bool (*archive_wfs_key_fn)(int index, uint8_t key[16]);
void archive_set_wfs_key_provider(archive_wfs_key_fn fn);
/* A file next to the archive being opened (only while archive_open runs), for formats that come
 * as a pair (INFO.DAT + GAME.DAT, .arc + .dir). The archive owns the source and closes it. */
typedef bool (*archive_sibling_fn)(const char *name, src_t *out);
void archive_set_sibling_provider(archive_sibling_fn fn);
bool archive_open_sibling(const char *name, src_t *out);

extern const char *g_archive_err;           /* last failure, for the user */

/* Looks at the header (and the name's extension) and says whether this is something we open. */
bool archive_probe(const src_t *file, const char *name);
/* On success the archive owns *file (it is closed with the archive, or right away if the
 * contents were decompressed into memory); on failure the caller still owns it. */
archive_t *archive_open(const src_t *file, const char *name);
void archive_close(archive_t *a);

/* Lazy archives: make sure the children of a folder are in the tree / look a path up
 * (expanding on the way). */
bool archive_expand(archive_t *a, int node);
int  archive_find(archive_t *a, const char *rel);

/* Shared helpers for the format files. */
#define ARC_MAX_DECOMPRESSED (24u * 1024 * 1024)
uint32_t be32(const uint8_t *p);
uint64_t be64(const uint8_t *p);
uint16_t be16(const uint8_t *p);
uint32_t le32(const uint8_t *p);
bool arc_name_has_ext(const char *name, const char *ext);
/* Compressed single files (.lz, .lz77, .huff, .stpl, .z, .gz ...): the name without that extension, and the data
 * decompressed into a 32-byte aligned buffer (NULL if it isn't compressed that way). */
bool arc_lz_name(const char *name, char *out, size_t outsz);
/* Codecs shared with the compression wrapper: Good-Feel GFCP modes 1 (byte pair) and 2 / 3 (headerless
 * LZ10), and Arika ALZ1 run to the end of its input. */
bool arc_bpe_decode(uint8_t *dst, uint32_t dsz, const uint8_t *src, uint32_t ssz);
bool arc_lz10_raw(uint8_t *dst, uint32_t dsz, const uint8_t *src, uint32_t ssz);
uint8_t *arc_alz1_unpack(const uint8_t *src, uint32_t n, uint32_t *out_len);
/* Camelot GX texture bank (TPL magic, but its own layout); arc_camelot_bank_tpl rewrites one as a TPL. */
bool arc_camelot_bank_valid(const uint8_t *b, uint32_t n);
uint8_t *arc_camelot_unpack(const uint8_t *src, uint32_t n, uint32_t *out_len);   /* Camelot LZ, as is */
uint8_t *arc_camelot_bank_tpl(const uint8_t *b, uint32_t n, uint32_t *out_len);
uint8_t *arc_lz_unwrap(const char *name, const uint8_t *buf, uint32_t len, uint32_t *out_len);   /* by name or contents: Camelot / zlib / CX */
archive_t *archive_alloc(const char *kind);
/* consumes data; NULL (and frees it) if not an archive */
archive_t *archive_open_memory(uint8_t *data, uint64_t size, const char *name);
/* one-file archive over a decompressed buffer */
archive_t *archive_open_single(uint8_t *data, uint64_t size, const char *fname);
int32_t cbc_stream_read(const src_t *src, const aes128_t *ctx, const uint8_t iv0[16],
                        uint64_t enc_off, uint64_t plain_size, uint64_t off, void *buf,
                        uint32_t len);

/* formats that live in their own files */
bool arc_vff_probe(const src_t *file, const char *name);
archive_t *arc_vff_open(const src_t *file, const char *name);
bool arc_yaz0_probe(const src_t *file, const char *name);      /* Yaz0 and Yay0 */
archive_t *arc_yaz0_open(const src_t *file, const char *name);
bool arc_rarc_probe(const src_t *file, const char *name);
archive_t *arc_rarc_open(const src_t *file, const char *name);
bool arc_brres_probe(const src_t *file, const char *name);
archive_t *arc_brres_open(const src_t *file, const char *name);
bool arc_ash0_probe(const src_t *file, const char *name);
archive_t *arc_ash0_open(const src_t *file, const char *name);
bool arc_brsar_probe(const src_t *file, const char *name);
archive_t *arc_brsar_open(const src_t *file, const char *name);
bool arc_disc_probe(const src_t *file, const char *name);       /* ISO / GCM / WBFS / CISO */
archive_t *arc_disc_open(const src_t *file, const char *name);
bool arc_ccf_probe(const src_t *file, const char *name);
archive_t *arc_ccf_open(const src_t *file, const char *name);
bool arc_pac_probe(const src_t *file, const char *name);
archive_t *arc_pac_open(const src_t *file, const char *name);
/* Retro Studios .pak (Metroid Prime, DKC Returns) */
bool arc_retro_probe(const src_t *file, const char *name);
archive_t *arc_retro_open(const src_t *file, const char *name);
/* arc_more.c: VCRA, SSZL, RFL_Res, GPAK, GPKG, BLPKG, ... */
bool arc_more_probe(const src_t *file, const char *name);
archive_t *arc_more_open(const src_t *file, const char *name);
bool arc_lzop_probe(const src_t *file, const char *name);      /* lzop .lzo */
archive_t *arc_lzop_open(const src_t *file, const char *name);
long lzo1x_decode(const uint8_t *src, size_t n, uint8_t *out, size_t cap);
bool arc_zip7zrar_probe(const src_t *file, const char *name);  /* zip, 7z, rar (1.5 - 5) */
archive_t *arc_zip7zrar_open(const src_t *file, const char *name);
bool arc_gfa_probe(const src_t *file, const char *name);
archive_t *arc_gfa_open(const src_t *file, const char *name);
bool arc_wad_probe(const src_t *file, const char *name);
archive_t *arc_wad_open(const src_t *file, const char *name);
bool arc_contentbin_probe(const src_t *file, const char *name);
archive_t *arc_contentbin_open(const src_t *file, const char *name);
bool arc_databin_probe(const src_t *file, const char *name);
archive_t *arc_databin_open(const src_t *file, const char *name);

bool arc_wfs_probe(const src_t *file, const char *name);
archive_t *arc_wfs_open(const src_t *file, const char *name);
bool arc_wfs_key_at(int index, uint8_t key[16]);        /* the provider, used by arc_wfs.c */

/* disk file systems and partition tables, read-only (arc_ext.c, arc_hfs.c, arc_xfs.c, arc_apfs.c, arc_zfs.c, arc_disk.c) */
bool arc_ext_probe(const src_t *file, const char *name);       /* ext2 / ext3 / ext4 */
archive_t *arc_ext_open(const src_t *file, const char *name);
bool arc_hfs_probe(const src_t *file, const char *name);       /* HFS+ / HFSX, "Mac OS Extended (Journaled)" */
archive_t *arc_hfs_open(const src_t *file, const char *name);
bool arc_xfs_probe(const src_t *file, const char *name);       /* XFS v4 / v5 */
archive_t *arc_xfs_open(const src_t *file, const char *name);
bool arc_apfs_probe(const src_t *file, const char *name);      /* APFS container */
archive_t *arc_apfs_open(const src_t *file, const char *name);
bool arc_zfs_probe(const src_t *file, const char *name);       /* OpenZFS / ZFS pool, one disk or mirror */
archive_t *arc_zfs_open(const src_t *file, const char *name);
bool arc_disk_probe(const src_t *file, const char *name);      /* MBR / GPT / Apple partition map */
archive_t *arc_disk_open(const src_t *file, const char *name);
bool arc_hsd_probe(const src_t *file, const char *name);       /* HAL sysdolphin .dat */
archive_t *arc_hsd_open(const src_t *file, const char *name);
bool arc_fjg_probe(const src_t *file, const char *name);       /* Paon FJG: HSD archives back to back */
archive_t *arc_fjg_open(const src_t *file, const char *name);
/* The first outn bytes a bare CX stream (LZ10 / LZ11 ...) at h (n bytes) decodes to. */
bool arc_cx_peek(const uint8_t *h, uint32_t n, uint8_t *out, uint32_t outn);
bool arc_rst_probe(const src_t *file, const char *name);       /* Monster Games RST (+ .toc), QuickLZ */
archive_t *arc_rst_open(const src_t *file, const char *name);
bool arc_pdf_probe(const src_t *file, const char *name);       /* PDF: page text and embedded images */
archive_t *arc_pdf_open(const src_t *file, const char *name);
bool arc_epub_probe(const src_t *file, const char *name);      /* EPUB: chapter text and pictures */
archive_t *arc_epub_open(const src_t *file, const char *name);
bool arc_nand_probe(const src_t *file, const char *name);      /* BootMii nand.bin */
archive_t *arc_nand_open(const src_t *file, const char *name);
void archive_set_nand_key_provider(archive_key_fn fn);        /* the console's own NAND key, when running on it */

/* keys shared by wad / content.bin / data.bin */
extern const uint8_t ARC_SD_KEY[16], ARC_SD_IV[16];
bool arc_prng_key(uint32_t console_id, uint8_t key[16]);
bool arc_common_key(int index, uint8_t key[16]);

#endif

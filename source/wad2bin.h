#ifndef WAD2BIN_H
#define WAD2BIN_H

#include <stdbool.h>
#include <stdint.h>

#include "archive.h"

/* WAD -> SD card backup package, as DarkMatterCore's wad2bin makes them: a channel becomes
 * private/wii/title/<ABCD>/content.bin (runs from the SD card menu, System Menu 4.0+), a DLC becomes
 * private/wii/data/<its own abcd>/<index>.bin. Signed and encrypted with this console's keys, so the
 * files only work on the console they were made on. A "<title id>_bogus.wad" (ticket and TMD, no
 * contents) is written to wad/ beside them. */

typedef struct {
    uint32_t console_id;
    uint8_t ng_priv[30];                /* the OTP's ECC private key */
    uint8_t prng_key[16];
    uint8_t device_cert[0x180];         /* ES_GetDeviceCert */
} w2b_keys_t;

typedef struct {
    bool dlc_null_key;                  /* DLC only: encrypt with a null key (older games via the Disc Channel) */
    uint64_t parent_tid;                /* DLC only: the game it belongs to; 0 = the DLC's own ID in upper case */
} w2b_opts_t;

/* Where the files go. mkdirs makes a folder and its parents; open creates / truncates a file;
 * close returns false when the data didn't all get written. progress may be NULL; false cancels. */
typedef struct {
    void *ctx;
    bool (*mkdirs)(void *ctx, const char *dir);
    void *(*open)(void *ctx, const char *path);
    bool (*write)(void *ctx, void *fh, const void *p, uint32_t n);
    bool (*close)(void *ctx, void *fh);
    void (*remove)(void *ctx, const char *path);
    bool (*progress)(void *ctx, const char *what, uint64_t done, uint64_t total);
} w2b_io_t;

typedef struct {
    uint64_t tid;
    bool dlc;
    int files;                          /* .bin files written */
    char path[320];                     /* content.bin, or the DLC's folder */
    char bogus[320];                    /* the ticket + TMD WAD ("" if it couldn't be written) */
    char err[160];
} w2b_result_t;

/* root is the card's root with its slash ("sd:/"). 0 on success; otherwise r->err says why, and
 * nothing half-written is left behind. */
int w2b_convert(const src_t *wad, const char *root, const w2b_keys_t *keys, const w2b_opts_t *opt,
                const w2b_io_t *io, w2b_result_t *r);

/* Signs `digest` the way the System Menu signs its SD card files: with an AP key (its certificate, issued by
 * this console's device certificate and signed with the NG key, goes to `ap`). r and s are 30 bytes each.
 * False when the NG key doesn't match the device certificate. */
bool w2b_ap_sign(const w2b_keys_t *k, const uint8_t digest[20], uint8_t r[30], uint8_t s[30], uint8_t ap[0x180]);

/* The title ID of an installable WAD; false when it isn't one. */
bool w2b_peek(const src_t *wad, uint64_t *tid);

/* "ABCD" of a title ID (non-printable bytes as '.'), and the game a DLC belongs to by default. */
void w2b_tid_ascii(uint64_t tid, char out[5]);
uint64_t w2b_parent_tid(uint64_t dlc_tid);

#endif

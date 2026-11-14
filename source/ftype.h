#ifndef FTYPE_H
#define FTYPE_H

/* What a file is, from the magic bytes at its start (after nintoolbox's file-type detection).
 * The name only decides for the few formats that have no magic (DSP / BTSND audio, BTI and TXTR
 * textures, headerless MP3, LZ-wrapped "x.brres.lz"), so a renamed or extensionless file opens in
 * the right viewer and a file with a misleading extension doesn't. */

#include <stdbool.h>
#include <gccore.h>

typedef enum {
    FT_NONE,
    FT_STREAM,          /* BRSTM / BCSTM / BFSTM, BRWAV / BCWAV / BFWAV, BNS, AST, DSP, BTSND, SFX, HPS */
    FT_AUDIO,           /* WAV, AIFF, MP3, FLAC, Ogg Vorbis */
    FT_MIDI,            /* standard MIDI, RIFF RMID */
    FT_SEQ,             /* RSEQ in a BRSAR */
    FT_VIDEO,           /* Mobiclip (MOC5), THP, Flipnote PPM / KWZ */
    /* TPL, TEX0, PLT0, BRFNT / BRFNA, BREFT, PNG, JPEG, WebP, AJPG, banners, BTI, TXTR, DKZ,
     * Monster Games TEX / ART / IMG / TM0 */
    FT_IMAGE,
    FT_MODEL,           /* BRRES, MDL0, J3D BMD / BDL */
    FT_BMG,
    FT_BREFF,
    FT_MSBT,
    FT_LAYOUT,          /* BRLYT */
    FT_ANIM,            /* BRLAN */
    FT_KMP,
    FT_MII,             /* a Mii (.miigx / .mii / .rcd / .rsd), a Mii database (RFL_DB.dat) */
    FT_WAD,
    FT_PROGRAM,         /* DOL / PowerPC ELF */
} ftype_t;

/* From the first bytes of a file (n of them, 0x40 is enough), its size and its name. */
ftype_t ftype_sniff(const u8 *head, u32 n, u64 size, const char *name);
/* Reads the start of the file at path. FT_NONE for directories and unreadable files. */
ftype_t ftype_detect(const char *path);

/* A type one of the viewers or players opens. */
static inline bool ftype_is_media(ftype_t t) { return t >= FT_STREAM && t <= FT_MII; }

#endif

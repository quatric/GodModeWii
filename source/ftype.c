/*
 * File type detection from magic bytes. See ftype.h.
 */
#include <string.h>

#include "ftype.h"
#include "vfs.h"
#include "archive.h"
#include "excite.h"
#include "media.h"

#define M(s) (n >= sizeof(s) - 1 && !memcmp(h, s, sizeof(s) - 1))

static bool ext_any(const char *name, const char *const *ext)
{
    for (int i = 0; ext[i]; i++)
        if (arc_name_has_ext(name, ext[i])) return true;
    return false;
}

/* The formats that carry no magic of their own. */
static ftype_t by_name(const char *name, const u8 *h, u32 n, u64 size)
{
    /* Monster Games: .sfx audio checks its header against the size; .tex / .art go to the image viewer,
     * .tm0 when its header at 0x80 checks out, .img (also a disk image name) only when it is shaped like
     * one (the header, or 2^k pixel bytes and a 128-byte tail) */
    if (arc_name_has_ext(name, ".sfx") && audio_sfx_header(h, n, size)) return FT_STREAM;
    if (arc_name_has_ext(name, ".tex") || arc_name_has_ext(name, ".art")) return FT_IMAGE;
    if (arc_name_has_ext(name, ".tm0") && n >= 0x86 && excite_head_ok(h + 0x80, n - 0x80)) return FT_IMAGE;
    if (arc_name_has_ext(name, ".img") && size > 128 && size <= (4u << 20) + 128 &&
        (excite_head_ok(h, n) || !((size - 128) & (size - 129))))
        return FT_IMAGE;
    static const char *const stream[] = { ".dsp", ".btsnd", NULL };
    static const char *const audio[] = { ".mp3", ".mp2", NULL };
    static const char *const image[] = { ".bti", ".txtr", NULL };
    if (ext_any(name, stream)) return FT_STREAM;
    if (ext_any(name, audio)) return FT_AUDIO;
    if (ext_any(name, image)) return FT_IMAGE;
    return FT_NONE;
}

/* "x.brres.lz": what is inside goes by the inner name (the viewers unwrap it when loading). */
static ftype_t lz_inner(const char *inner)
{
    static const char *const model[] = { ".brres", ".mdl0", ".bmd", ".bdl", ".hsf", NULL };
    static const char *const image[] = { ".tpl",   ".bti",   ".tex0",  ".brtex",
                                         ".brfnt", ".brfna", ".breft", NULL };
    if (ext_any(inner, model)) return FT_MODEL;
    if (ext_any(inner, image)) return FT_IMAGE;
    return FT_NONE;
}

/* A DOL: an entry point in MEM1 and section offsets / sizes that fit the file. */
static bool dol(const u8 *h, u32 n, u64 size)
{
    if (n < 0x100 || size < 0x100 || size > 0x2000000) return false;
    u32 entry = be32(h + 0xE0);
    if (entry < 0x80000000u || entry >= 0x81800000u) return false;
    u32 any = 0;
    for (int i = 0; i < 18; i++) {
        u32 off = be32(h + i * 4), sz = be32(h + 0x90 + i * 4), addr = be32(h + 0x48 + i * 4);
        if (!sz) continue;
        if (off < 0x100 || off > size || sz > size - off) return false;
        if (addr < 0x80000000u || addr >= 0x81800000u) return false;
        any++;
    }
    return any > 0;
}

ftype_t ftype_sniff(const u8 *h, u32 n, u64 size, const char *name)
{
    char inner[256];
    if (name && arc_lz_name(name, inner, sizeof(inner))) {
        ftype_t t = lz_inner(inner);
        if (t != FT_NONE) return t;
    }
    if (n >= 4) {
        /* NW4R / NW4C / NW4F audio */
        if (M("RSTM") || M("CSTM") || M("FSTM") || M("RWAV") || M("CWAV") || M("FWAV") || M("BNS "))
            return FT_STREAM;
        if (M("IMD5") && n >= 0x24 && !memcmp(h + 0x20, "BNS ", 4)) return FT_STREAM;
        /* AST: PCM16 / ADPCM */
        if (M("STRM") && n >= 0x40 && be16(h + 8) <= 1) return FT_STREAM;
        if (M("RSEQ")) return FT_SEQ;
        if (M("MThd")) return FT_MIDI;
        if (n >= 12 && M("RIFF") && !memcmp(h + 8, "RMID", 4)) return FT_MIDI;
        if (n >= 12 && (M("RIFF") || M("RF64") || M("riff")) && !memcmp(h + 8, "WAVE", 4))
            return FT_AUDIO;
        if (n >= 12 && M("FORM") && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4)))
            return FT_AUDIO;
        if (M("fLaC") || M("OggS") || M("ID3")) return FT_AUDIO;

        if (M("MOC5") || M("THP\0") || M("PARA") || M("KFH")) return FT_VIDEO;

        if (be32(h) == 0x0020AF30u) return FT_IMAGE;                                   /* TPL */
        if (M("PLT0") || M("AJPG")) return FT_IMAGE;
        if (M("TEX0") || M("RFNT") || M("RFNU") || M("RFNA") || M("REFT") || M("DKZF") ||
            M("\x89PNG"))
            return FT_IMAGE;
        if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return FT_IMAGE;            /* JPEG */
        if (n >= 12 && M("RIFF") && !memcmp(h + 8, "WEBP", 4)) return FT_IMAGE;
        /* GameCube / Wii save banners */
        if (M("BNR1") || M("BNR2") || M("WIBN")) return FT_IMAGE;

        if (M("bres") || M("MDL0")) return FT_MODEL;
        if (M("HSFV037")) return FT_MODEL;                                            /* Mario Party HSF */
        if (n >= 8 && (M("J3D2") || M("J3D1")) &&
            (!memcmp(h + 4, "bmd", 3) || !memcmp(h + 4, "bdl", 3))) return FT_MODEL;

        if (M("MESGbmg1")) return FT_BMG;
        if (M("REFF")) return FT_BREFF;
        if (M("MsgStdBn")) return FT_MSBT;
        if (M("RLYT")) return FT_LAYOUT;
        if (M("RLAN")) return FT_ANIM;
        if (M("RKMD")) return FT_KMP;

        /* WAD: header size 0x20, then "Is" (installable), "ib" (boot2) or "Bk" (backup) */
        if (n >= 8 && be32(h) == 0x20 &&
            (!memcmp(h + 4, "Is", 2) || !memcmp(h + 4, "ib", 2) || !memcmp(h + 4, "Bk", 2)))
            return FT_WAD;
        /* a channel's NAND contents (0000000N.app) are often its DOL / ELF: browse them, never
         * launch them */
        bool app = name && arc_name_has_ext(name, ".app");
        if (!app && n >= 0x14 && M("\x7F" "ELF") && h[4] == 1 && h[5] == 2 &&
            be16(h + 0x12) == 20)
            return FT_PROGRAM;
        if (!app && dol(h, n, size)) return FT_PROGRAM;
    }
    return name ? by_name(name, h, n, size) : FT_NONE;
}

ftype_t ftype_detect(const char *path)
{
    vfs_file f;
    if (!vfs_open_read(&f, path)) return FT_NONE;
    static u8 head[0x100] __attribute__((aligned(32)));
    u32 got = 0;
    while (got < sizeof(head) && got < f.size) {
        s32 r = vfs_read(&f, head + got, sizeof(head) - got);
        if (r <= 0) break;
        got += r;
    }
    u64 size = f.size;
    vfs_close(&f);
    return ftype_sniff(head, got, size, vfs_basename(path));
}

# Credits

**GodModeWii is by quatric & cmyksoda.** It also stands on a lot of other people's work. Thank you!

## Code included

* **[TinyLoad](https://wiibrew.org/wiki/TinyLoad)** 0.2 by Hector Martin (marcan), with code by Segher Boessenkool, Andre Heider
  and Nuke (GPL-2.0) — the game disc loader, built into `res/tinyload.elf` from `tinyload/` (`tinyload/build.sh`) and started
  through the launcher.
* **[MoonShell](https://www.gamebrew.org/wiki/Moonshell)** 2.10 by Moonlight (GPL-2.0 or later) — **inspired us a lot**: the clock and calendar, the alarm and the
  calculator follow its own, and the interface sound effects are its (`res/sfx.bin`, made by `scripts/make_sfx.py` from its
  `misctools/sndeff` folder).
* **TiVo**'s interface sounds — an easter egg (`res/tivo.bin`, made by `scripts/make_tivo.py`); the last credits page has a hint, and the code works on the drive list too.
* **[GlowWii](https://github.com/larsenv/GlowWii)** (larsenv) — the disc slot light driver and Nintendo's light patterns
  (`source/wiilight.c`, `source/wiilight_patterns.h`).
* **[PictoCard](https://github.com/larsenv/PictoCard-Client)** (larsenv) — the doodle pad's palette, brush sizes and tools
  (`source/draw.c`).
* **[YAWM ModMii Edition](https://github.com/modmii/YAWM-ModMii-Edition)** by OverjoY, blackb0x, scooby74029, Peter0x44 and XFlak
  (from Some YAWMM Mod, YAWMM and WAD Manager by Waninkoko; GPL-2.0) — the WAD installer (`source/wadinstall.c`: the ES install
  sequence, the vWii ticket fix-up and every brick check) and the `.dol` / `.elf` launcher (`source/launch.c` and its app booter in
  `appboot/`, with Segher Boessenkool's `crt0.s` / `link.ld`).
* **[wii-mobiclip-player](https://github.com/quatric/wii-mobiclip-player)** (quatric; MIT) — the Mobiclip, THP and Flipnote
  players in `source/mobiclip/` (see its `LICENSE` and `CREDITS_MOBICLIP.md`). Its Mobiclip decoder is ported from FFmpeg's
  (Florian Nouwt, Adib Surani, Paul B Mahol; LGPL 2.1+), with thanks to PlayMobic, MobiclipDecoder and Gericom's x264 fork (Gericom)
  and WiiLink24's FFmpeg fork. THP frames are decoded with NanoJPEG (Martin J. Fiedler) and Vorbis audio with Tremor (portlibs).
* **[FSToolBox-Mod](https://github.com/WiiDatabase/FSToolBox-Mod)** by the WiiDatabase.de team, based on **FSToolBox by nickasa**
  and the **AHBPROT mod by Lupo96** — the approach for NAND access on the Wii and the Wii U's vWii mode
  (staying on the loader's IOS, ISFS permission patch). Parts of its code are included in `source/runtimeiospatch.c/.h`.
* **libruntimeiospatch** by Nano (Christopher Bratusek), with code by Joseph Jordan, damysteryman, DarkMatterCore, megazig
  and FIX94, as shipped in FSToolBox-Mod. GodModeWii adds two tiny single-patch entry points (`IosPatch_ISFS`, `IosPatch_DIReadLimit`).
* **[TinySoundFont](https://github.com/schellingb/TinySoundFont)** by Bernhard Schelling (MIT, in `source/tsf`) — `tsf.h` / `tml.h`
  play MIDI files with SoundFonts (patched here to read the little-endian SoundFont on the big-endian Wii). The built-in General
  MIDI set is **Gervill**'s emergency soundbank (OpenJDK; GPL-2.0 with the Classpath exception), zlib-packed in `res/gmsf2.bin`.
* **[dr_libs](https://github.com/mackron/dr_libs)** by David Reid (public domain / MIT-0, in `source/drlibs`) — `dr_wav`, `dr_mp3` and `dr_flac` decode WAV / AIFF, MP3 and FLAC.
* **[Zstandard](https://github.com/facebook/zstd)** by Meta Platforms (BSD, in `source/zstd`) — the single-file decoder, as vendored by RiftWii, for RVZ.
* **[unarr](https://github.com/selmf/unarr)** (selmf and contributors, LGPL-3.0, in `source/unarr`, with the LZMA SDK by Igor Pavlov inside) — the zip, 7z and RAR (up to 4.x) readers of the FTP installer and of archive browsing, and the LZMA / LZMA2 decoder used for RVZ; RAR5 is read by `source/rar5.c`, written from the public format description.
* **[csm-installer](https://github.com/Naim2000/csm-installer)** by Naim2000 (GPL-2.0) — the Wii Menu theme
  installer's checks and install sequence (`source/theme.c`: identifying the System Menu and its archive, Priiloader detection,
  the theme checks, the vWii 4:3 database patch after DarkMatterCore's ww-43db-patcher, writing through `/tmp`, downloading the original archive
  from NUS).
* **mym-to-csm** by cmyksoda — `source/mym.c` is a C port of it (mym.ini handling, U8 rebuild, TPL encoder, LZ77 compressor).
* **GlowWii** — the controller input layer (Wii Remote / Classic / GameCube pad / USB keyboard mapping) comes from its `main.c`.
* **libwiidrc** and **libwupc** (FIX94) and **libsicksaxis** (xerpi) — Wii U GamePad, Wii U Pro Controller and PS3 pad support; the build script and libwupc compatibility patch use GlowWii's approach.
* **Priiloader** (Dacoslove's `Input.cpp` and the vWii detection) — USB keyboard handling and the vWii check, via GlowWii.
* **[libntfs](https://github.com/extremscorner/libntfs)** by Extrems and contributors, on **NTFS-3G** (GPL-2.0): NTFS drives,
  built by `scripts/build_libntfs.sh`.
* **[lwext4](https://github.com/gkostka/lwext4)** by Grzegorz Kostka (GPL-2.0 / BSD; Extrems' big-endian fork) with Extrems'
  libdvm ext4 driver (ZPL-2.1): ext2/3/4 drives, ported and made read-only by `scripts/build_libext4.sh`.
* **libpng**, **libjpeg-turbo**, **libwebp**, **zlib**, **bzip2**, **Tremor** and **libogg**, as packaged in devkitPro's portlibs — PNG, JPEG and WebP viewing, bzip2 RVZ / WIA images, and Vorbis sound in Mobiclip videos and `.ogg` files.
* **[devkitPro](https://devkitpro.org/)** — devkitPPC, **libogc** (including libdi, wiiuse, ISFS) and **libfat**.

## Formats and references

* **[nintoolbox](https://github.com/quatric/nintoolbox)** and **[mobipeg](https://github.com/quatric/mobipeg)** (quatric) — the layouts of
  every first-party format the viewers read: BRSTM / BCSTM / BFSTM, BRWAV / BCWAV / BFWAV, BNS, DSP, AST, BTSND, TPL, BTI, TEX0,
  BRFNT, BREFT, BRRES / MDL0, J3D BMD / BDL, BMG, MSBT, BRLYT / BRLAN, KMP, BREFF, BRSAR / RWAR, BNR, WIBN, Retro TXTR,
  Yaz0 / Yay0, ASH0, LZ10 / LZ11, Huffman, run length, LZH8, GFA, CCF, Brawl PAC, RARC, and GameCube / Wii disc images
  (ISO, WBFS, CISO), and file-type detection.
* **[HSDLib](https://github.com/Ploaj/HSDLib)** by Ploaj (MIT) — the HSD `.dat` container and relocation layout, via nintoolbox's `lib-hsd.c`; and **[Metanoia](https://github.com/Ploaj/Metanoia)**'s HSF reader, via nintoolbox's `lib-hsf.c` (Mario Party HSFV037 models).
* **Mario Party `.bin` archives** — the member table and the LZSS / slide / run-length / zlib decoders follow nintoolbox's `lib-mpbin.c` (GPL-2.0+).
* **Camelot STPL and texture banks** — the codec and bank layout come from nintoolbox's `lib-camelot.c` and `lib-camtexbank.c`. nintoolbox only claims
  a Camelot file by its name, because scanning for the bank signature matched inside other archives; GodModeWii also opens an
  extensionless one, but only when the first bytes it unpacks to are the start of a valid bank, and scans for banks only in `.sbn`
  modules. The `LZ77` tag handling follows nintoolbox's `lib-wii-banner.c`.
* **Monster Games** (Excite Truck, Excitebots) — the RST / TOC layouts (`lib-rst.c`), the `.sfx` header (`lib-sfx.c`) and the
  `.tex` / `.art` / `.tm0` texture recovery and the `.mod` display-list models (`lib-excite.c`, itself ported from the **ExciteExtract**
  research tool), all via nintoolbox. GodModeWii differs in two places: Excite Truck's 0x44-byte TOC offsets count from the archive's
  data offset (read from the file's start, every member came out 0x80 bytes early), and an NDL2 model's positions and texcoords are
  found through its header's array pointers (packed or padded per file).
* **[QuickLZ](http://www.quicklz.com/)** by Lasse Mikkel Reinhold (GPL) — `source/qlz.c` decodes the 1.4.0 (level 3) and 1.20
  streams the way its reference decoders do, with byte-order-independent reads; the version test follows nintoolbox's `lib-quicklz.c`.
* **AJPG / ODH** — `source/ajpg/odh_core.c` is the ODH codec core from the mobipeg FFmpeg fork (LGPL-2.1+, as packaged in
  nintoolbox), which reads its header byte by byte so it decodes the same on the big-endian Wii.
* **Miis** — drawn by **[MiiKit](https://github.com/cmyksoda/MiiKit)** (public domain, CC0, in `source/miikit`), which
  started as the hbc-reborn developers' sdk/mii, with cmyksoda's NAND loader. The Mii record fields shown come from WiiBrew's
  Mii data documentation.
* **PDF** — written from the PDF 1.7 specification (ISO 32000-1).
* **EPUB** — written from the EPUB 3.2 / Open Container Format specifications. **GIF** — written from the GIF89a specification.
* **ext2/3/4, XFS, HFS+, APFS, OpenZFS** — written from Apple's APFS reference, Apple Technical Note TN1150 (HFS+), the kernel.org ext4 and XFS on-disk format documents and the OpenZFS on-disk format description; `mke2fs` and macOS's `newfs_hfs` / `hdiutil` produced the test images.
* **[VGMTrans](https://github.com/vgmtrans/vgmtrans)** (zlib license) — the BRSAR / RSEQ / RBNK / RWAR layouts and NW4R envelope tables that `source/seqsynth.c` plays sequences with.
* **[RiftWii](https://github.com/KakarottoCake/RiftWii)** by KakarottoCake — the design of the RVZ / WIA reader (the disc outside partitions read by disc offset, partition data read decrypted without hashes, the last group kept decoded). No code is taken from it; the reader in `source/rvz.c` is written from **Dolphin**'s `docs/WiaAndRvz.md` format description.
* **[lzo](https://www.oberhumer.com/opensource/lzo/)** / **lzop** by Markus F.X.J. Oberhumer — the LZO1X format and the `.lzo` file layout, decoded by `source/arc_retro.c`, whose PAK layouts and decoder match nintoolbox's.
* **[xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod)** by DarkMatterCore (building on Xyzzy by bushing, libOTP / libSEEPROM by joedj and MINI by Sven Peter, Haxx Enterprises,
  Hector Martin and John Kelley) — the key set, file names and `keys.txt` / `bootmii_keys.bin` formats of the Console Keys drive, and the proven
  OTP and SEEPROM access sequences used here. The hashes used to recognize the SD key come from it too.
* **[wad2bin](https://github.com/DarkMatterCore/wad2bin)** by DarkMatterCore — the `content.bin` and `<index>.bin` layouts (encrypted header, `Bk` header with the included-content bitfield, PRNG-key encrypted contents, the AP certificate and signature), the bogus WAD and how tickets / TMDs are fakesigned, and the IMET / U8 handling. GodModeWii's converter (`source/wad2bin.c`) and its sect233r1 ECDSA (`source/ecc233.c`) are its own code, checked byte for byte against wad2bin's output; MD5 is written from RFC 1321.
* **[dlc-wad-splitter](https://github.com/DarkMatterCore/dlc-wad-splitter)** by DarkMatterCore — the split DLC WAD layout (the TMD kept as it is, content 0 and a run of the other contents in each WAD, `<title id>_split_<n>.wad`, the ticket fakesigned when it was bound to a console) and wad2bin's way of finding the contents a partial DLC WAD holds. `w2b_split` and the content locator in `source/wad2bin.c` are GodModeWii's own code.
* **[Yet Another BlueDump MOD](https://github.com/DarkMatterCore/bluedump-mod)** by DarkMatterCore (building on BlueDump by nicksasa and WiiPower) — what a title backup WAD holds and in which order (certificates, ticket with its console data wiped, TMD, contents padded to 64 bytes, the first content as the footer), finding shared contents through `content.map`, the Priiloader System Menu handling and the common key index change. GodModeWii's dumper (`source/waddump.c`) is its own code.
* **[nanddumper@IOS](https://github.com/Naim2000/nanddumper_ios)** by thepikachugamer (GPL-2.0; with mkwcat's `/dev/sha` exploit) — the NAND backup: its IOS stages (`nanddumper/`, prebuilt in `res/nd_*.bin`), the `/dev/flash` FS patch, the page / bad block reading, the `keys.bin` and `setting.txt` handling and the file layout, ported in `source/nanddump.c`.
* **Sharpii NetCore** / **libWiiSharp** (Leathl and contributors) — WAD, U8, IMD5 and LZ77 (LZ10 / LZ11) layouts.
* **[wfslib / wfs-tools](https://github.com/koolkdev/wfs-tools)** by koolkdev — the WFS layout (areas, directory tries, file layouts, block encryption and key derivation) behind the Wii U volume reader.
* **[vffmod](https://github.com/wii-tools/vffmod)** (wii-tools) and **[vffdump.py](https://marcan.st/transf/vffdump.py)** by marcan — the VFF layout.
* **SaveGame Manager GX** (Dolphin's `WiiSave` code is the reference used for the `data.bin` format) and the **Dolphin** `IOSC` sources — the fixed SD-card key / IV and the Wii common keys.
* **GCMM** (suloku) — GameCube Memory Card Manager, the model for the memory card viewer and its `.gci` backup format; the card access itself uses libogc's CARD API.
* **[WiiBrew](https://wiibrew.org/)** — Hollywood register / GPIO documentation and the SEEPROM layout used for the SEEPROM reader.

## Inspiration

* **[MoonCalc](https://www.gamebrew.org/wiki/MoonCalc)** and **Morning Timer** (for MoonShell) — the calculator and the alarm clock.
* **Terminal** on macOS — the default profiles the themes follow.
* **[GodMode9](https://github.com/d0k3/GodMode9)** by d0k3 — the look, feel and workflow this tool is modeled on.
* **[GodMode9i](https://github.com/DS-Homebrew/GodMode9i)** by the DS-Homebrew team — the DSi / DS take on the same idea.
* **[ftpii](https://code.google.com/p/ftpii/source/list)** by Joseph Jordan and contributors — the Wii FTP server whose idea the built-in FTP server takes further.
* **[WiiXplorer](https://github.com/niuus/wiixplorer)** by Dimok and contributors, and the niuus fork — the Wii file-manager reference.
* The **Dolphin** team, for the emulator used during development.

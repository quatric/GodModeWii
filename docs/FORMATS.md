# What GodModeWii can open

Press **A** on a file and GodModeWii picks the right viewer by **what is in the file** (its magic bytes, after nintoolbox's
detection), not by its name. Renamed or extensionless files still open, and a misleading extension doesn't fool it. Only formats
with no magic of their own go by extension: DSP, BTSND, BTI, TXTR, MP3 without an ID3 tag, and `.lz`-wrapped files.

The viewers only look and listen: nothing is converted, exported or repacked (that's nintoolbox's job).

## Archives and disc images

These open like folders (read-only). **B** goes back out, and **+** or the actions menu copies anything out. Archives nest, e.g.
`sd:/x.wad|/00000001.app|/meta/banner.bin`. Files inside open in the viewers like any other.

| Kind | Formats |
| --- | --- |
| Compression | Yaz0 / SZS, Yay0, ASH0, LZ10 / LZ11 (bare, whatever the name or what they hold, or behind an `LZ77` tag), Huffman, run length, LZH8 (the HOME Menu's compressed files), LZO (`.lzo`), **zlib** and gzip (found by their header whatever the name, as well as `.z`, `.zlib`, `.gz`), **QuickLZ** 1.4.0 / 1.20 (bare, or Monster Games' `PMCr` wrapper), Good-Feel **GFCP** (byte pair or LZ10) and Arika **ALZ1** on their own, and **Camelot STPL** (`.stpl`, or an extensionless file that unpacks to a Camelot texture bank, as on the Mario Power Tennis / Mario Golf: Toadstool Tour discs; the bank is shown as a TPL) |
| Disc images | GameCube / Wii `.iso` / `.gcm`, single-file `.wbfs`, `.ciso`, **RVZ** and **WIA** |
| Disks and partitions | MBR (with logical partitions), GPT and Apple partition maps; **ext2 / ext3 / ext4**, **XFS**, **HFS+ / HFSX**, **APFS** and **OpenZFS** volumes (see [Drives](DRIVES.md)) |
| Documents | **PDF**: each page's text (`page_001.txt`), all of it in `text.txt`, and every embedded image (JPEG as stored, the rest as PNG). See below for what it reads. **EPUB** (2 and 3): each chapter's text in reading order (`chapter_001.txt`), the whole book in `text.txt` (title and author first), and its pictures under `images/` |
| Game archives | **Mario Party `.bin`** (Mario Party 4 – 8: stored, LZSS, slide, run length and zlib members, each unpacked when read and named by what it holds, so an `.hsf` model opens in the model viewer); **Monster Games RST** with its `.toc` (Excite Truck, Excitebots, Excitebike: World Rally; QuickLZ blocks, big- and little-endian, WiiWare `tocres.res`), Camelot `.sbn` modules (their inline texture banks, as TPLs; We Love Golf!), Retro Studios PAK (Metroid Prime 1 – 3, Trilogy, Donkey Kong Country Returns), **HAL sysdolphin `.dat` (HSD)**: its roots, external references and every GX texture, palette ones with their palette (as a one-image TPL, opened by the image viewer; *Open as archive* for a `.dat` that holds a model), **Paon FJG** (Donkey Kong: Barrel Blast; HSD archives, TPLs and nested FJGs back to back, LZ10-compressed or not; members named `.dat` / `.tpl` / `.fjg` by what they hold) |
| NAND dumps | **BootMii `nand.bin`** (with or without spare areas, with or without the appended keys): the newest SFFS superblock as folders, files decrypted with the NAND key from the dump, a `keys.bin` / `otp.bin` next to it, or this console's own OTP |
| Nintendo archives | U8 (`.arc`, `.app`, `.bnr`, `.carc` …, also with an IMET / IMD5 header), RARC, BRRES, Virtual Console CCF, Brawl PAC, Good-Feel GFA, Arika `INFO.DAT` + `GAME.DAT` (Dr. Mario Online Rx, Endless Ocean; ALZ1 members, RF2 groups) |
| PC archives | zip, 7z, RAR 1.5 – 5.x |
| Sound archives | BRSAR: groups, sequences, banks, sound files and every wave, playable |
| Titles and saves | WAD (with a `wad_info.txt`), `content.bin` (DLC / WiiWare SD backup), `data.bin` (Wii save backup), VFF (WiiConnect24 / Mii mailboxes). Decrypted as you browse, vWii-key WADs included |

* A Wii disc image shows each partition (DATA, UPDATE, CHANNEL) with `ticket.bin`, `tmd.bin`, `sys/` and `files/`, decrypted on the
  fly. RVZ / WIA are decompressed on the fly (Zstandard, bzip2, LZMA, LZMA2, purge, RVZ packing).
* zip / 7z / RAR files up to 16 MiB are unpacked whole when first read; bigger ones are streamed.
* *Open as archive* in the actions menu opens a file as an archive explicitly (a `.brres` instead of the model viewer, say).

## Viewers and players

| Viewer | Formats | Buttons |
| --- | --- | --- |
| **3D models** | MDL0 in `.brres` or on its own, J3D `.bmd` / `.bdl`, Mario Party `.hsf` (HSFV037, with each material's texture, or its color), HAL sysdolphin `.dat` / `.usd` (HSD: Super Smash Bros. Melee, Kirby Air Ride; posed by their joints and envelopes, with each material's texture, palette ones included, or its color), Paon `OCN.` models (Donkey Kong: Barrel Blast `chara/` and `entry/` `.dat`, LZ10-compressed: skinned characters in their bind pose, rigid props, CMPR textures) and FJG bundles (each HSD model in it, **1** for the next), and Monster Games `.mod` (Excite Truck / Excitebots; textured from the `.tex` files in their archive, the region's base track and the shared `HobbySet.res` / `race.res` / `common.res`); lit, posed by their bones and textured; a BRRES model plays its animations, looping: `AnmChr` (CHR0) bones, `AnmVis` (VIS0) hidden bones, `AnmTexSrt` (SRT0) moving textures, `AnmClr` (CLR0) material colors (the balloons' palette steps twice a second), `AnmTexPat` (PAT0) texture swaps and `AnmScn` (SCN0) ambient light color; animations of one name play together | D-Pad rotate, **+** / **-** zoom, **A** lit / unlit / wireframe, **1** next model, **2** next animation (or info, for a model with none) |
| **Audio** | BRSTM, BCSTM, BFSTM, BRWAV, BCWAV, BFWAV, BNS, DSP, AST, BTSND, Monster Games `.sfx`, WAV, AIFF, MP3, FLAC, Ogg Vorbis, tracker modules (`.mod`, `.s3m`, `.xm`, `.it`) | **A** pause, **1** loop, **+** / **-** pick channels, **B** stop |
| **Images** | TPL, BTI, TEX0 / BRTEX (with a `.plt0` beside it), PLT0 (as color swatches), BRFNT font sheets, BREFT textures, J3D model textures, GameCube `.bnr`, Wii save `banner.bin`, Retro `.txtr`, Monster Games `.tex` / `.art` / `.img` / `.tm0`, AJPG (Wii Message Board photos), Paon `.d4` (Donkey Kong: Barrel Blast), PNG, JPEG, WebP, TGA (true-color, grayscale and color-mapped, raw or RLE), BMP (1 to 32-bit, RLE4 / RLE8, bit fields and alpha, OS/2 too), GIF (first frame), Photoshop `.psd` / `.psb` (the flattened image), Illustrator `.ai` (its embedded preview). Every GX format (I4 to CMPR, with palettes) | **1** fit / 1:1, **+** / **-** zoom, D-Pad pan (or previous / next image when it all fits), **A** background, **2** info |
| **Layouts** | BRLYT layouts (banners and icons) with their BRLAN animations | **A** play / pause, D-Pad **Left / Right** step a frame, **Up / Down** previous / next animation, **1** loop, **+** / **-** zoom, **2** info |
| **MIDI** | `.mid`, `.rmi`, `.kar`, and BRSAR sequences (a sequence with a loop loops for ever, as in the game) | Up / Down pick a channel, **A** on / off, **2** solo, **-** all on, **+** pause, **1** repeat (sequences: loop / repeat / play the loop twice and fade), **B** stop |
| **Miis** | A Mii (`.miigx`, `.mii`, `.rcd`, `.rsd`) or a Mii database (`RFL_DB.dat`): each Mii's head and shoulders, drawn on the Wii by MiiKit, with its name, creator, gender, birthday, favorite color, height, weight and Mii ID. A database lists its Miis and can export them like **Miis** in the HOME menu | Up / Down pick, **A** mark, **1** mark all, **+** export, **2** change the expression, **B** back |
| **Text** | BMG (message ids, attribute bytes, control codes as `{group.type params}`, and the FLW1 / FLI1 flow nodes), MSBT, BREFF (emitters), BRLYT (pane tree and texts), BRLAN (what it animates), Mario Kart Wii KMP | |
| **Video** | Mobiclip `.mo`, THP, Flipnote `.kwz` / `.ppm`, with sound | **A** / **+** pause, **B** stop |

* **Images** open pixel for pixel (1:1) in their real colors. A `.stpl` is unpacked first and shown as the TPL it holds.
* **Photoshop** files show the flattened image Photoshop saves after the layers (the default "Maximize Compatibility"): 1, 8 and 16 bits
  per channel; bitmap, grayscale, indexed, RGB, CMYK and Lab; raw or RLE. Transparency is kept when the file marks it. ZIP-compressed
  composites and 32-bit float files don't open. An image over 4096 pixels either way is shown at a whole-number fraction of its size.
* **Illustrator** files are PDFs (or PostScript) that can't be drawn here, so the viewer shows the JPEG preview Illustrator stores in the
  file's metadata (usually at most 256 pixels). A file saved without one doesn't open as an image.
* **Monster Games textures** don't store their pixel format, so `.tex` / `.art` / `.img` are decoded in every format that fits and
  the one whose mip levels agree (or whose GX tiles join up) is shown, as nintoolbox and ExciteExtract do: best effort, a few
  percent of files come out in the wrong format. `.tm0` and Excitebots' headed files are exact.
* **Text files** (`.txt`, such as a PDF's pages or an EPUB's chapters) open in the text viewer straight away.
* **A bare MDL0** (opened on its own, not inside its BRRES) gets its textures too: each one is looked up as `<name>.tex0` (with its
  `.plt0`) next to the model, or inside any `.brres` in the same folder. A model opened from inside a BRRES uses that BRRES.
* **Layouts** are drawn with GX: picture and window panes with their textures, texture SRTs and flips, TEV stages, colors,
  alpha compare and blending. A banner plays its Start animation into its Loop one, as on the Wii Menu, and opening a BRLAN
  plays it on its layout. Other animations play once and stop on their last frame (**A** starts them again). Text boxes are not
  drawn, and a layout the viewer can't draw falls back to the text view.
* **MIDI** uses the SoundFont with the same name next to the file (`song.mid` + `song.sf2`), or else a built-in General MIDI set.
  Each channel shows its instrument, note count and level.
* **Audio** covers DSP-ADPCM, AFC and PCM8 / PCM16, plus WAV in PCM, float, IMA / MS ADPCM, A-law, µ-law and RF64, and FLAC in Ogg.
* **Tracker modules** (ProTracker `.mod` with 4 to 32 channels, ScreamTracker `.s3m`, FastTracker 2 `.xm`, Impulse Tracker `.it`) are
  mixed on the Wii at 32 kHz (`source/tracker.c`, its own code): all the usual effects, XM / IT volume columns and volume envelopes
  with fadeout, ping-pong loops and compressed IT samples. Not played: IT's NNA and duplicate checks, pitch and pan envelopes,
  auto-vibrato, filters. A song plays once and stops where it would loop; **1** repeats it. A `.mod` that is not a ProTracker module
  (Monster Games models also use the name) still opens in the model viewer.

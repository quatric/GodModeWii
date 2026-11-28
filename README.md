# GodModeWii

A text-mode file manager for the **Wii** and the **Wii U's vWii mode**, in the style of [GodMode9](https://github.com/d0k3/GodMode9).
No fancy GUI: a console, a cursor and a lot of power.

Version 0.16.0, by quatric & cmyksoda. Looking for the Wii U version? See [GodModeU](https://github.com/quatric/GodModeU).

* Copy, move and delete files between every drive the console can see.
* Open archives, disc images, saves and WADs like folders.
* View images, 3D models, banners and layouts. Play music, sequences and videos.
* Install WADs (to the NAND, an emulated NAND or the SD card) and Wii Menu themes, launch homebrew, and send apps and WADs over
  the network with FTP or `wiiload`.
* Back up installed titles as WADs, saves, and the whole NAND.
* And, only if you really mean it, change the NAND.

> [!CAUTION]
> **GodModeWii can permanently brick your console.** Writing, deleting or renaming files on the NAND (IOS, Wii Menu, boot2,
> tickets...) can leave a Wii, or a Wii U's vWii, unable to boot.
>
> * NAND changes are **locked by default**. Copying *from* the NAND is always safe.
> * The first time you try to change the NAND, a warning appears and you must enter the [Konami code](#konami-code). It then
>   stays unlocked until you exit or lock it again from the HOME menu.
> * **Make a full NAND backup first** (GodModeWii can do it, or use BootMii) and keep a copy off the console.
> * There is no warranty. What you do with it is your responsibility. If you aren't sure what a file does, leave it alone.

## Install

1. Download `GodModeWii.zip` from the [latest release](../../releases/latest).
2. Extract it to the root of your SD card or USB drive. This puts `boot.dol` and `meta.xml` in `apps/GodModeWii/`.
3. Start GodModeWii from the **Homebrew Channel**.

Starting from the Homebrew Channel (IOS58 with HW_AHBPROT disabled) gives GodModeWii full NAND permissions and access to the
SEEPROM and the disc drive. Started any other way, SD and USB still work and the NAND can be browsed with limited permissions.

**On a Wii U**, run it from the vWii Homebrew Channel. NAND access works as in FSToolBox-Mod, and the SD card and USB ports work.
SD Gecko, memory cards, the SEEPROM and the disc drive don't exist inside vWii and show as unavailable. The vWii NAND is just as
easy to brick as a Wii's, and a bricked vWii may need the Wii U's own recovery tools.

## Drives

Every drive is shown with its free space and a usage bar.

| Drive | Path | Notes |
| --- | --- | --- |
| SD card | `sd:/` | Read / write |
| USB drives | `usb1:/` to `usb8:/` | Every port, every drive behind a hub, every LUN of a card reader |
| Wii NAND | `nand:/` | Read-only until unlocked with the Konami code |
| Disc | `disc:/` | Read-only. A Wii or GameCube disc as folders |
| GameCube memory cards | `mca:/`, `mcb:/` | Each save as a `.gci` file. Original Wii only |
| SD Gecko | `gca:/`, `gcb:/` | Original Wii only |
| SEEPROM | `seeprom:/` | Read-only |
| Console keys | `keys:/` | Read-only |
| Emulated NANDs | e.g. `sd:/nands/<name>/` | Listed after the drives when found: SNEEK / UNEEK (a drive's root), neek2o (`nands/<name>/`), `nand/`, `emunand/` |

An emulated NAND opens as folders, like the real one but with nothing to unlock. **Title info** works on its title folders, its
`RFL_DB.dat` opens in the Mii viewer (plaza and Parade), and WADs install into it from a WAD's menu.

FAT, FAT32, exFAT and NTFS are read / write. ext2/3/4, HFS+, APFS, XFS, OpenZFS and Wii U WFS are read-only. Details, including
how to read a Wii U drive, are in [docs/DRIVES.md](docs/DRIVES.md).

## Features

**Files and folders**

* Copy, cut, paste and delete between any writable drives, folders included
* Progress, speed and overwrite / skip prompts, with **hold B to cancel** (half-written files are removed)
* Multi-select, select all, deselect all and invert selection
* Rename, new folder and new empty file
* Info and folder size
* Disk usage: a folder's entries by total size, with a share bar
* Text viewer and hex viewer
* In-place text and hex editors (edits are only written when you save)
* Hex compare: a file against the one on the clipboard, side by side, the differing bytes in red (**A** / **1** jump to the next / previous one)
* Replace a file's contents with the copied file, or inject the copied file into it at an offset (GodMode9's "inject data")
* Zip files and folders (everything in them) from the Actions menu: deflated, with files that are compressed already stored as they are
* Title info for WADs, TMDs, tickets and installed titles: title ID, version, IOS, region, access rights, contents, ticket and signatures, like ShowMiiWads or NUSD show them
* Copying from a disc carries on past read errors and lists the bad sectors
* Deleting read-only FAT files and folders
* Clickable hints along the bottom of each screen (point and press **A**)

**Drives and file systems**

* SD card, USB drives (up to 8 volumes, hubs and card readers), SD Gecko, GameCube memory cards, the disc, the SEEPROM and the NAND
* FAT, FAT32, exFAT and NTFS read / write
* ext2/3/4, HFS+, APFS, XFS, OpenZFS and Wii U WFS read-only
* Disk images and partitions (MBR, GPT, Apple partition map) opened as folders
* Wii U USB drives and WFS images, read with the keys saved by GodModeU
* A `keys:/` drive with the console's keys in xyzzy-mod's layout
* GameCube saves copied to and from memory cards as `.gci` files

**Opening files**

* The viewer is chosen from what is inside the file, not its name
* Archives open like folders: U8, RARC, BRRES, zip, 7z, RAR, PDF, EPUB and many game-specific archives
* Disc images: ISO, GCM, WBFS, CISO, RVZ and WIA, decrypted on the fly
* WADs, `content.bin`, `data.bin`, VFF and BootMii `nand.bin` browsed decrypted
* Many compression formats (Yaz0, Yay0, ASH0, LZ10 / LZ11, LZO, zlib, QuickLZ...)

**Viewers and players**

* 3D models (BRRES, J3D, HSF, HSD and more), lit, posed and textured, with BRRES animations
* Images in every GX format, PNG, JPEG, 3DS MPO, WebP, TGA, BMP, GIF and PSD
* Banners and layouts (BRLYT with BRLAN animations)
* Audio: BRSTM, BCSTM, BFSTM, DSP, WAV, MP3, M4A, FLAC, Ogg Vorbis and more
* Fast-forward and rewind in the audio player
* Tracker modules: `.mod`, `.s3m`, `.xm`, `.it`
* MIDI with SoundFonts, and BRSAR game sequences
* Video: Mobiclip, THP, Flipnote and Photo Channel (Motion-JPEG) videos, with sound
* Miis, drawn on the Wii, with their name, creator, gender, birthday, favourite colour, height and weight
* Text formats: BMG, MSBT, BREFF, BRLYT, BRLAN, KMP

**Installing and launching**

* Install WADs to the NAND, with YAWM ModMii Edition's brick checks
* Install WADs to an emulated NAND (neek2o, uneek, WiiFlow and Dolphin layout)
* Install WADs to the SD card as `content.bin` or DLC `<index>.bin` files (wad2bin)
* A null key for DLC on older games such as Rock Band 2
* Install DLC WADs that have pieces missing
* Split DLC WADs into small WADs, one content (a song) each
* Install Wii Menu themes (`.csm`, `.mym`) with backups
* Build a `.csm` from a `.mym` without touching the NAND
* Launch homebrew (`.dol`, `.elf`) with its `meta.xml` arguments
* Favorite apps: on a `.dol`, `.elf` or an app's folder, *Add to HOME menu favorites* (Actions menu) puts it in HOME > Favorite apps, to start from anywhere
* Development console (RVT) programs run on a retail Wii: Error 002 is patched out of dev-kit (NDEV) ELFs and DOLs as they load,
  before they ever reach the Wii, the way USB Loader GX's anti-002 does it
* Development console WADs are converted for a retail Wii as they install, and the dev common key opens their WADs and RVT-R discs
* Boot a Wii game disc with TinyLoad
* Dump a disc to ISO like CleanRip: GameCube, Wii (single and dual layer), DVD-Video (CSS decrypted with libdvdcss)
  and data DVDs / CDs, from the Wii's drive or a USB CD / DVD drive; damaged discs are retried sector by sector, then
  filled with 0x55 and logged; split into 4 GB parts on FAT32, one file on exFAT / NTFS; CRC32, MD5 and SHA-1 to check on redump.info.
  A spiral shows each part of the disc as it's read, dvdisaster style, green when good and red when not (**1** hides it); a
  disc whose first 8 MB won't read at all is stopped there instead of writing gigabytes of 0x55
* Save data is never deleted, and is backed up before a title is installed over

**Backups**

* Back up installed titles as WADs, the way YABDM does
* Back up a `content.bin` from the SD card as a WAD
* Export all saves as folders or `data.bin` files
* Import a `data.bin` save to the NAND, even for a game or channel that has never been started
* Back up the whole NAND as a BootMii `nand.bin`, with no BootMii needed
* Back up the console keys only
* Console info: model, region, serial and boot2's version; boot1's revision (a–d), bad blocks in the boot blocks and what
  each boot2 copy is (official, BootMii, RGD SDBoot / NANDBoot), read from the NAND chip
* Edit `setting.txt` (region, video, model, serial), backed up first
* Install or update HBC Reborn from its latest GitHub release, replacing the installed Homebrew Channel in place

**Network**

* FTP server, read-only or read / write
* FTP install server for apps and WADs (archives go to the SD card or USB)
* Wiiload: send apps, WADs and files and have them installed to the SD card or a USB drive
* All of them only accept computers on your local network

**HOME menu extras**

* Miis: the Mii Channel's plaza, the Mii Parade (the Miis the Wii has met) and the ten Miis on each Wii Remote, exported as `.miigx`
* A Mii as a QR code that Mii Maker on a 3DS or Wii U scans (converted to their format and encrypted as they expect), on the screen or saved as a PNG; copied to a Wii Remote (its memory backed up first), onto this
  Wii, or off a Wii Remote (MIIS, press **-**)
* Gold pants: make a Mii in the Mii Channel "special" or normal again (MIIS, press **-**)
* Miis import: a Mii file into the Mii Channel plaza or the Mii Parade, or a whole folder of them into the Parade (ACTIONS > Import Miis)
* Sketchpad: draw with the pointer, a mouse, a pen tablet or a uDraw GameTablet
* Voice memos with a Wii Speak or a USB microphone
* IR remote: TV, receiver and set-top box codes from the sensor bar (the Logitech Harmony archive's 259,000 remotes, with search), on any controller's buttons, in named
  presets, sent with OptimizeIR's timing; several presets can be deleted at once
* Set the clock from the internet (SNTP), with the time zone guessed and adjustable
* Boot BootMii or Priiloader's menu, or go straight to the Wii Menu, when they're installed
* Screenshots: press **1** and **2** together, saved as PNGs in `GodModeWii/screenshots/`
* Clock and calendar
* Alarm clock with a beep and the disc slot light
* Sleep timer with white noise, for 15 to 90 minutes or until you turn it off
* Scientific calculator
* Colour themes
* Sound effects and the disc slot light (each can be turned off)
* A clock in the top bar of the browser
* Lock the NAND again, remount drives, restart, exit
* The HOME menu is grouped: drives and network, discs, saves and backups, console and system, tools, settings, and exit and power
* Checking for an update runs in the background after start-up, so the Wii boots straight into the browser

**Safety and input**

* The NAND is locked behind a Konami code, with an optional "always allow" mode
* A brick guard that refuses changes that would certainly brick the Wii (the Wii Menu, its IOS, their tickets, `/sys`, `/shared1`,
  `SYSCONF` and the NAND's top folders), even in Risky Mode
* The write permission level shown by the colour of the tag on the top bar (see below)
* Every controller slot works: four Wii Remotes, four GameCube pads, and the Wii U pads
* The console's own buttons work the menus with no controller at all (see below)
* Wii Remote, Classic Controller, GameCube controller, USB keyboard, PS3 pad, Wii U GamePad and Pro Controller
* Works on a Wii U in vWii mode
* A TiVo sound mode, as an easter egg

The details of each are in [docs/FEATURES.md](docs/FEATURES.md), [docs/FORMATS.md](docs/FORMATS.md),
[docs/DRIVES.md](docs/DRIVES.md) and [docs/NETWORK.md](docs/NETWORK.md).

## What it can open

Press **A** on a file and the viewer is chosen by **what is inside the file**, not by its name. Renamed files still open.

### Archives and disc images

These open like folders, read-only. Archives nest, for example `sd:/x.wad|/00000001.app|/meta/banner.bin`. Press **B** to go back
out, and **+** or the Actions menu to copy anything out.

**Nintendo archives**

| Format | Extensions | Notes |
| --- | --- | --- |
| Arika `INFO.DAT` + `GAME.DAT` | `.dat` | Dr. Mario Online Rx, Endless Ocean |
| Brawl PAC | `.pac` | Super Smash Bros. Brawl |
| BRRES | `.brres` | Models, textures and animations |
| BRWAR | `.brwar` | Its waves, playable |
| Good-Feel GFA | `.gfa` | |
| RARC | | |
| U8 | `.arc` `.app` `.bnr` `.carc` | Also with an IMET / IMD5 header |
| Virtual Console CCF | | |

**PC archives, documents and sound**

| Format | Extensions | Notes |
| --- | --- | --- |
| 7z | `.7z` | |
| BRSAR | `.brsar` | Groups, sequences, wave sounds by name, banks, sound files and waves |
| EPUB | `.epub` | Each chapter's text, and its pictures. EPUB 2 and 3 |
| PDF | `.pdf` | Each page's text, and every embedded image |
| RAR | `.rar` | 1.5 to 5.x |
| zip | `.zip` | |

**Titles, saves and discs**

| Format | Extensions | Notes |
| --- | --- | --- |
| BootMii NAND dump | `nand.bin` | With or without spare areas and appended keys |
| CISO | `.ciso` | |
| `content.bin` | `.bin` | DLC / WiiWare SD backups |
| `data.bin` | `.bin` | Wii save backups |
| GameCube / Wii disc | `.iso` `.gcm` | Each Wii partition decrypted on the fly |
| RVZ | `.rvz` | Decompressed on the fly |
| VFF | `.vff` | WiiConnect24 and Mii mailboxes |
| WAD | `.wad` | Decrypted as you browse, vWii-key WADs included |
| WBFS | `.wbfs` | Single-file |
| WIA | `.wia` | Decompressed on the fly |

**Disks and partitions**

| Format | Extensions | Notes |
| --- | --- | --- |
| APFS | | Each volume is a folder |
| Apple partition map | | |
| Disk images | `.img` `.iso` | A raw image of a whole disk |
| ext2 / ext3 / ext4 | | Read-only |
| GPT | | |
| HFS+ / HFSX | | |
| MBR | | With logical partitions |
| OpenZFS | | |
| XFS | | v4 and v5 |

**Game-specific archives**

| Format | Extensions | Games |
| --- | --- | --- |
| Camelot module | `.sbn` | We Love Golf! |
| HAL sound bank | `.ssm` | Super Smash Bros. Melee, Kururin Squash!. Each sound as a playable `.dsp` |
| HAL sysdolphin (HSD) | `.dat` | Super Smash Bros. Melee, Kirby Air Ride |
| Mario Party archive | `.bin` | Mario Party 4 to 8 |
| Monster Games RST + TOC | `.toc` | Excite Truck, Excitebots, Excitebike: World Rally |
| Paon FJG | | Donkey Kong: Barrel Blast |
| Retro Studios PAK | | Metroid Prime 1 to 3, Trilogy, Donkey Kong Country Returns |

**Compression**

| Format | Extensions | Notes |
| --- | --- | --- |
| ALZ1 | | Arika |
| ASH0 | `.ash` | |
| Camelot STPL | `.stpl` | Mario Power Tennis, Mario Golf: Toadstool Tour |
| GFCP | | Good-Feel (byte pair or LZ10) |
| gzip | `.gz` | Found by its header |
| Huffman | | |
| LZ10 / LZ11 | | Bare, or behind an `LZ77` tag |
| LZH8 | | The HOME Menu's compressed files |
| LZO | `.lzo` | |
| QuickLZ | | Versions 1.4.0 and 1.20, bare or in Monster Games' `PMCr` wrapper |
| Run length | | |
| Yay0 | | |
| Yaz0 / SZS | `.szs` | |
| zlib | `.z` `.zlib` | Found by its header |

### Viewers and players

**3D models** (lit, posed by their bones and textured)

| Format | Extensions | Games / notes |
| --- | --- | --- |
| HAL sysdolphin (HSD) | `.dat` `.usd` | Super Smash Bros. Melee, Kirby Air Ride |
| J3D | `.bmd` `.bdl` | |
| Mario Party HSF | `.hsf` | HSFV037 |
| MDL0 | `.brres` | Plays the BRRES animations: bones, hidden bones, moving textures, material colours, texture swaps, ambient light |
| Monster Games models | `.mod` | Excite Truck, Excitebots |
| Paon `OCN.` models and FJG bundles | `.dat` | Donkey Kong: Barrel Blast |

**Images**

| Format | Extensions | Notes |
| --- | --- | --- |
| AJPG | | Wii Message Board photos |
| BMP | `.bmp` | 1 to 32-bit, RLE4 / RLE8, bit fields and alpha, OS/2 too |
| BREFT | `.breft` | Textures |
| BRFNT | `.brfnt` | Font sheets |
| BTI | `.bti` | |
| GameCube banner | `.bnr` | |
| GIF | `.gif` | First frame |
| GX texture formats | | Every one, I4 to CMPR, with palettes |
| Illustrator | `.ai` | Its embedded preview |
| JPEG | `.jpg` `.jpeg` | |
| Monster Games textures | `.tex` `.art` `.img` `.tm0` | Excite Truck, Excitebots |
| MPO | `.mpo` | 3DS photos: both the left and the right picture |
| Paon texture | `.d4` | Donkey Kong: Barrel Blast |
| Photoshop | `.psd` `.psb` | The flattened image |
| PLT0 | `.plt0` | As colour swatches |
| PNG | `.png` | |
| Retro texture | `.txtr` | |
| TEX0 / BRTEX | `.tex0` `.brtex` | With a `.plt0` beside it |
| TGA | `.tga` | True-colour, grayscale and colour-mapped, raw or RLE |
| TPL | `.tpl` | |
| WebP | `.webp` | |
| Wii save banner | `banner.bin` | |

**Banners and layouts**

| Format | Extensions | Notes |
| --- | --- | --- |
| BRLAN | `.brlan` | Animations, played on their layout |
| BRLYT | `.brlyt` | Banners and icons, drawn with GX |

**Audio**

| Format | Extensions | Notes |
| --- | --- | --- |
| AIFF | `.aiff` | |
| AST | `.ast` | |
| BCSTM | `.bcstm` | |
| BCWAV | `.bcwav` | |
| BFSTM | `.bfstm` | |
| BFWAV | `.bfwav` | |
| BNS | `.bns` | |
| BRSTM | `.brstm` | |
| BRWAV | `.brwav` | |
| BTSND | `.btsnd` | The Wii's boot sound |
| DSP | `.dsp` | |
| FastTracker 2 | `.xm` | |
| FLAC | `.flac` | Also in Ogg |
| HAL HPS | `.hps` | Super Smash Bros. Melee, Kirby Air Ride. Loops as in the game |
| Impulse Tracker | `.it` | |
| M4A (AAC) | `.m4a` | What the Photo Channel plays. Needs `ppc-faad2` at build time |
| Monster Games sound | `.sfx` | |
| MP3 | `.mp3` | |
| Ogg Vorbis | `.ogg` | |
| ProTracker | `.mod` | 4 to 32 channels |
| ScreamTracker | `.s3m` | |
| WAV | `.wav` | PCM, float, IMA / MS ADPCM, A-law, µ-law, RF64 |

**MIDI and sequences**

| Format | Extensions | Notes |
| --- | --- | --- |
| BRSAR sequences | | A sequence with a loop loops for ever, as in the game |
| MIDI | `.mid` `.rmi` `.kar` | Uses the SoundFont with the same name next to it, or a built-in General MIDI set |

**Miis**

| Format | Extensions | Notes |
| --- | --- | --- |
| Mii | `.miigx` `.mii` `.rcd` `.rsd` | Head and shoulders, with name, creator, birthday and more |
| Mii database | `RFL_DB.dat` | Lists its Miis, and can export them |

**Video** (with sound)

| Format | Extensions | Notes |
| --- | --- | --- |
| Flipnote | `.kwz` `.ppm` | |
| Mobiclip | `.mo` | |
| Motion-JPEG | `.avi` `.mov` | The Photo Channel's videos, with PCM sound |
| THP | `.thp` | |

**Text**

| Format | Extensions | Notes |
| --- | --- | --- |
| BMG | `.bmg` | Message ids, attribute bytes, control codes and flow nodes |
| BREFF | `.breff` | Emitters |
| BRLAN | `.brlan` | What it animates |
| BRLYT | `.brlyt` | The pane tree and texts |
| Mario Kart Wii KMP | `.kmp` | |
| MSBT | `.msbt` | |

The viewers only look and listen. Nothing is converted or repacked (that's [nintoolbox](https://github.com/quatric/nintoolbox)'s
job). Each viewer's buttons are in [docs/FORMATS.md](docs/FORMATS.md).

## Installing with Wiiload

Wiiload sends things to the Wii over the network, or over a USB Gecko in a memory card slot, and installs them on the **SD
card, a USB drive or an SD Gecko** (press **1** on the Wii for the next drive). The Wii warns when the Homebrew Channel can't
load apps from that drive (not FAT, not the first partition, not the first USB drive). On the Wii, open HOME → *Wiiload*, then
from your computer:

```sh
zip -r myapp.zip myapp
WIILOAD=tcp:192.168.1.23 wiiload myapp.zip
WIILOAD=/dev/ttyUSB0 wiiload myapp.zip     # over a USB Gecko (COMn on Windows)
```

* **Apps:** a `.dol` / `.elf`, or a `.zip` / `.7z` / `.rar` holding a program, is installed to `apps/<name>/` on the drive.
* **WADs:** install them to the NAND, or to the SD card (wad2bin), or leave them as files.
* **Archives of files** (WADs, `private/`, `apps/`...) are unpacked onto the drive the way the FTP install server does.
* **Anything else** is saved at the root of the drive.

See [docs/NETWORK.md](docs/NETWORK.md) for the full rules.

## Controls

| Button | File browser |
| --- | --- |
| D-Pad Up / Down | Move (hold to scroll) |
| D-Pad Left / Right | Page up / down |
| **A** | Open a folder, archive or file. Paste on the green `Paste` row. |
| **B** | Up one folder (back to the drive list at the top) |
| **1** | Mark / unmark and move down. Clears the clipboard on the `Paste` row. |
| **2** | Actions menu (paste, copy, cut, delete, rename, new folder, info, viewers, editors...) |
| **+** | Copy the marked items (or the one under the cursor) |
| **-** | Cut the marked items |
| **HOME** | HOME menu (HOME or B closes it) |

**To copy or move:** copy (**+**) or cut (**-**), browse to the destination, and press **A** on the green `Paste` row at the top. A
copy stays in the clipboard so you can paste it in several places. **1** on the `Paste` row empties it.

**Other controllers:**

| Controller | **+** | **-** | **1** | **2** | A / B / HOME |
| --- | --- | --- | --- | --- | --- |
| Classic Controller | ZR, + | ZL, - | Y | X | A / B / HOME |
| GameCube controller | R | L | Y | X | A / B / Start |
| PS3 SIXAXIS / DualShock 3 | Start, R1, R2 | Select, L1, L2 | Square | Triangle | Cross / Circle / PS |
| USB keyboard | C (copy) | V (cut) | X | Y | Enter / Backspace / Esc, arrows to move |
| Wii U GamePad, Pro Controller | R, ZR, + | L, ZL, - | Y | X | A / B / HOME |

The GamePad, Pro Controller and PS3 pad work in release builds but are untested on real hardware. Wii Remotes have to be synced
in the Wii Menu first: libogc2 can't pair new ones.

**Screenshot:** **1** and **2** together, anywhere. A lone **1** or **2** still works as usual.

**The console's buttons** (when no controller is connected; the HOME menu's *Settings > Console buttons work the menus* sets: with no controller, always, or off):

| Button | Does |
| --- | --- |
| **Power** | Down (the next item) |
| **Reset** (tap) | A |
| **Reset** (hold ~1 s) | 2 (the actions menu) |
| **Reset** (hold ~3 s) | HOME |
| **Power** while holding Reset | B |
| Eject a disc | HOME |

### Write permission levels

Like GodMode9, the tag at the right of the top bar says what GodModeWii is allowed to change. Only the tag takes the colour; the bar
keeps your theme's.

| Tag | Colour | Means |
| --- | --- | --- |
| `SAFE` | Green | Only the SD card, USB drives and other removable storage can be written |
| `UNLOCKED` | Yellow | The Konami code was entered: the NAND's ordinary data (saves, channels, titles) can be changed |
| `CONSOLE DATA` | Magenta | Unlocked, and you're in console-unique data (the keys, the SEEPROM, `/sys`, `/shared2/sys`, the Wii Menu's data) |
| `FORCED` | Red | Risky Mode was forced on at start-up: nothing asks first |

GodMode9's orange is magenta here, since the Wii's console has eight colours. Whatever the level, the brick guard still refuses the
few changes that would leave the Wii unbootable.

### Konami code

It appears by itself the first time you try to change the NAND. On the D-Pad, then B and A:

`UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A`

HOME cancels. Once entered, the NAND stays unlocked for the rest of the session.

**Always allow dangerous actions:** enter the same code while the "Looking for drives..." screen is up at start-up. It says *Risky
mode forced on* once, and from then on nothing asks for a warning or the code again. Enter it at start-up once more to turn it off.
(It's saved as `force.cfg` in `apps/GodModeWii/` on the SD card.) Take your time: a code that is part way in keeps the screen up
for up to 15 seconds.

**IR remote shortcut:** hold **1** (Y on a GamePad, Classic or GameCube pad) while GodModeWii starts to go straight to the IR
remote with the last preset. In the remote, **HOME + Left / Right** switches preset and the controller buzzes once per preset
number, so you can change it with the TV on another input; **HOME** on its own leaves.

## Status

Developed against devkitPPC / libogc and tested mostly in Dolphin.

**Tried on a Wii U (vWii):** the SD card; FAT32, NTFS and ext4 USB drives; a Wii U USB drive read with GodModeU's saved keys; the
FTP server; exporting Miis.

**Not yet verified on real hardware** (please report what you see): the boot1 / boot2 check, installing HBC Reborn, multi-drive and multi-LUN USB, SD Gecko, Wiiload over a USB Gecko, the SEEPROM / OTP
readers and the Console Keys drive, disc browsing, archives (zip / 7z / RAR, RVZ / WIA, Retro PAK), the model viewer, the audio
player, the extra controllers, and the IR remote (sent from the sensor bar).

Known limits for each area (drives, discs, memory cards, archives, keys, disk file systems, PDF, EPUB...) are in
[docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## Building

You need [devkitPro](https://devkitpro.org/) with devkitPPC, libogc2 and libfat, plus some portlibs:

```sh
dkp-pacman -S ppc-libpng-apng ppc-libjpeg-turbo ppc-libwebp ppc-zlib ppc-bzip2 ppc-libvorbisidec ppc-libogg ppc-faad2
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

This writes `GodModeWii.dol` and a much smaller self-extracting `GodModeWii-xz.dol` (copy it as `boot.dol`). Optional features
(exFAT, ext4, NTFS, extra controllers), testing in Dolphin and CI are covered in [docs/BUILDING.md](docs/BUILDING.md).

## Credits

GodModeWii is by quatric & cmyksoda, and it stands on a lot of other people's work. Thank you! Details, licences and file names are
in [docs/CREDITS.md](docs/CREDITS.md).

**Code included**

* [CleanRip](https://github.com/emukidid/cleanrip) (emu_kidid) and [spycrab's CleanRip](https://github.com/spycrab/cleanrip): how the disc dumper gets past read errors
* [csm-installer](https://github.com/Naim2000/csm-installer) (Naim2000): the Wii Menu theme installer
* [devkitPro](https://devkitpro.org/): devkitPPC, libogc2 and libfat
* [dolxz](https://github.com/FIX94/dolxz) (FIX94): the small self-extracting `.dol`
* [dr_libs](https://github.com/mackron/dr_libs) (David Reid): WAV, MP3 and FLAC decoding
  * [minimp3](https://github.com/lieff/minimp3) (lieff)
* [FSToolBox-Mod](https://github.com/WiiDatabase/FSToolBox-Mod) (WiiDatabase.de team, nickasa, Lupo96): NAND access on the Wii and vWii
* [GameTDB](https://www.gametdb.com): box and disc art
* [GlowWii](https://github.com/larsenv/GlowWii) (larsenv): the disc slot light, and the controller input layer
* [libdvdcss](https://www.videolan.org/developers/libdvdcss.html) (VideoLAN): DVD-Video decryption in the disc dumper
* [libntfs](https://github.com/extremscorner/libntfs) (Extrems) and NTFS-3G: NTFS drives
* libpng, libjpeg-turbo, libwebp, zlib, bzip2, Tremor and libogg (devkitPro portlibs)
* libruntimeiospatch (Nano, Joseph Jordan, damysteryman, DarkMatterCore, megazig, FIX94): the IOS patch patterns
* [libwbfs](https://github.com/kwiirk/wbfs) (Kwiirk, Waninkoko): the WBFS layout
* libwiidrc, libwupc (FIX94) and libsicksaxis (xerpi): GamePad, Pro Controller and PS3 pad support
* [Logitech Harmony IR archive](https://github.com/pickysysadmin/logitech-harmony-ir-archive) (CC0): the IR remote's device list, as `irdb.dat`
* [lwext4](https://github.com/gkostka/lwext4) (Grzegorz Kostka) and Extrems' libdvm ext4 driver: ext2/3/4 drives
* [MiiKit](https://github.com/cmyksoda/MiiKit) (cmyksoda, from hbc-reborn's sdk/mii): the Mii faces
* [MoonShell](https://www.gamebrew.org/wiki/Moonshell) (Moonlight): the interface sound effects
* mym-to-csm (cmyksoda): ported into `source/mym.c`
* OptimizeIR and SingIR (Dakota Thorpe): the IR remote's sensor bar transmitter
* [PictoCard](https://github.com/larsenv/PictoCard-Client) (larsenv): the sketchpad's palette, brushes and tools
* Priiloader (Dacoslove): USB keyboard handling and the vWii check, via GlowWii
* [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) (Project Nayuki, MIT): the Mii QR codes
* [SaveGame Manager GX](https://github.com/Cyan-Coding/savegame-manager-gx), [GCMM](https://github.com/suloku/gcmm) and GerbilSoft's [rom-properties](https://github.com/GerbilSoft/rom-properties): save icon and banner formats for the save manager
* [TinyLoad](https://wiibrew.org/wiki/TinyLoad) (marcan and others, with the USB Gecko kit): the game disc loader
* [TinySoundFont](https://github.com/schellingb/TinySoundFont) (Bernhard Schelling): MIDI playback
  * [SFZero](https://github.com/stevefolta/SFZero) (Steve Folta) and the earlevel.com biquad filter
  * Gervill's emergency soundbank (OpenJDK): the built-in General MIDI set
* TiVo: the easter-egg interface sounds
* [unarr](https://github.com/selmf/unarr) (selmf and contributors): zip, 7z and RAR readers
  * LZMA SDK (Igor Pavlov) and The Unarchiver's XADMaster code
* [wii-mobiclip-player](https://github.com/quatric/wii-mobiclip-player) (quatric): the Mobiclip, THP and Flipnote players
  * FFmpeg's Mobiclip decoder (Florian Nouwt, Adib Surani, Paul B Mahol), PlayMobic, MobiclipDecoder and Gericom's x264 fork, WiiLink24's FFmpeg fork
  * NanoJPEG (Martin J. Fiedler) and Tremor
* [WiiIR](https://github.com/dakotath/WiiIR) (Dakota Thorpe, used with permission), through [libwiiir](https://github.com/quatric/libwiiir): the IR remote's sender and its irdb.xml schema
* [YAWM ModMii Edition](https://github.com/modmii/YAWM-ModMii-Edition) (OverjoY, blackb0x, scooby74029, Peter0x44, XFlak): the WAD installer, brick checks and the `.dol` / `.elf` launcher
* [Zstandard](https://github.com/facebook/zstd) (Meta): RVZ decoding, with xxHash and Finite State Entropy (Yann Collet)

**Formats and references**

* AJPG / ODH: from the mobipeg FFmpeg fork
* Apple's APFS reference and TN1150 (HFS+), the kernel.org ext4 and XFS documents, the OpenZFS format description
* [dlc-wad-splitter](https://github.com/DarkMatterCore/dlc-wad-splitter) (DarkMatterCore): the split DLC WAD layout
* GCMM (suloku): the memory card viewer and `.gci` format
* [HSDLib](https://github.com/Ploaj/HSDLib) and [Metanoia](https://github.com/Ploaj/Metanoia) (Ploaj): HSD and HSF
* [lzo](https://www.oberhumer.com/opensource/lzo/) / lzop (Markus Oberhumer)
* Mario Party `.bin`, Camelot STPL and texture banks, Monster Games (ExciteExtract): via nintoolbox
* MII-FLOOD-2: the Mii Parade's list and record layout in `RFL_DB.dat`
* [nanddumper@IOS](https://github.com/Naim2000/nanddumper_ios) (thepikachugamer, mkwcat's `/dev/sha` exploit): the NAND backup
* [nintoolbox](https://github.com/quatric/nintoolbox) and [mobipeg](https://github.com/quatric/mobipeg) (quatric): the first-party formats, compression and file-type detection
* Priiloader (DacoTaco) and BootMii (Team Twiizers): how to boot each from a program
* [QuickLZ](http://www.quicklz.com/) (Lasse Mikkel Reinhold)
* RFC 4330 (SNTP): the network clock
* [RiftWii](https://github.com/KakarottoCake/RiftWii) (KakarottoCake) and Dolphin's `WiaAndRvz.md`: the RVZ / WIA reader
* SaveGame Manager GX and Dolphin's `WiiSave` and `IOSC` code: `data.bin` and the fixed keys
* Sharpii NetCore / libWiiSharp (Leathl): WAD, U8, IMD5 and LZ77 layouts
* The PDF 1.7, EPUB 3.2 and GIF89a specifications
* [USB Loader GX](https://github.com/wiidev/usbloadergx) (its anti-002 patch) and ndev_to_rvl: running development console programs on a retail Wii
* [vffmod](https://github.com/wii-tools/vffmod) (wii-tools) and vffdump.py (marcan): VFF
* [VGMTrans](https://github.com/vgmtrans/vgmtrans): the BRSAR / RSEQ / RBNK / RWAR layouts
* [wad2bin](https://github.com/DarkMatterCore/wad2bin) (DarkMatterCore): `content.bin`, `<index>.bin` and the bogus WAD
* [wfslib / wfs-tools](https://github.com/koolkdev/wfs-tools) (koolkdev): Wii U WFS
* WiiBrew: the Mii data, the Wii Remote's memory, Hollywood registers, SYSCONF and the SEEPROM layout
* WiiElfFix and RVT2RVL: the dev-kit ELF and WAD fixes
* [xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod) (DarkMatterCore, bushing, joedj, Sven Peter, Hector Martin, John Kelley): the Console Keys drive
* [Yet Another BlueDump MOD](https://github.com/DarkMatterCore/bluedump-mod) (DarkMatterCore, nicksasa, WiiPower): the WAD backup format

**Inspiration**

* [dvdisaster](https://dvdisaster.jcea.es/): the disc dumper's spiral
* [ftpii](https://code.google.com/p/ftpii/source/list) (Joseph Jordan) and [WiiXplorer](https://github.com/niuus/wiixplorer) (Dimok)
* [GodMode9](https://github.com/d0k3/GodMode9) (d0k3) and [GodMode9i](https://github.com/DS-Homebrew/GodMode9i) (DS-Homebrew), and GodMode9's write permission colours
* mii-qr: Miis as QR codes; jaames' mii-qr.py: the 3DS / Wii U QR code's encryption; 3dbrew: their Mii format; MiiJS (Stewared, ISC): the
  Wii to 3DS face features
* MoonCalc and Morning Timer (for MoonShell): the calculator and the alarm
* ShowMiiWads (Leathl) and NUSD: the title info screen
* Terminal on macOS: the colour themes
* The Dolphin team, for the emulator used during development

## License

GodModeWii is released under the **GNU General Public License v3.0 or later** (see `LICENSE`), which also covers the
GPL-3.0 / AGPL-3.0 / Apache-2.0 / LGPL-3.0 code it is built with (libwiiinput, libwiiupdate, libwiiir, unarr). Programs it
carries and starts separately (the TinyLoad disc loader, the nanddumper@IOS stages and the app booter) keep their own GPL-2.0
license; their source is in this repository. See `docs/CREDITS.md` for every component and its license.

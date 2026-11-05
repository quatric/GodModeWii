# GodModeWii

**Version:** 0.9.0 · **By:** quatric & cmyksoda

A text-mode file manager for the **Wii** and the **Wii U's vWii mode**, in the style of [GodMode9](https://github.com/d0k3/GodMode9).
No fancy GUI: just a console, a cursor and a lot of power.

* Copy, move and delete files between every drive the console can see.
* Open archives, disc images, saves and WADs like folders.
* View images, 3D models, banners and layouts; play music, sequences and videos.
* Install WADs, launch homebrew, and install apps and WADs over the network (FTP or `wiiload`).
* And, only if you really mean it, change the NAND.

> [!CAUTION]
> **GodModeWii can permanently brick your console.** Writing, deleting or renaming files on the NAND (IOS, System Menu,
> boot2, tickets ...) can leave a Wii, or a Wii U's vWii, unable to boot.
>
> * NAND changes are **locked by default**. Copying *from* the NAND is always safe.
> * The first time you try to change the NAND, a warning appears and you must enter the [Konami code](#konami-code).
>   It then stays unlocked until you exit, or lock it again from the HOME menu.
> * **Make a full NAND backup with BootMii first**, and keep a copy off the console.
> * There is **no warranty**. You are responsible for what you do with it.
>
> If you aren't sure what a file does, leave it alone.

## Contents

* [Getting started](#getting-started)
* [Drives](#drives)
* [What it can open](#what-it-can-open)
* [Other features](#other-features)
* [Controls](#controls)
* [Network: FTP and Wiiload](#network-ftp-and-wiiload)
* [Console keys](#console-keys)
* [Building](#building)
* [Status and limitations](#status-and-limitations)
* [Credits](#credits) · [License](#license)

## Getting started

1. Download `GodModeWii.zip` from the [latest release](../../releases/latest) (older versions are on the [releases page](../../releases)).
2. Extract it to the root of your SD card or USB drive. This puts `boot.dol` and `meta.xml` in `apps/GodModeWii/`.
3. Start GodModeWii from the **Homebrew Channel**.

Launching from the Homebrew Channel (IOS58 with HW_AHBPROT disabled) gives GodModeWii full NAND permissions and access to
the SEEPROM and the disc drive. Launched another way, SD and USB still work and the NAND can be browsed with limited permissions.

**On a Wii U**, run it from the vWii Homebrew Channel. NAND access works as in FSToolBox-Mod, and the SD card and USB ports are
supported. SD Gecko, memory cards, the SEEPROM and the disc drive don't exist inside vWii and are shown as unavailable. The vWii
NAND is just as brick-prone as a Wii's, and a bricked vWii may need the Wii U's own recovery tools.

## Drives

Every drive is shown with its free space and a usage bar.

| Drive | Path | Notes |
| --- | --- | --- |
| Console keys | `keys:/` | Read-only; see [Console keys](#console-keys) |
| Disc | `disc:/` | Read-only. A Wii or GameCube disc as folders, plus `sys/` (`boot.bin`, `bi2.bin`, `apploader.img`, `fst.bin`, `main.dol`). GameCube and unrecognised discs also get a raw `disc.iso` |
| GameCube memory cards | `mca:/`, `mcb:/` | Each save as a GCMM-compatible `.gci` file: copy out to back up, paste in to restore. Original Wii only |
| SD card | `sd:/` | Read / write |
| SD Gecko (slot A / B) | `gca:/`, `gcb:/` | Original Wii only |
| SEEPROM | `seeprom:/` | Read-only; holds `seeprom.bin` |
| USB drives | `usb1:/` … `usb8:/` | Every port, every drive behind a hub, every LUN of a card reader (up to 8 volumes) |
| Wii NAND | `nand:/` | Read-only until unlocked with the Konami code |

**File systems:**

| File system | Support |
| --- | --- |
| APFS | Read-only, built-in reader; each volume is a folder, encrypted volumes are skipped |
| exFAT | Read / write (release builds); MBR, GPT or unpartitioned; files over 4 GiB |
| ext2 / ext3 / ext4 | Read-only. Mounted from SD and USB (`WITH_EXT4`), and read from disk images, partitions and raw drives by the built-in reader (below) |
| FAT / FAT32 | Read / write |
| HFS+ / HFSX (Mac OS Extended, journaled or not) | Read-only, built-in reader |
| NTFS | Read / write. A drive Windows didn't shut down fully (Fast Startup, hibernation) is left alone |
| OpenZFS | Read-only, built-in reader; single-disk and mirror pools, lzjb / zle / lz4 / gzip / zstd |
| Wii U WFS | Read-only; needs that Wii U's keys (see below) |
| XFS (v4 and v5) | Read-only, built-in reader |

* Extra FAT / exFAT / NTFS partitions on a USB drive each get their own `usbN:` entry ("USB Drive 1, part. 2").
* **Drives the Wii can't mount** (an APFS, HFS+, XFS or ZFS disk, or one with a GPT / MBR / Apple partition map) are shown read-only
  as a disk: each partition is a `1_Linux.part`-style file that opens as the file system inside it. Disk images (`.img`, or a raw
  `.iso` of such a disk) open the same way.
* A FAT32 drive hidden from the Wii U with **ustealth / USBHide** works like any other; the home screen notes that it is hidden.
* **Wii U WFS:** a USB drive formatted by a Wii U shows up as its own `usbN:/` drive, and a WFS image (a dump of a Wii U USB drive
  or of the MLC) opens like an archive. WFS is encrypted with keys of the Wii U that formatted it, so put them in `sd:/wfs/`
  (or the card's root) first, either as that Wii U's `otp.bin` and `seeprom.bin`, or as the finished 16-byte keys `usb.key` / `mlc.key`.
  The easiest way: on that Wii U, GodModeU → SYSTEM → *Save Wii U keys for GodModeWii*. Decrypted ("plain") images need no key.
  Remount drives (HOME menu) after changing the key files.

## What it can open

Press **A** on a file and GodModeWii picks the right viewer by **what is in the file** (its magic bytes, after nintoolbox's
detection), not by its name. Renamed or extensionless files still open, and a misleading extension doesn't fool it. Only formats
with no magic of their own go by extension: DSP, BTSND, BTI, TXTR, MP3 without an ID3 tag, and `.lz`-wrapped files.

The viewers only look and listen: nothing is converted, exported or repacked (that's nintoolbox's job).

### Archives and disc images

These open like folders (read-only). **B** goes back out, and **+** or the actions menu copies anything out. Archives nest, e.g.
`sd:/x.wad|/00000001.app|/meta/banner.bin`. Files inside open in the viewers like any other.

| Kind | Formats |
| --- | --- |
| Compression | Yaz0 / SZS, Yay0, ASH0, LZ10 / LZ11 (bare, whatever the name or what they hold, or behind an `LZ77` tag), Huffman, run length, LZH8 (the HOME Menu's compressed files), LZO (`.lzo`), **zlib** and gzip (found by their header whatever the name, as well as `.z`, `.zlib`, `.gz`), **QuickLZ** 1.4.0 / 1.20 (bare, or Monster Games' `PMCr` wrapper), Good-Feel **GFCP** (byte pair or LZ10) and Arika **ALZ1** on their own, and **Camelot STPL** (`.stpl`, or an extensionless file that unpacks to a Camelot texture bank, as on the Mario Power Tennis / Mario Golf: Toadstool Tour discs; the bank is shown as a TPL) |
| Disc images | GameCube / Wii `.iso` / `.gcm`, single-file `.wbfs`, `.ciso`, **RVZ** and **WIA** |
| Disks and partitions | MBR (with logical partitions), GPT and Apple partition maps; **ext2 / ext3 / ext4**, **XFS**, **HFS+ / HFSX**, **APFS** and **OpenZFS** volumes (see [Drives](#drives)) |
| Documents | **PDF**: each page's text (`page_001.txt`), all of it in `text.txt`, and every embedded image (JPEG as stored, the rest as PNG). See below for what it reads. **EPUB** (2 and 3): each chapter's text in reading order (`chapter_001.txt`), the whole book in `text.txt` (title and author first), and its pictures under `images/` |
| Game archives | **Mario Party `.bin`** (Mario Party 4 – 8: stored, LZSS, slide, run length and zlib members, each unpacked when read and named by what it holds, so an `.hsf` model opens in the model viewer); **Monster Games RST** with its `.toc` (Excite Truck, Excitebots, Excitebike: World Rally; QuickLZ blocks, big- and little-endian, WiiWare `tocres.res`), Camelot `.sbn` modules (their inline texture banks, as TPLs; We Love Golf!), Retro Studios PAK (Metroid Prime 1 – 3, Trilogy, Donkey Kong Country Returns), **HAL sysdolphin `.dat` (HSD)**: its roots, external references and every GX texture, palette ones with their palette (as a one-image TPL, opened by the image viewer; *Open as archive* for a `.dat` that holds a model) |
| NAND dumps | **BootMii `nand.bin`** (with or without spare areas, with or without the appended keys): the newest SFFS superblock as folders, files decrypted with the NAND key from the dump, a `keys.bin` / `otp.bin` next to it, or this console's own OTP |
| Nintendo archives | U8 (`.arc`, `.app`, `.bnr`, `.carc` …, also with an IMET / IMD5 header), RARC, BRRES, Virtual Console CCF, Brawl PAC, Good-Feel GFA, Arika `INFO.DAT` + `GAME.DAT` (Dr. Mario Online Rx, Endless Ocean; ALZ1 members, RF2 groups) |
| PC archives | zip, 7z, RAR 1.5 – 5.x |
| Sound archives | BRSAR: groups, sequences, banks, sound files and every wave, playable |
| Titles and saves | WAD (with a `wad_info.txt`), `content.bin` (DLC / WiiWare SD backup), `data.bin` (Wii save backup), VFF (WiiConnect24 / Mii mailboxes). Decrypted as you browse, vWii-key WADs included |

* A Wii disc image shows each partition (DATA, UPDATE, CHANNEL) with `ticket.bin`, `tmd.bin`, `sys/` and `files/`, decrypted on the
  fly. RVZ / WIA are decompressed on the fly (Zstandard, bzip2, LZMA, LZMA2, purge, RVZ packing).
* zip / 7z / RAR files up to 16 MiB are unpacked whole when first read; bigger ones are streamed.
* *Open as archive* in the actions menu opens a file as an archive explicitly (a `.brres` instead of the model viewer, say).

### Viewers and players

| Viewer | Formats | Buttons |
| --- | --- | --- |
| **3D models** | MDL0 in `.brres` or on its own, J3D `.bmd` / `.bdl`, Mario Party `.hsf` (HSFV037, with each material's texture, or its colour), HAL sysdolphin `.dat` / `.usd` (HSD: Super Smash Bros. Melee, Kirby Air Ride; posed by their joints and envelopes, with each material's texture, palette ones included, or its colour), and Monster Games `.mod` (Excite Truck / Excitebots; textured from the `.tex` files in their archive, the region's base track and the shared `HobbySet.res` / `race.res` / `common.res`); lit, posed by their bones and textured | D-Pad rotate, **+** / **-** zoom, **A** lit / unlit / wireframe, **1** next model |
| **Audio** | BRSTM, BCSTM, BFSTM, BRWAV, BCWAV, BFWAV, BNS, DSP, AST, BTSND, Monster Games `.sfx`, WAV, AIFF, MP3, FLAC, Ogg Vorbis | **A** pause, **1** loop, **+** / **-** pick channels, **B** stop |
| **Images** | TPL, BTI, TEX0 / BRTEX (with a `.plt0` beside it), PLT0 (as colour swatches), BRFNT font sheets, BREFT textures, J3D model textures, GameCube `.bnr`, Wii save `banner.bin`, Retro `.txtr`, Monster Games `.tex` / `.art` / `.img` / `.tm0`, AJPG (Wii Message Board photos), PNG, JPEG, WebP, GIF (first frame). Every GX format (I4 to CMPR, with palettes) | **1** fit / 1:1, **+** / **-** zoom, D-Pad pan (or previous / next image when it all fits), **A** background, **2** info |
| **Layouts** | BRLYT layouts (banners and icons) with their BRLAN animations | **A** play / pause, D-Pad **Left / Right** step a frame, **Up / Down** previous / next animation, **1** loop, **+** / **-** zoom, **2** info |
| **MIDI** | `.mid`, `.rmi`, `.kar`, and BRSAR sequences (a sequence with a loop loops for ever, as in the game) | Up / Down pick a channel, **A** on / off, **2** solo, **-** all on, **+** pause, **1** repeat (sequences: loop / repeat / play the loop twice and fade), **B** stop |
| **Miis** | A Mii (`.miigx`, `.mii`, `.rcd`, `.rsd`) or a Mii database (`RFL_DB.dat`): each Mii's face, drawn online by Arian Kordi's Mii renderer (as PictoCard shows them), with its name, creator, gender, birthday, favourite colour, height, weight and Mii ID. A database lists its Miis and can export them like **Miis** in the HOME menu | Up / Down pick, **A** mark, **1** mark all, **+** export, **2** fetch the faces again, **B** back |
| **Text** | BMG, MSBT, BREFF (emitters), BRLYT (pane tree and texts), BRLAN (what it animates), Mario Kart Wii KMP | |
| **Video** | Mobiclip `.mo`, THP, Flipnote `.kwz` / `.ppm`, with sound | **A** / **+** pause, **B** stop |

* **Images** open pixel for pixel (1:1) in their real colours. A `.stpl` is unpacked first and shown as the TPL it holds.
* **Monster Games textures** don't store their pixel format, so `.tex` / `.art` / `.img` are decoded in every format that fits and
  the one whose mip levels agree (or whose GX tiles join up) is shown, as nintoolbox and ExciteExtract do: best effort, a few
  percent of files come out in the wrong format. `.tm0` and Excitebots' headed files are exact.
* **Text files** (`.txt`, such as a PDF's pages or an EPUB's chapters) open in the text viewer straight away.
* **A bare MDL0** (opened on its own, not inside its BRRES) gets its textures too: each one is looked up as `<name>.tex0` (with its
  `.plt0`) next to the model, or inside any `.brres` in the same folder. A model opened from inside a BRRES uses that BRRES.
* **Layouts** are drawn with GX: picture and window panes with their textures, texture SRTs and flips, TEV stages, colours,
  alpha compare and blending. A banner plays its Start animation into its Loop one, as on the Wii Menu, and opening a BRLAN
  plays it on its layout. Other animations play once and stop on their last frame (**A** starts them again). Text boxes are not
  drawn, and a layout the viewer can't draw falls back to the text view.
* **MIDI** uses the SoundFont with the same name next to the file (`song.mid` + `song.sf2`), or else a built-in General MIDI set.
  Each channel shows its instrument, note count and level.
* **Audio** covers DSP-ADPCM, AFC and PCM8 / PCM16, plus WAV in PCM, float, IMA / MS ADPCM, A-law, µ-law and RF64, and FLAC in Ogg.

## Other features

**Files and folders**

* Copy, cut, paste and delete between any writable drives, folders included, with progress, speed, overwrite / skip prompts
  and **hold B to cancel**. Half-written files are removed.
* Summaries keep the result line (files, size, time); page through every problem with **L / R** (**-** / **+**).
* Copying from a disc keeps going on read errors: bad sectors become zeros, listed in `<file>.errors.txt`.
* Multi-select, select all / invert, rename, new folder, folder size and info.
* Text viewer, hex viewer, and in-place editors (*Edit as text*, *Edit as hex*). Edits are only written when you save.
* Read-only FAT files and folders can be deleted.

**Installing and launching**

* **Install WADs:** **A** on a `.wad` → *Install to NAND*, or mark several and use *Install marked WADs*. The install and its brick
  checks come from YAWM ModMii Edition. Refused: boot2 and System Menu WADs (use YAWM ModMii Edition itself, which keeps
  Priiloader), Wii IOS on vWii and the other way round, stubs over the IOS of the System Menu, EULA, region select or Homebrew
  Channel, System Menu IOS that would brick a Korean-key Wii, and titles whose IOS is missing or a stub. Needs the Konami code.
* **Install WADs to the SD card (wad2bin):** **A** on a `.wad` → *Install to SD card*, or mark several. The WAD is converted the way
  [wad2bin](https://github.com/DarkMatterCore/wad2bin) does it, with this console's own keys (read from the OTP, so start from the
  Homebrew Channel): a channel becomes `private/wii/title/<ID>/content.bin` and runs from the SD card menu (System Menu 4.0 or
  later) without taking NAND space; DLC (any title, no list of supported games) becomes `private/wii/data/<ID>/<index>.bin`,
  encrypted with the console's key or, for older games through the Disc Channel, a null key. The ticket and TMD then go on the
  NAND (Konami code; a title that is installed already keeps what it has), and a `wad/<title id>_bogus.wad` with just those is
  left for other WAD managers. The files only work on the Wii that made them. A ticket that isn't for this console needs a System
  Menu IOS with the signing bug (a cIOS for DLC), as with wad2bin. Every content is checked against its TMD hash and both
  signatures are verified before the file is kept. System titles (IOS, System Menu …) can't run from the SD card and are refused.
* **Save data is never deleted:** there is no uninstall, and an installed title's `data` folder is first backed up to
  `GodModeWii/save-backups/` and checked again after the install.
* **Launch homebrew:** **A** on a `.dol` or `.elf`. Arguments from a `meta.xml` next to it are passed on, and HW_AHBPROT access is kept.
  Channel contents (`0000000N.app`) are browsed, not launched.

**HOME menu**

* **Export all saves** of every disc game, channel and WiiWare title to `GodModeWii/save-export/<date-time>/<title id> <game id>/`.
* **Miis**: the Wii's Miis with their faces and details, exported as `/savemiis/<Name>.miigx`, the files SaveGame
  Manager GX writes and installs. The faces need the internet: each Mii's 74-byte record (its name and its creator's
  name included) goes to Arian Kordi's Mii renderer, `mii-unsecure.ariankordi.net`, over plain HTTP, and a picture of
  its face comes back. B skips them; the list works without them.
* **FTP server**, **FTP install server** and **Wiiload**; see [Network](#network-ftp-and-wiiload).
* Lock the NAND again, remount drives, credits, exit.
* **Restart Wii**; on a Wii U it returns to the Wii U Menu.

The NAND is only read by the exports.

## Controls

| Button | File browser |
| --- | --- |
| **+** | Copy the marked items (or the one under the cursor) |
| **-** | Cut the marked items |
| **1** | Mark / unmark and move down / clear the clipboard on the `Paste` row |
| **2** | Actions menu (paste, copy, cut, delete, rename, new folder, info, viewers, editors, selection tools …) |
| **A** | Open a folder, archive or file / paste on the green `Paste` row |
| **B** | Up one folder (back to the drive list at the top) |
| D-Pad Left / Right | Page up / down (on the drive list too, when the drives fill more than one page) |
| D-Pad Up / Down | Move (hold to scroll) |
| **HOME** | HOME menu (HOME or B closes it) |

**To copy or move:** copy (**+**) or cut (**-**), browse to the destination and press **A** on the green `Paste` row at the top.
A copy stays in the clipboard so you can paste it in several places; **1** on the `Paste` row empties it.

**Other controllers:**

| Controller | **+** | **-** | **1** | **2** | A / B / HOME |
| --- | --- | --- | --- | --- | --- |
| Classic Controller | ZR, + | ZL, - | Y | X | A / B / HOME |
| GameCube controller | R | L | Y | X | A / B / Start |
| PS3 SIXAXIS / DualShock 3 | Start, R1, R2 | Select, L1, L2 | Square | Triangle | Cross / Circle / PS |
| USB keyboard | C (copy) | V (cut) | X | Y | Enter / Backspace / Esc, arrows to move |
| Wii U GamePad, Wii U Pro Controller | R, ZR, + | L, ZL, - | Y | X | A / B / HOME |

The GamePad, Pro Controller and PS3 pad work in release builds (untested on real hardware).

### Konami code

It appears by itself the first time you try to change the NAND. On the D-Pad, then B and A:

`UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A`

HOME cancels. Once entered, the NAND stays unlocked for the rest of the session.

**Always allow dangerous actions:** enter the same code **while the "Looking for drives..." screen is up** at start-up. That flips
a switch (a `force.cfg` file in `apps/GodModeWii/` on the SD card): from then on every start announces *Risky mode forced on* and
nothing asks for a warning or the code again. Enter the code at start-up once more to turn it off. The reader is forgiving: take
your time (a code that is part way in keeps the screen up for up to 15 seconds), a wrong button only drops what no longer
matches, and extra buttons held at the same time are ignored.

## Network: FTP and Wiiload

All three servers only accept computers on your local network, and speak plain, unencrypted protocols: use them on a network you trust.

### FTP server

HOME → *FTP server (read-only)* or *(read/write)*. The screen shows the address, the user name (`godmode`) and a fresh random
password. Every mounted drive is a folder (`/sd`, `/usb1`, `/nand`, `/disc` …).

It is built to be harder to break and abuse than ftpii:

* **Login required**, random password per run; failed logins are throttled and a burst locks the server for a minute.
  **1** on the server screen turns login off (anyone on your network can then connect) and on again.
* **No FTP bounce:** `PORT` must point back at the client (port ≥ 1024), and passive connections must come from the client.
* **Sandboxed paths:** `..` tricks, odd characters and unmounted drives go nowhere.
* **Read-only by default**; the SEEPROM and disc are always read-only.
* **Same NAND rules:** the first NAND change asks for the Konami code on the console (the FTP command waits).
* **Safe uploads:** written under a temporary name and swapped in when complete, so a dropped connection never leaves a half file.
* `RMD` only removes empty folders: nothing is deleted recursively over the network.
* Bounded buffers, sessions (4) and timeouts. A client that stops reading mid-download holds the server until it times out.

Commands: `LIST`, `NLST`, `MLSD`, `RETR` (with `REST`), `STOR`, `DELE`, `MKD`, `RMD`, `RNFR` / `RNTO`, `SIZE`, `PASV`, `EPSV`, `PORT`.

### FTP install server

HOME → *FTP install server (archives to SD)* or *(archives to USB)*. Upload a **`.zip`, `.7z` or `.rar`** and it is unpacked onto
the drive, then deleted:

| Archive holds | Goes to |
| --- | --- |
| Loose files | `apps/<archive name>/` |
| One program folder (`Foo/boot.dol`) | `apps/Foo/` |
| Only `.wad` files | `wad/` |
| The drive layout (`apps/…`, `wad/…`, `private/…`), with or without a wrapper folder | The drive root (wrapper dropped, top-level docs skipped) |

A failed or cancelled install (hold **B**) keeps the archive. Big 7z / RAR dictionaries need RAM, so an archive can be refused
on low memory.

### Wiiload

HOME → *Wiiload (install apps over the network)* listens on TCP port **4299**, like the Homebrew Channel, so devkitPro's
`wiiload` (or any wiiload 0.4 / 0.5 client) works as is. Pack your app the way the Homebrew Channel wants it, one folder with
the program in it:

```
myapp.zip
└── myapp/          → installed to apps/myapp/
    ├── boot.dol    (or boot.elf)
    ├── meta.xml
    └── icon.png
```

```sh
zip -r myapp.zip myapp
WIILOAD=tcp:192.168.1.23 wiiload myapp.zip
```

It installs to the SD card, or to USB after pressing **1**. What happens depends on what you send:

| You send | What happens |
| --- | --- |
| `.dol` / `.elf` | Kept in `GodModeWii/wiiload/`, and you're offered to start it |
| `.wad` | **WADs can be installed here.** You choose: *to the NAND* (as from the file browser: asks first, then for the Konami code; save data kept and backed up), *to the SD card* (wad2bin, see [Installing and launching](#other-features)), or not at all. Removed from `GodModeWii/wiiload/` once installed, kept there otherwise |
| `.zip` / `.7z` / `.rar` with a program | The folder holding `boot.dol` / `boot.elf` (or another `.dol` / `.elf`) goes to `apps/<folder>/`, wherever it sits in the archive |
| `.zip` / `.7z` / `.rar` without a program (WADs, say) | Placed like the FTP install server does it; then each WAD in it (put in `wad/`) asks the same as a `.wad` |
| Anything else | Kept in `GodModeWii/wiiload/` |

* Extra folders around the app don't matter: `MyApp-v1.2/apps/myapp/boot.dol` still ends up in `apps/myapp/`.
* Files outside the program folder (readmes …) are left out, a zip with several apps installs each one, and a program at the
  top of the zip goes into `apps/<zip name>/`.
* Files are recognised by their contents, not their names. Uploads are unpacked as they arrive (up to 512 MiB, or the drive's
  free space). Hold **B** to cancel.

## Console keys

The `keys:/` drive (original Wii, launched from the Homebrew Channel) shows what [xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod)
dumps, in its layout, so existing tools and [wad2bin](https://github.com/DarkMatterCore/wad2bin) understand the files:

| File | Contents |
| --- | --- |
| `bootmii_keys.bin` | BootMii's `keys.bin` layout |
| `keys.txt` | Every key as hex, in wad2bin's format (`boot1_hash`, `wii_common_key`, `console_id`, `ecc_private_key`, `nand_hmac`, `nand_aes_key`, `prng_key`, `ng_key_id`, `ng_signature`, `wii_korean_key`, `sd_key`) |
| `keys/<name>.bin` | Each key as its own file |
| `otp.bin`, `seeprom.bin`, `device.cert` | Raw dumps |

For xyzzy-mod's layout, copy them into `xyzzy/<console id>/` on your SD card. Not included yet: SD IV, MD5 blanker, MAC address
and the boot0 dump. **These keys are unique to your console. Keep them private.**

## Building

You need [devkitPro](https://devkitpro.org/) with devkitPPC, libogc and libfat, plus these portlibs:

```sh
dkp-pacman -S ppc-libpng ppc-libjpeg-turbo ppc-libwebp ppc-zlib ppc-bzip2 ppc-libvorbisidec ppc-libogg
```

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

This writes `GodModeWii.dol` and `GodModeWii-xz.dol`, the same program as a much smaller self-extracting `.dol`; copy the
latter as `boot.dol`. The first build downloads and builds [dolxz](https://github.com/FIX94/dolxz), which needs a host C compiler
and liblzma (Debian / Ubuntu: `apt-get install liblzma-dev pkg-config`; macOS: `brew install xz`). `make NO_DOLXZ=1` skips it.

### Optional features

Off by default; the CI and release builds turn them all on. Build each library once with its script, then pass the flags:

| Flag | Adds | Build first |
| --- | --- | --- |
| `WITH_EXFAT=1` | exFAT and GPT (devkitPro's libdvm rebuilt with `FF_FS_EXFAT` and `FF_LBA64`, into `portlibs/exfat`) | `scripts/build_exfat_libfat.sh` (git, python3, cmake, network) |
| `WITH_EXT4=1` | ext2 / 3 / 4 (needs `WITH_EXFAT=1`) | `scripts/build_libext4.sh` (git, network) |
| `WITH_NTFS=1` | NTFS | `scripts/build_libntfs.sh` (git, network) |
| `WITH_SICKSAXIS=1` | PS3 SIXAXIS / DualShock 3 | `scripts/build_controller_portlibs.sh` (git, python3, network) |
| `WITH_WIIDRC=1` | Wii U GamePad | `scripts/build_controller_portlibs.sh` (git, python3, network) |
| `WITH_WUPC=1` | Wii U Pro Controller | `scripts/build_controller_portlibs.sh` (git, python3, network) |

```sh
make WITH_WIIDRC=1 WITH_WUPC=1 WITH_SICKSAXIS=1 WITH_EXFAT=1 WITH_NTFS=1 WITH_EXT4=1
```

### Testing in Dolphin

```sh
make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1
scripts/autotest.sh out/ some.brlyt game.rvz::DATA/files/x.brres
```

The test build opens each file listed in `sd:/gmwtest.txt` and saves a screenshot of every viewer; `scripts/autotest.sh` runs it
in Dolphin with those files on a temporary SD card image and turns the screenshots into PNGs. `GFX=Metal` / `Vulkan` / `OGL`
renders the GX viewers, `AUDIO=True` dumps the sound, and `@wiiload` waits for a wiiload upload.

### CI

GitHub Actions builds every push (download the `GodModeWii` artifact: a zip ready to extract to your SD card). Pushing a tag
like `v0.1.0` publishes a GitHub Release with that zip.

## Status and limitations

Developed against devkitPPC / libogc and tested mostly in Dolphin.

**Tried on a Wii U (vWii):** the SD card; FAT32, NTFS (read / write) and ext4 (read-only) USB drives; a Wii U USB drive read with
GodModeU's saved keys; the FTP server; exporting Miis (the Mii faces are tried in Dolphin only).

**Not yet verified on real hardware** (please report what you see): multi-drive / multi-LUN USB and SD Gecko, the SEEPROM / OTP
readers and the Console Keys drive, disc browsing, archives (zip / 7z / RAR, RVZ / WIA, Retro PAK), the model viewer, the
WAV / MP3 / FLAC / Ogg player, and the extra controllers.

**Limitations:**

* **Drives:** logical partitions, extra ext2/3/4 partitions and extra SD card partitions aren't mounted. Files over 4 GiB can't
  be written to FAT32 (exFAT is fine). The font is ASCII only: accented letters lose their accents and other characters show as
  `?` (the files keep their real names).
* **Disc drive:** only the Wii data partition (not the update partition); no raw Wii disc image, but GameCube and unrecognised
  discs can be copied as `disc.iso`. A damaged disc copies slowly while the drive retries. Opening `disc:/` spins the disc up
  once and a bounded number of times: a disc the drive can't read shows "disc not readable" after a few tries and the drive is
  stopped (open `disc:/` again to retry). The same disc isn't read in again when you come back to it.
* **Memory cards:** saves are `<company>-<gamecode>-<name>.gci` (the 0x40-byte directory entry, then the data, as GCMM and Dolphin
  use). To restore, copy `.gci` files onto the card (not `.gcs` / `.sav`): you're asked first, an existing save is never
  replaced (delete it first), and a file is only written once complete and valid. Its identity comes from the header, not the
  file name. No renaming or folders, read-only over FTP. Don't pull the card while it is writing; cards are re-read when you
  open the drive or remount.
* **Archives** are read-only; encrypted and multi-volume archives aren't supported. A RAR5 file has to fit in 48 MiB, and a big
  solid RAR5 can be too large for the Wii's memory. Split WBFS parts (`.wbf1` …) aren't joined. RVZ / WIA chunks are limited
  to 32 MiB (Dolphin's defaults fit). LZ files and IMD5 payloads are unpacked in memory (24 MiB at most). Archives can't be
  opened over FTP.
* **Keys:** the vWii common key (index 2, Wii U era WADs) isn't included. `content.bin` needs this console's keys (OTP, so the
  Homebrew Channel with AHBPROT) and only opens on the console that made it.
* **VFF** is FAT12 / FAT16 with 8.3 names. **`data.bin`** is read the way Dolphin's WiiSave code does it.
* **Disk file systems** (ext, XFS, HFS+, APFS, ZFS) are read-only and read without checking checksums. ext, HFS+ and APFS were
  tested on generated images; **XFS and ZFS are written from the on-disk format and not yet tested against real volumes**. Journals
  are not replayed, so a volume that wasn't unmounted cleanly shows its last checkpoint. Not read: ext compression and encryption,
  HFS+ resource forks and decmpfs-compressed files, APFS snapshots / sealed-volume details / compressed files and symlink targets,
  ZFS RAID-Z, striped, encrypted and big-endian pools, snapshots and zvols, XFS realtime volumes and large extent counts.
* **BootMii NAND:** the file system is read from the newest superblock; without the NAND key (nothing appended, no `keys.bin` /
  `otp.bin` next to the dump, not this console) the folders can be browsed but file contents can't be read. No ECC or HMAC checks.
* **HSD / HSF:** HSD archives show roots and every GX texture, palette ones with their palette (TV no Tomo's palette textures
  hang off the channel's own compact texture records and aren't listed). HSD models show each material's first texture (with
  its UV scale and offset) or its colour, in the bind pose, without animation; a `.dat` with no model in it (a fighter's
  `Pl*.dat`, TV no Tomo's 2D scenes) opens as an archive. HSF models show each material's
  first texture (with its UV scale and offset) or its diffuse colour, without multi-texture blending, skinning or animation.
* **PDF:** pages aren't drawn, only their text and images come out. Text is in drawing order with a line break wherever it moves
  to another line, which reads well for most documents but not for multi-column layouts. Read: FlateDecode (with predictors), LZW,
  ASCIIHex, ASCII85 and RunLength streams, object streams, ToUnicode maps, WinAnsi / MacRoman encodings and glyph-name
  differences; images in grey, RGB, CMYK, ICC-based and indexed colour at 1 – 16 bits. Not read: encrypted documents, JPEG 2000,
  JBIG2 and CCITT images, and text in a composite font without a ToUnicode map (marked in the text). Up to 32 MiB per PDF.
* **EPUB:** the book isn't laid out, only its text comes out: a line break at each block, a blank line after each paragraph and
  heading, list items bulleted, and `[image: name]` where a picture was (find it under `images/`). Books with DRM (an
  `encryption.xml`) can't be read. Up to 32 MiB per book.
* **WFS** is read the way wfslib does (no block hashes checked, areas assumed contiguous, links not shown), tested against
  generated images and one real Wii U drive. Large folders take a moment to list.

## Credits

**GodModeWii is by quatric & cmyksoda.** It also stands on a lot of other people's work. Thank you!

### Code included

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
* **GlowWii** — the controller input layer (Wii Remote / Classic / GameCube pad / USB keyboard mapping) comes from its `main.c`.
* **libwiidrc** and **libwupc** (FIX94) and **libsicksaxis** (xerpi) — Wii U GamePad, Wii U Pro Controller and PS3 pad support; the build script and libwupc compatibility patch use GlowWii's approach.
* **Priiloader** (Dacoslove's `Input.cpp` and the vWii detection) — USB keyboard handling and the vWii check, via GlowWii.
* **[libntfs](https://github.com/extremscorner/libntfs)** by Extrems and contributors, on **NTFS-3G** (GPL-2.0): NTFS drives,
  built by `scripts/build_libntfs.sh`.
* **[lwext4](https://github.com/gkostka/lwext4)** by Grzegorz Kostka (GPL-2.0 / BSD; Extrems' big-endian fork) with Extrems'
  libdvm ext4 driver (ZPL-2.1): ext2/3/4 drives, ported and made read-only by `scripts/build_libext4.sh`.
* **libpng**, **libjpeg-turbo**, **libwebp**, **zlib**, **bzip2**, **Tremor** and **libogg**, as packaged in devkitPro's portlibs — PNG, JPEG and WebP viewing, bzip2 RVZ / WIA images, and Vorbis sound in Mobiclip videos and `.ogg` files.
* **[devkitPro](https://devkitpro.org/)** — devkitPPC, **libogc** (including libdi, wiiuse, ISFS) and **libfat**.

### Formats and references

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
* **Mii faces** — drawn by **Arian Kordi**'s Mii renderer (`mii-unsecure.ariankordi.net`), asked for the way
  **PictoCard** ([pictocard.net](https://pictocard.net)) asks for them. The Mii record fields shown come from WiiBrew's Mii data
  documentation.
* **PDF** — written from the PDF 1.7 specification (ISO 32000-1).
* **EPUB** — written from the EPUB 3.2 / Open Container Format specifications. **GIF** — written from the GIF89a specification.
* **ext2/3/4, XFS, HFS+, APFS, OpenZFS** — written from Apple's APFS reference, Apple Technical Note TN1150 (HFS+), the kernel.org ext4 and XFS on-disk format documents and the OpenZFS on-disk format description; `mke2fs` and macOS's `newfs_hfs` / `hdiutil` produced the test images.
* **[VGMTrans](https://github.com/vgmtrans/vgmtrans)** (zlib licence) — the BRSAR / RSEQ / RBNK / RWAR layouts and NW4R envelope tables that `source/seqsynth.c` plays sequences with.
* **[RiftWii](https://github.com/KakarottoCake/RiftWii)** by KakarottoCake — the design of the RVZ / WIA reader (the disc outside partitions read by disc offset, partition data read decrypted without hashes, the last group kept decoded). No code is taken from it; the reader in `source/rvz.c` is written from **Dolphin**'s `docs/WiaAndRvz.md` format description.
* **[lzo](https://www.oberhumer.com/opensource/lzo/)** / **lzop** by Markus F.X.J. Oberhumer — the LZO1X format and the `.lzo` file layout, decoded by `source/arc_retro.c`, whose PAK layouts and decoder match nintoolbox's.
* **[xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod)** by DarkMatterCore (building on Xyzzy by bushing, libOTP / libSEEPROM by joedj and MINI by Sven Peter, Haxx Enterprises,
  Hector Martin and John Kelley) — the key set, file names and `keys.txt` / `bootmii_keys.bin` formats of the Console Keys drive, and the proven
  OTP and SEEPROM access sequences used here. The hashes used to recognise the SD key come from it too.
* **[wad2bin](https://github.com/DarkMatterCore/wad2bin)** by DarkMatterCore — the `content.bin` and `<index>.bin` layouts (encrypted header, `Bk` header with the included-content bitfield, PRNG-key encrypted contents, the AP certificate and signature), the bogus WAD and how tickets / TMDs are fakesigned, and the IMET / U8 handling. GodModeWii's converter (`source/wad2bin.c`) and its sect233r1 ECDSA (`source/ecc233.c`) are its own code, checked byte for byte against wad2bin's output; MD5 is written from RFC 1321.
* **Sharpii NetCore** / **libWiiSharp** (Leathl and contributors) — WAD, U8, IMD5 and LZ77 (LZ10 / LZ11) layouts.
* **[wfslib / wfs-tools](https://github.com/koolkdev/wfs-tools)** by koolkdev — the WFS layout (areas, directory tries, file layouts, block encryption and key derivation) behind the Wii U volume reader.
* **[vffmod](https://github.com/wii-tools/vffmod)** (wii-tools) and **[vffdump.py](https://marcan.st/transf/vffdump.py)** by marcan — the VFF layout.
* **SaveGame Manager GX** (Dolphin's `WiiSave` code is the reference used for the `data.bin` format) and the **Dolphin** `IOSC` sources — the fixed SD-card key / IV and the Wii common keys.
* **GCMM** (suloku) — GameCube Memory Card Manager, the model for the memory card viewer and its `.gci` backup format; the card access itself uses libogc's CARD API.
* **[WiiBrew](https://wiibrew.org/)** — Hollywood register / GPIO documentation and the SEEPROM layout used for the SEEPROM reader.

### Inspiration

* **[GodMode9](https://github.com/d0k3/GodMode9)** by d0k3 — the look, feel and workflow this tool is modelled on.
* **[GodMode9i](https://github.com/DS-Homebrew/GodMode9i)** by the DS-Homebrew team — the DSi / DS take on the same idea.
* **[ftpii](https://code.google.com/p/ftpii/source/list)** by Joseph Jordan and contributors — the Wii FTP server whose idea the built-in FTP server takes further.
* **[WiiXplorer](https://github.com/niuus/wiixplorer)** by Dimok and contributors, and the niuus fork — the Wii file-manager reference.
* The **Dolphin** team, for the emulator used during development.

## License

GodModeWii is released under the **GNU General Public License v2.0** (see `LICENSE`), the license of the FSToolBox-Mod /
libruntimeiospatch code it includes.

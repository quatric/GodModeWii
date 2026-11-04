# GodModeWii

**Version:** 0.8.0 · **By:** quatric & cmyksoda

A text-mode, [GodMode9](https://github.com/d0k3/GodMode9)-style file manager for the **Wii** and the **Wii U's vWii mode**.
No fancy GUI: just a console, a cursor and a lot of power. Copy, cut, paste and delete files between every storage device
the console can see, watch the free space of each one, and (only if you really mean it) poke around in the NAND.

> ## ⚠️ BRICK RISK
>
> **GodModeWii can permanently brick your console.**
> Writing, deleting or renaming files on the Wii NAND (IOS, System Menu, boot2, tickets, ...) can leave the console
> unbootable, and on a Wii U the vWii NAND is just as fragile.
>
> * Everything that modifies the NAND is **locked by default**. Copying *from* the NAND is always safe.
> * The first time you try to change the NAND (delete, paste, cut, rename, new folder, ...) a warning appears and you must enter the **Konami code**. There is no menu to visit first, and once is enough: it stays unlocked until you exit or lock it again from the HOME menu.
> * **Make a full NAND backup with BootMii before you use risky mode**, and keep a copy of it off the console.
> * This software comes with **no warranty**. You are solely responsible for whatever you do with it.
>
> If you aren't sure what a file does, leave it alone.

## Download

Grab `GodModeWii.zip` from the [latest release](../../releases/latest). Older versions are on the [releases page](../../releases).

1. Extract `GodModeWii.zip` to the root of your SD card (or USB drive).
2. Open GodModeWii from the Homebrew Channel.

**Modifying the NAND can brick your console** - read the README first.

## Features

* One text interface for all your storage, shown with free space and a usage bar in the file browser:
  | Drive | Path | Notes |
  | --- | --- | --- |
  | SD card | `sd:/` | read / write |
  | SD Gecko (slot A / B) | `gca:/`, `gcb:/` | original Wii only |
  | USB drives | `usb1:/` ... `usb8:/` | every USB port and every drive behind a hub, every LUN of card readers (up to 8 volumes) |
  | Wii NAND | `nand:/` | read-only until unlocked with the Konami code |
  | SEEPROM | `seeprom:/` | read-only virtual drive containing `seeprom.bin` |
  | Disc | `disc:/` | read-only: browse the files on a Wii or GameCube disc like any folder, and copy them out. A `sys/` folder holds `boot.bin`, `bi2.bin`, `apploader.img`, `fst.bin` and `main.dol`; GameCube and unrecognised discs also get a raw `disc.iso` |
  | GameCube memory cards | `mca:/`, `mcb:/` | every save as a `.gci` file (GCMM-compatible): copy them out to back up, paste `.gci` files in to restore, delete saves. Original Wii only |
  | Console keys | `keys:/` | read-only: each key as its own file plus the files [xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod) writes (see below) |
* **Open archives like folders** (read-only, extract only, not available over FTP): press **A** on a
  U8 archive (`.arc`, `.app`, `.bnr`, `.carc` ... also with an IMET or IMD5 header), an **LZ10 / LZ11** compressed file (`.lz`,
  `.lz10`, `.lz11`, `.lz77`, or data starting with `LZ77`; a compressed archive opens as the archive inside),
  a **WAD**, a **`content.bin`** (the DLC / WiiWare SD-card backup), a **`data.bin`** (a Wii save backup) or a **VFF** (`.vff`, WiiConnect24 / Mii
  mailboxes). Archives nest (`sd:/x.wad|/00000001.app|/meta/banner.bin`), **B** goes back out, copy anything out
  with **+** / the actions menu. *Open as archive* is also in the actions menu. WAD, `content.bin` and `data.bin` contents are
  decrypted while browsing and copying, vWii-key WADs included; a WAD also shows a `wad_info.txt` (title ID, version, IOS, contents).
* **Install WADs**: press **A** on a `.wad` and pick *Install to NAND* (or *Browse decrypted contents*), or mark several and use
  *Install marked WADs* in the actions menu. The install and its brick checks come from YAWM ModMii Edition: boot2 and System Menu
  WADs are refused (install those with YAWM ModMii Edition itself, which keeps Priiloader), Wii IOS on vWii and vWii IOS on Wii are
  refused, as are stubs over the System Menu's, EULA's, region select's or Homebrew Channel's IOS, System Menu IOS that would brick a
  Korean-key Wii, and titles whose IOS is missing or a stub. **Save data is never deleted**: there is no uninstall, and when a title is
  already installed its `data` folder is first copied to `GodModeWii/save-backups/` on the SD card (or a USB drive) and counted again
  after the install. Needs the Konami code, like any other NAND change.
* **Launch homebrew**: press **A** on a `.dol` or `.elf` and confirm. Arguments from a `meta.xml` next to it are passed on, after the
  program's path, and HW_AHBPROT access is kept when GodModeWii has it.
* **Export all saves** in the HOME menu: copies the save data of every disc game, channel and WiiWare title to
  `GodModeWii/save-export/<date-time>/<title id> <game id>/` on the SD card or a USB drive. The NAND is only read.
* **Audio player** (press **A**): BRSTM, BCSTM, BFSTM, BRWAV, BCWAV, BFWAV, BNS, DSP, AST and BTSND, streamed and decoded on the
  console (DSP-ADPCM, AFC, PCM8 / PCM16), with looping, and the common formats: **WAV** (PCM, float, IMA / MS ADPCM, A-law, µ-law,
  RF64), **AIFF**, **MP3**, **FLAC** (also in Ogg) and **Ogg Vorbis**. **A** pause, **1** loop / repeat on / off, **+** / **-** pick
  channels on multi-track streams, **B** stop.
* **Image viewer** (press **A**): TPL, BTI, TEX0 / BRTEX, BRFNT font sheets, BREFT effect textures, the textures inside J3D models,
  and PNG, JPEG and WebP. Every GX texture format (I4 to CMPR, with palettes). Images open pixel for pixel (1:1) in their real
  colours. **1** scale to fit / back to 1:1, **+** / **-** zoom, D-Pad pans, and when the whole image is on
  screen D-Pad **Left / Right** go to the previous / next image; **A** background, **2** info.
* **3D model viewer** (press **A**): MDL0 models in a `.brres` and J3D `.bmd` / `.bdl`, drawn with GX and lit, with bone / joint
  placement and their textures (each material's first texture, with its wrap mode and cut-out alpha; a missing texture leaves
  the mesh plain). D-Pad rotate, **+** / **-** zoom, **A** lit / unlit / wireframe, **1** next model in the file. *Open as archive* still
  browses a `.brres`.
* **Video player** (press **A**): Mobiclip `.mo`, THP, and Flipnote `.kwz` / `.ppm`, with sound, played straight from any drive,
  the NAND, a disc or inside an archive. **A** / **+** pause, **B** stop.
* **Disc images**: GameCube and Wii `.iso` / `.gcm`, single-file `.wbfs` and `.ciso` open like folders. A Wii disc shows each
  partition (DATA, UPDATE, CHANNEL) with its `ticket.bin`, `tmd.bin`, `sys/` (boot.bin, bi2.bin, apploader.img, main.dol,
  fst.bin) and `files/`, decrypted on the fly; a GameCube disc shows `sys/` and `files/`. Files inside open in the viewers like any
  other (a course `.szs`, a `.thp`, a `.brstm` ...). Split WBFS parts (`.wbf1` ...) are not joined.
  **RVZ** and **WIA** images (Dolphin / wit) open the same way, decompressed on the fly (Zstandard, bzip2, LZMA, LZMA2, purge and
  RVZ packing); chunks are limited to 32 MiB, which covers Dolphin's defaults.
* **zip, 7z and RAR** (1.5 - 5.x) archives open like folders, read-only: files up to 16 MiB are unpacked whole the first time
  they're read, bigger ones are streamed (a RAR5 file has to fit in 48 MiB). Images, models, sounds and archives inside open
  directly. Encrypted and multi-volume archives aren't supported, and a big solid RAR5 can be too large for the Wii's memory.
* **Retro Studios PAK** archives (Metroid Prime 1 - 3, Metroid Prime Trilogy, Donkey Kong Country Returns; compressed resources
  included) and
  **LZO** (`.lzo`, lzop) files open like folders.
* **Text views**: BMG and MSBT messages, BREFF effect sets (their emitter list), BRLYT layouts (canvas, textures, fonts,
  materials and the pane tree with each text box's text), BRLAN animations (length and what they animate) and Mario Kart Wii
  KMP course data (sections, counts, laps).
* **Folders**: **Yaz0 / SZS**, **Yay0**, **ASH0**, the Nintendo SDK codecs (**LZ10**, **LZ11**, **Huffman**, **run length**,
  **LZH8** — recognised by extension or by what they decompress to, like the HOME Menu's compressed files), **RARC**, **BRRES**,
  Virtual Console **CCF**, Super Smash Bros. Brawl **PAC** and Good-Feel **GFA** archives. A **BRSAR** sound archive opens as its groups, with each sequence,
  bank and sound file, and every wave in its wave archives, which the audio player plays. GameCube `.bnr` banners, Wii save
  `banner.bin` files (banner and icon frames) and Retro Studios `.txtr` textures open in the image viewer.
* Everything here is for looking and listening: nothing is converted, exported or repacked (that is nintoolbox's job).
* **ustealth / USBHide drives**: a FAT32 USB drive hidden from the Wii U with ustealth (USBHide) is read and written like any other drive; the home screen notes that it is hidden.
* **exFAT** SD cards and USB drives (release builds), partitioned MBR or GPT (as macOS formats them) or not at all: they mount like FAT32 ones, files over 4 GiB can be written to them, and the home screen shows *exFAT* next to the drive.
* **Restart Wii** in the HOME menu: on a Wii U it leaves vWii and returns to the Wii U menu.
* **Export Miis** in the HOME menu: pick Miis by name and save them to the SD card or a USB drive as
  `/savemiis/<Name>.miigx`, the same files SaveGame Manager GX writes (and can install again). The Mii database is only read.
* Copy / move / delete summaries keep the result line (files, size, time) and let you page through every problem with **L / R** (**-** / **+**). Read-only FAT files and folders can be deleted.
* **NTFS** SD cards and USB drives, read and write (NTFS-3G through extremscorner's libntfs). A drive Windows didn't shut
  down fully (Fast Startup, hibernation) is left alone: shut Windows down fully, then remount. Further NTFS partitions on a
  USB drive are mounted too, each on its own `usbN:` entry.
* **ext2 / ext3 / ext4** (Linux) SD cards and USB drives, **read-only**: copy files off, nothing is written to them.
* **Wii U WFS volumes** (read-only): a USB drive formatted by a Wii U (WFS, which libfat can't mount) shows up as its own
  `usbN:/` drive, and a WFS image file (a dump of a Wii U USB drive or of the MLC) opens like an archive with **A**. WFS is encrypted
  with keys that belong to the Wii U that formatted it, and a Wii or vWii can't read them, so copy them to the SD card first: either the two dumps
  `otp.bin` and `seeprom.bin` of that Wii U, or the finished keys as 16 byte files `usb.key` / `mlc.key`, in `sd:/wfs/` (or the root of the card).
  The easiest way is GodModeU on that Wii U: SYSTEM → *Save Wii U keys for GodModeWii* writes `usb.key` and `mlc.key` to `sd:/wfs/`.
  Decrypted ("plain") images need no key. Remount drives (HOME menu) after changing the key files.
* Copy from a disc keeps going on **read errors**: unreadable sectors are written as zeros and a `<file>.errors.txt` next to the copy lists every
  byte range that could not be read.
* Copy, cut, paste and delete between any two writable devices, folders included, with a progress screen,
  transfer speed, overwrite / skip prompts and **hold B to cancel**. Half-written files are removed again.
* Multi-select, select all / invert, rename, new folder, folder size / info, a text viewer and a hex viewer, plus in-app editors (Actions menu, Edit as text / Edit as hex) for files on writable drives: change bytes in place, or edit text a line at a time with the character roller. Files are edited in memory and only written when you save.
* Konami-code gate (asked once, at the moment you try something risky) and brick warnings for NAND changes.
* Works with the Wii Remote, Classic Controller, GameCube controller and a USB keyboard; release builds also support the Wii U GamePad, Wii U Pro Controller and PS3 SIXAXIS / DualShock 3 (untested on hardware).
* Wii U (vWii) aware: the GameCube-only SD Gecko slots, the SEEPROM and the disc drive are skipped there.
* A built-in **FTP server** (HOME → *FTP server*), a sturdier take on ftpii's — see below.
* No scripts and no WAD installation, on purpose. It's a handy little tool.

## Controls

| Button | File browser |
| --- | --- |
| D-Pad Up / Down | Move (hold to scroll) |
| D-Pad Left / Right | Page up / down |
| **A** | Open folder or archive / actions menu on any other file / paste on the green `Paste` row |
| **B** | Go up one folder (back to the drive list at the top) |
| **1** | Mark / unmark the item and move down / clear the clipboard on the green `Paste` row |
| **2** | Actions menu (paste, clear clipboard, copy, cut, delete, rename, new folder, info, text viewer, hex viewer, edit as text, edit as hex, selection tools) |
| **+** | Copy the marked items (or the item under the cursor) |
| **-** | Cut the marked items |
| **HOME** | System menu: lock risky options again, remount drives, FTP server, credits, exit. HOME or B closes it again. |

Copy or cut something, browse to the destination and press **A** on the green `Paste` row at the top of the list
(or use *Paste* in the actions menu). A copy stays in the clipboard so you can paste it in several places. To empty the
clipboard without pasting, press **1** on the `Paste` row or pick *Clear clipboard* in the actions menu.

Classic Controller: ZR / + = **+**, ZL / - = **-**, X = **2**, Y = **1**. GameCube pad: R = **+**, L = **-**, X = **2**, Y = **1**,
Start = **HOME**. Wii U GamePad / Pro Controller: like the Classic Controller (R/ZR/+ = **+**, L/ZL/- = **-**, X = **2**, Y = **1**).
PS3 pad: Start/R1/R2 = **+**, Select/L1/L2 = **-**, Triangle = **2**, Square = **1**, Cross = A, Circle = B, PS = **HOME**. USB keyboard: arrows, Enter = A, Backspace = B, X = 1, Y = 2, C = copy, V = cut, Esc = HOME.

### Konami code

It appears by itself the first time you try to change the NAND. Enter (D-Pad, then B and A):

`UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A`

HOME cancels. After that the NAND stays unlocked for the rest of the session.

## Console keys (xyzzy-mod compatible)

The `keys:/` drive (original Wii, launched from the Homebrew Channel) exposes what [xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod)
dumps, laid out the way it writes it, so existing tools and [wad2bin](https://github.com/DarkMatterCore/wad2bin) understand the files:

* `keys.txt` — a hex dump of every key in the wad2bin format (`boot1_hash`, `wii_common_key`, `console_id`, `ecc_private_key`,
  `nand_hmac`, `nand_aes_key`, `prng_key`, `ng_key_id`, `ng_signature`, `wii_korean_key`, `sd_key`)
* `otp.bin`, `seeprom.bin`, `bootmii_keys.bin` (BootMii `keys.bin` layout) and `device.cert`
* `keys/<name>.bin` — **each key as its own file**, named like the entries in `keys.txt`

Copy them wherever you like (for xyzzy-mod's layout, into `xyzzy/<console id>/` on your SD card). Not included yet: SD IV, MD5 blanker,
MAC address and the boot0 dump. **These keys are unique to your console. Keep them private.**

## FTP server

HOME → *FTP server (read-only)* or *(read/write)* opens a status screen with the address, the user name (`godmode`) and a
fresh random password. Connect with any FTP client; every mounted drive shows up as a folder (`/sd`, `/usb1`, `/nand`, `/disc`, ...).

It is meant to be harder to break and harder to abuse than the original ftpii:

* **Login is required by default**; the password is random per run. Failed logins are throttled and a burst of them locks the server for a minute.
  **1** on the server screen switches login off for that run (no user name or password; anyone on your network can connect) and back on.
* **LAN only**: peers outside private / link-local ranges are refused.
* **No FTP bounce / hijack**: `PORT` must point back at the client (port >= 1024) and passive data connections must come from the client's IP.
* **Sandboxed paths**: every path is canonicalised inside the virtual root, so `..` tricks, odd characters and unmounted drives go nowhere.
* **Read-only by default**, and the SEEPROM / disc drives are always read-only.
* **Same NAND rules as the browser**: the first NAND change asks for the Konami code on the console itself (the FTP command waits for it).
* **Safe uploads**: files are written to a temporary name and only swapped in when complete (the old file is restored if the swap fails), so a dropped connection can't leave a half-written file.
* `RMD` only removes empty folders; nothing is deleted recursively over the network.
* Single-threaded with bounded buffers, command lines, sessions (4) and timeouts. Downloads go out in short blocking sends once the
  client can take data, because libogc's non-blocking send can lose track of a partly sent block; a client that stops reading in the
  middle of a download holds the server until its connection times out.

### FTP install server

HOME → *FTP install server (archives to SD)* or *(archives to USB)* starts the same server in read/write mode and
unpacks every **`.zip`, `.7z` or `.rar`** (RAR 1.5 - 5.x, single volume, no password) you upload straight onto the SD card
or the first USB drive, then deletes the archive. The contents go where they belong: an archive that already holds the
drive layout (`apps/...`, `wad/...`, `private/...`) is unpacked at the root, a wrapper folder around it is dropped, a single
program folder (`Foo/boot.dol`) goes to `apps/Foo`, and loose files get a folder named after the archive (`.wad` files go to
`wad/`). Docs (`.txt`, `.md`...) at the top level of a ready-made layout are skipped. A failed or cancelled install keeps
the archive; hold **B** to cancel. Big 7z / RAR dictionaries need RAM, so an archive can be refused on low memory.

It speaks plain FTP (no TLS): only use it on a network you trust. Supported commands include `LIST`, `NLST`, `MLSD`, `RETR`
(with `REST`), `STOR`, `DELE`, `MKD`, `RMD`, `RNFR` / `RNTO`, `SIZE`, `PASV`, `EPSV`, `PORT`.

## Installation

Copy `boot.dol` and `meta.xml` to `SD:/apps/GodModeWii/` and start it from the Homebrew Channel.
**Use the Homebrew Channel (IOS58 with HW_AHBPROT disabled)**: that is what gives GodModeWii full NAND permissions and
access to the SEEPROM and the disc drive. Without it you can still use SD / USB, and the NAND is browsable with
limited permissions.

### Wii U (vWii)

Run it from the vWii Homebrew Channel. NAND access works the same way as in FSToolBox-Mod; the SD card and the USB ports
are supported. SD Gecko, memory cards, SEEPROM and the disc drive don't exist from within vWii and are shown as unavailable.
The vWii NAND is as brick-prone as the Wii's, and a bricked vWii may need the Wii U's own recovery tools.

## Building

You need [devkitPro](https://devkitpro.org/) with devkitPPC, libogc and libfat, plus these portlibs:
`dkp-pacman -S ppc-libpng ppc-libjpeg-turbo ppc-libwebp ppc-zlib ppc-bzip2 ppc-libvorbisidec ppc-libogg`.

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

The result is `GodModeWii.dol`. `make` also writes `GodModeWii-xz.dol`, the same program packed into a self-extracting `.dol` a fraction of the size; that is the one to copy as `boot.dol`. The first build downloads and builds [dolxz](https://github.com/FIX94/dolxz), which needs a host C compiler and liblzma (Debian/Ubuntu: `apt-get install liblzma-dev pkg-config`; macOS: `brew install xz`). `make NO_DOLXZ=1` skips this and uses the plain `.dol`.

Optional controller support is off by default. Build the libraries once, then enable what you want:

```sh
./scripts/build_controller_portlibs.sh        # needs git, python3, network
make WITH_WIIDRC=1 WITH_WUPC=1 WITH_SICKSAXIS=1
```

`WITH_WIIDRC` = Wii U GamePad, `WITH_WUPC` = Wii U Pro Controller, `WITH_SICKSAXIS` = PS3 SIXAXIS / DualShock 3. The CI and release builds enable all three.

exFAT support is off by default too. The packaged libfat (devkitPro's libdvm with FatFs) is built with exFAT turned off, so the script
rebuilds the same libdvm release with `FF_FS_EXFAT` and `FF_LBA64` (GPT) on, into `portlibs/exfat`:

```sh
./scripts/build_exfat_libfat.sh             # needs git, python3, cmake, network
make WITH_EXFAT=1
```

The CI and release builds enable it. NTFS and ext2/3/4 are optional in the same way (ext reuses the exFAT build's libdvm, so it needs `WITH_EXFAT=1`):

```sh
./scripts/build_libntfs.sh                  # needs git, network
./scripts/build_libext4.sh                  # needs git, network
make WITH_EXFAT=1 WITH_NTFS=1 WITH_EXT4=1
```

`make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1` builds a test version that opens each file listed in
`sd:/gmwtest.txt` and saves a screenshot of every viewer; `scripts/autotest.sh` runs it in Dolphin with those files on a
temporary SD card image and turns the screenshots into PNGs (`GFX=Vulkan` for real rendering, `AUDIO=True` to dump the sound).

GitHub Actions builds every push (download the `GodModeWii` artifact: a zip ready to extract to the root of your SD card), and pushing a tag like `v0.1.0` publishes a GitHub Release with that zip.

## Status / honesty section

Developed against devkitPPC / libogc and exercised in the Dolphin emulator only. These parts are **not yet verified on real
hardware** and may need fixes — please report what you see:

* multi-drive / multi-LUN USB mounting and SD Gecko,
* the SEEPROM and OTP readers and the Console Keys drive (read-only; the SEEPROM sequence matches xyzzy-mod's),
* browsing the disc (GameCube file table, or the Wii data partition opened through IOS; needs a readable disc).

Running inside vWii has been tried on a Wii U: the SD card, FAT32, NTFS (read / write) and ext4 (read-only) USB drives, a Wii U USB
drive read with GodModeU's saved keys, the FTP server and Export Miis.

Known limitations: on a USB drive with several partitions (MBR or GPT primary partitions), the first one is the drive itself and every further FAT / exFAT / NTFS partition takes the next free `usbN:` slot (listed as "USB Drive 1, part. 2"); logical partitions in an extended partition, extra ext2/3/4 partitions and extra partitions on the SD card aren't mounted, and ext2/3/4 is read-only.
The console font is ASCII only, so accented letters show without their accents and other characters as `?`; the files keep their real names.
Files over 4 GiB can't be written to FAT / FAT32 (exFAT has no such limit).
Memory cards: each save shows as `<company>-<gamecode>-<name>.gci` (the 0x40-byte directory entry followed by the save data, the
format GCMM and Dolphin import and export). Copy them to SD or USB for a backup. To restore, copy `.gci` files onto the card (only `.gci`, not `.gcs` / `.sav`): you are asked to
confirm first, a save that is already on the card is never replaced (delete it first), and a file is only written once it is complete and valid.
The save's identity comes from the `.gci` header, not the file name. Renaming and folders don't exist on cards, and the FTP server keeps them read-only.
Don't pull the card out while it is writing. Cards are re-read every time you open the drive, or with *Remount drives*.

The disc drive shows the Wii data partition only (not the update partition); there is no raw Wii disc image, but GameCube and unrecognised
discs can be copied as `disc.iso`. A disc is only as readable as the Wii's drive makes it: copying a damaged disc is slow while the drive retries.

Archives (zip / 7z / RAR, RVZ / WIA and Retro PAK included), the texture-mapped model viewer and the WAV / MP3 / FLAC / Ogg player are
exercised in Dolphin only. Archives are read-only and **untested on real hardware** and only checked against generated samples on a PC. Limits: LZ files and
IMD5 payloads are decompressed in memory (24 MiB at most), the vWii common key (index 2, used by Wii U era WADs) isn't included, `content.bin` needs this console's
keys (OTP, so Homebrew Channel with AHBPROT) and only opens on the console that made it, and VFF is FAT12 / FAT16 with 8.3 names.
WFS is read the way wfslib does it (no block hashes are checked, areas are assumed to be contiguous, links are not shown) and was tested
against images generated by a script on a PC and one real Wii U USB drive; large directories take a moment to list.
SaveGame Manager GX only covers saves stored in NAND, so a `data.bin` is read the way Dolphin's WiiSave code does.

## Credits

**GodModeWii is by quatric & cmyksoda.** It also stands on a lot of other people's work. Thank you!

* **[YAWM ModMii Edition](https://github.com/modmii/YAWM-ModMii-Edition)** by OverjoY, blackb0x, scooby74029, Peter0x44 and XFlak
  (from Some YAWMM Mod, YAWMM and WAD Manager by Waninkoko; GPL-2.0) — the WAD installer (`source/wadinstall.c`: the ES install
  sequence, the vWii ticket fix-up and every brick check) and the `.dol` / `.elf` launcher (`source/launch.c` and its app booter in
  `appboot/`, with Segher Boessenkool's `crt0.s` / `link.ld`).
* **[nintoolbox](https://github.com/quatric/nintoolbox)** and **[mobipeg](https://github.com/quatric/mobipeg)** (quatric) — the layouts of
  every first-party format the viewers read: BRSTM / BCSTM / BFSTM, BRWAV / BCWAV / BFWAV, BNS, DSP, AST, BTSND, TPL, BTI, TEX0,
  BRFNT, BREFT, BRRES / MDL0, J3D BMD / BDL, BMG, MSBT, BRLYT / BRLAN, KMP, BREFF, BRSAR / RWAR, BNR, WIBN, Retro TXTR,
  Yaz0 / Yay0, ASH0, LZ10 / LZ11, Huffman, run length, LZH8, GFA, CCF, Brawl PAC, RARC, and GameCube / Wii disc images
  (ISO, WBFS, CISO).
* **[wii-mobiclip-player](https://github.com/quatric/wii-mobiclip-player)** (quatric; MIT) — the Mobiclip, THP and Flipnote
  players in `source/mobiclip/` (see its `LICENSE` and `CREDITS_MOBICLIP.md`). Its Mobiclip decoder is ported from FFmpeg's
  (Florian Nouwt, Adib Surani, Paul B Mahol; LGPL 2.1+), with thanks to PlayMobic, MobiclipDecoder and Gericom's x264 fork (Gericom)
  and WiiLink24's FFmpeg fork. THP frames are decoded with NanoJPEG (Martin J. Fiedler) and Vorbis audio with Tremor (portlibs).
* **libpng**, **libjpeg-turbo**, **libwebp**, **zlib**, **bzip2**, **Tremor** and **libogg**, as packaged in devkitPro's portlibs — PNG, JPEG and WebP viewing, bzip2 RVZ / WIA images, and Vorbis sound in Mobiclip videos and `.ogg` files.
* **[VGMTrans](https://github.com/vgmtrans/vgmtrans)** (zlib licence) — the BRSAR / RSEQ / RBNK / RWAR layouts and NW4R envelope tables that `source/seqsynth.c` plays sequences with.
* **[dr_libs](https://github.com/mackron/dr_libs)** by David Reid (public domain / MIT-0, in `source/drlibs`) — `dr_wav`, `dr_mp3` and `dr_flac` decode WAV / AIFF, MP3 and FLAC.
* **[Zstandard](https://github.com/facebook/zstd)** by Meta Platforms (BSD, in `source/zstd`) — the single-file decoder, as vendored by RiftWii, for RVZ.
* **[RiftWii](https://github.com/KakarottoCake/RiftWii)** by KakarottoCake — the design of the RVZ / WIA reader (the disc outside partitions read by disc offset, partition data read decrypted without hashes, the last group kept decoded). No code is taken from it; the reader in `source/rvz.c` is written from **Dolphin**'s `docs/WiaAndRvz.md` format description.
* **[lzo](https://www.oberhumer.com/opensource/lzo/)** / **lzop** by Markus F.X.J. Oberhumer — the LZO1X format and the `.lzo` file layout, decoded by `source/arc_retro.c`, whose PAK layouts and decoder follow nintoolbox's.

* **[GodMode9](https://github.com/d0k3/GodMode9)** by d0k3 — the look, feel and workflow this tool is modelled on.
* **[GodMode9i](https://github.com/DS-Homebrew/GodMode9i)** by the DS-Homebrew team — the DSi / DS take on the same idea.
* **[ftpii](https://code.google.com/p/ftpii/source/list)** by Joseph Jordan and contributors — Wii file-management homebrew that
  inspired this project (no FTP here, but the spirit is the same).
* **[WiiXplorer](https://github.com/niuus/wiixplorer)** by Dimok and contributors, and the niuus fork — the Wii file-manager reference.
* **[xyzzy-mod](https://github.com/DarkMatterCore/xyzzy-mod)** by DarkMatterCore (building on Xyzzy by bushing, libOTP / libSEEPROM by joedj and MINI by Sven Peter, Haxx Enterprises,
  Hector Martin and John Kelley) — the key set, file names and `keys.txt` / `bootmii_keys.bin` formats of the Console Keys drive, and the proven
  OTP and SEEPROM access sequences used here. The hashes used to recognise the SD key come from it too.
* **[FSToolBox-Mod](https://github.com/WiiDatabase/FSToolBox-Mod)** by the WiiDatabase.de team, based on **FSToolBox by nickasa**
  and the **AHBPROT mod by Lupo96** — the approach for NAND access on the Wii and the Wii U's vWii mode
  (staying on the loader's IOS, ISFS permission patch). Parts of its code are included in `source/runtimeiospatch.c/.h`.
* **libruntimeiospatch** by Nano (Christopher Bratusek), with code by Joseph Jordan, damysteryman, DarkMatterCore, megazig
  and FIX94, as shipped in FSToolBox-Mod. GodModeWii adds two tiny single-patch entry points (`IosPatch_ISFS`, `IosPatch_DIReadLimit`).
* **GlowWii** — the controller input layer (Wii Remote / Classic / GameCube pad / USB keyboard mapping) comes from its `main.c`.
* **libwiidrc** and **libwupc** (FIX94) and **libsicksaxis** (xerpi) — Wii U GamePad, Wii U Pro Controller and PS3 pad support; the build script and libwupc compatibility patch follow GlowWii's approach.
* **[wad2bin](https://github.com/DarkMatterCore/wad2bin)** by DarkMatterCore — the `content.bin` layout (encrypted header, `Bk` header with the included-content bitfield, PRNG-key encrypted contents) and the IMET / U8 handling.
* **Sharpii NetCore** / **libWiiSharp** (Leathl and contributors) — WAD, U8, IMD5 and LZ77 (LZ10 / LZ11) layouts.
* **[wfslib / wfs-tools](https://github.com/koolkdev/wfs-tools)** by koolkdev — the WFS layout (areas, directory tries, file layouts, block encryption and key derivation) behind the Wii U volume reader.
* **[unarr](https://github.com/selmf/unarr)** (selmf and contributors, LGPL-3.0, in `source/unarr`, with the LZMA SDK by Igor Pavlov inside) — the zip, 7z and RAR (up to 4.x) readers of the FTP installer and of archive browsing, and the LZMA / LZMA2 decoder used for RVZ; RAR5 is read by `source/rar5.c`, written from the public format description.
* **[vffmod](https://github.com/wii-tools/vffmod)** (wii-tools) and **[vffdump.py](https://marcan.st/transf/vffdump.py)** by marcan — the VFF layout.
* **SaveGame Manager GX** (Dolphin's `WiiSave` code is the reference used for the `data.bin` format) and the **Dolphin** `IOSC` sources — the fixed SD-card key / IV and the Wii common keys.
* **GCMM** (suloku) — GameCube Memory Card Manager, the model for the memory card viewer and its `.gci` backup format; the card access itself uses libogc's CARD API.
* **Priiloader** (Dacoslove's `Input.cpp` and the vWii detection) — USB keyboard handling and the vWii check, via GlowWii.
* **[devkitPro](https://devkitpro.org/)** — devkitPPC, **libogc** (including libdi, wiiuse, ISFS) and **libfat**.
* **[libntfs](https://github.com/extremscorner/libntfs)** by Extrems and contributors, on **NTFS-3G** (GPL-2.0): NTFS drives,
  built by `scripts/build_libntfs.sh`.
* **[lwext4](https://github.com/gkostka/lwext4)** by Grzegorz Kostka (GPL-2.0 / BSD; Extrems' big-endian fork) with Extrems'
  libdvm ext4 driver (ZPL-2.1): ext2/3/4 drives, ported and made read-only by `scripts/build_libext4.sh`.
* **[WiiBrew](https://wiibrew.org/)** — Hollywood register / GPIO documentation and the SEEPROM layout used for the SEEPROM reader.
* The **Dolphin** team, for the emulator used during development.

## License

GodModeWii is released under the **GNU General Public License v2.0** (see `LICENSE`), the license of the FSToolBox-Mod /
libruntimeiospatch code it includes.

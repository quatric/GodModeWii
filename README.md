# GodModeWii

A text-mode file manager for the **Wii** and the **Wii U's vWii mode**, in the style of [GodMode9](https://github.com/d0k3/GodMode9).
No fancy GUI: a console, a cursor and a lot of power.

Version 0.14.0, by quatric & cmyksoda. Looking for the Wii U version? See [GodModeU](https://github.com/quatric/GodModeU).

* Copy, move and delete files between every drive the console can see.
* Open archives, disc images, saves and WADs like folders.
* View images, 3D models, banners and layouts. Play music, sequences and videos.
* Install WADs (to the NAND, an emulated NAND or the SD card) and Wii Menu themes, launch homebrew, and send apps and WADs over
  the network with FTP or `wiiload`.
* Back up installed titles as WADs, saves, and the whole NAND.
* And, only if you really mean it, change the NAND.

> [!CAUTION]
> **GodModeWii can permanently brick your console.** Writing, deleting or renaming files on the NAND (IOS, System Menu, boot2,
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

FAT, FAT32, exFAT and NTFS are read / write. ext2/3/4, HFS+, APFS, XFS, OpenZFS and Wii U WFS are read-only. Details, including
how to read a Wii U drive, are in [docs/DRIVES.md](docs/DRIVES.md).

## What it can open

Press **A** on a file and the viewer is chosen by **what is inside the file**, not by its name. Renamed files still open.

* **Archives and disc images** open like folders, read-only: U8, RARC, BRRES, zip / 7z / RAR, WADs, `content.bin`, `data.bin`,
  GameCube / Wii discs (ISO, WBFS, CISO, RVZ, WIA), BootMii `nand.bin`, disk images and partitions, PDF and EPUB, many game-specific
  archives and compression formats.
* **Viewers and players:** 3D models, images (every GX format, PNG, JPEG, WebP, TGA, BMP, GIF, PSD...), banners and layouts, audio
  (BRSTM, BCSTM, WAV, MP3, FLAC, Ogg, tracker modules...), MIDI and sequences, Miis, text, and video (Mobiclip, THP, Flipnote).

The viewers only look and listen. Nothing is converted or repacked (that's [nintoolbox](https://github.com/quatric/nintoolbox)'s
job). The full list, with each viewer's buttons, is in [docs/FORMATS.md](docs/FORMATS.md).

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
* Images in every GX format, PNG, JPEG, WebP, TGA, BMP, GIF and PSD
* Banners and layouts (BRLYT with BRLAN animations)
* Audio: BRSTM, BCSTM, BFSTM, DSP, WAV, MP3, FLAC, Ogg Vorbis and more
* Fast-forward and rewind in the audio player
* Tracker modules: `.mod`, `.s3m`, `.xm`, `.it`
* MIDI with SoundFonts, and BRSAR game sequences
* Video: Mobiclip, THP and Flipnote, with sound
* Miis, drawn on the Wii, with their details
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
* Boot a Wii game disc with TinyLoad
* Save data is never deleted, and is backed up before a title is installed over

**Backups**

* Back up installed titles as WADs, the way YABDM does
* Back up a `content.bin` from the SD card as a WAD
* Export all saves as folders or `data.bin` files
* Import a `data.bin` save to the NAND
* Back up the whole NAND as a BootMii `nand.bin`, with no BootMii needed
* Back up the console keys only

**Network**

* FTP server, read-only or read / write
* FTP install server for apps and WADs (archives go to the SD card or USB)
* Wiiload for apps, WADs and files
* All of them only accept computers on your local network

**HOME menu extras**

* Miis export (`.miigx`)
* Sketchpad: draw with the pointer, a mouse, a pen tablet or a uDraw GameTablet
* Voice memos with a Wii Speak or a USB microphone
* Clock and calendar
* Alarm clock with a beep and the disc slot light
* Sleep timer with white noise, for 15 to 90 minutes or until you turn it off
* Scientific calculator
* Colour themes
* Sound effects and the disc slot light (each can be turned off)
* A clock in the top bar of the browser
* Lock the NAND again, remount drives, restart, exit

**Safety and input**

* The NAND is locked behind a Konami code, with an optional "always allow" mode
* Wii Remote, Classic Controller, GameCube controller, USB keyboard, PS3 pad, Wii U GamePad and Pro Controller
* Works on a Wii U in vWii mode
* A TiVo sound mode, as an easter egg

The details of each are in [docs/FEATURES.md](docs/FEATURES.md), [docs/FORMATS.md](docs/FORMATS.md),
[docs/DRIVES.md](docs/DRIVES.md) and [docs/NETWORK.md](docs/NETWORK.md).

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

The GamePad, Pro Controller and PS3 pad work in release builds but are untested on real hardware.

### Konami code

It appears by itself the first time you try to change the NAND. On the D-Pad, then B and A:

`UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A`

HOME cancels. Once entered, the NAND stays unlocked for the rest of the session.

**Always allow dangerous actions:** enter the same code while the "Looking for drives..." screen is up at start-up. It says *Risky
mode forced on* once, and from then on nothing asks for a warning or the code again. Enter it at start-up once more to turn it off.
(It's saved as `force.cfg` in `apps/GodModeWii/` on the SD card.) Take your time: a code that is part way in keeps the screen up
for up to 15 seconds.

## Status

Developed against devkitPPC / libogc and tested mostly in Dolphin.

**Tried on a Wii U (vWii):** the SD card; FAT32, NTFS and ext4 USB drives; a Wii U USB drive read with GodModeU's saved keys; the
FTP server; exporting Miis.

**Not yet verified on real hardware** (please report what you see): multi-drive and multi-LUN USB, SD Gecko, the SEEPROM / OTP
readers and the Console Keys drive, disc browsing, archives (zip / 7z / RAR, RVZ / WIA, Retro PAK), the model viewer, the audio
player, and the extra controllers.

Known limits for each area (drives, discs, memory cards, archives, keys, disk file systems, PDF, EPUB...) are in
[docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## Building

You need [devkitPro](https://devkitpro.org/) with devkitPPC, libogc and libfat, plus some portlibs:

```sh
dkp-pacman -S ppc-libpng ppc-libjpeg-turbo ppc-libwebp ppc-zlib ppc-bzip2 ppc-libvorbisidec ppc-libogg
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

This writes `GodModeWii.dol` and a much smaller self-extracting `GodModeWii-xz.dol` (copy it as `boot.dol`). Optional features
(exFAT, ext4, NTFS, extra controllers), testing in Dolphin and CI are covered in [docs/BUILDING.md](docs/BUILDING.md).

## Credits

GodModeWii is by quatric & cmyksoda, and it stands on a lot of other people's work. Thank you!

**MoonShell by Moonlight inspired us a lot.** The interface sound effects are MoonShell 2.10's (GPL-2.0 or later), and the clock,
calendar, calculator and alarm follow its own.

Also modelled on [GodMode9](https://github.com/d0k3/GodMode9) (d0k3), with the NAND approach from FSToolBox-Mod, the WAD installer
and launcher from YAWM ModMii Edition, and format knowledge from nintoolbox. The complete list of code, formats and references
is in [docs/CREDITS.md](docs/CREDITS.md).

## License

GodModeWii is released under the **GNU General Public License v2.0** (see `LICENSE`), the license of the FSToolBox-Mod /
libruntimeiospatch code it includes.

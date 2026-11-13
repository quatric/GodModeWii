# Features

* [Files and folders](#files-and-folders)
* [Installing WADs](#installing-wads)
* [Backing up titles and saves](#backing-up-titles-and-saves)
* [Wii Menu themes](#wii-menu-themes)
* [Launching things](#launching-things)
* [HOME menu](#home-menu)

## Files and folders

* Copy, cut, paste and delete between any writable drives, folders included. You get progress, speed, overwrite / skip
  prompts, and **hold B to cancel**. Half-written files are removed.
* Multi-select, select all / invert, rename, new folder, new empty file, folder size and info.
* Copying from a disc keeps going on read errors. Bad sectors become zeros and are listed in `<file>.errors.txt`.
* **Disk usage** (Actions menu) lists a folder's entries by total size with a share bar. **A** opens a folder, **B** goes up,
  **1** flips size / name order, **2** deletes.
* Text viewer, hex viewer, and in-place editors (*Edit as text*, *Edit as hex*). Edits are only written when you save.
* Read-only FAT files and folders can be deleted.

## Installing WADs

Press **A** on a `.wad` (or mark several). You can install to the NAND, to an emulated NAND, or to the SD card. WADs can also be
installed straight from Wiiload (see [Network](NETWORK.md)).

### To the NAND

Pick *Install to NAND*. The install and its brick checks come from YAWM ModMii Edition. It needs the Konami code. It refuses:

* boot2 and System Menu WADs (use YAWM ModMii Edition, which keeps Priiloader)
* a Wii IOS on vWii and the other way round
* stubs over the IOS of the System Menu, the EULA, region select or the Homebrew Channel
* a System Menu IOS that would brick a Wii with the Korean key
* titles whose IOS is missing or a stub

**Save data is never deleted.** There is no uninstall. A title's `data` folder is backed up to `GodModeWii/save-backups/`
before it is installed over, and checked again afterwards.

### To an emulated NAND

Pick *Install to emulated NAND*. GodModeWii lists the emunands it finds on the SD card or a USB drive (a folder in `nands/`,
or `emunand/`, `nand/`), or makes a new one in `nands/emunand/`.

The contents are decrypted with the ticket's title key and checked against the TMD. They are written as
`title/<hi>/<lo>/content/<id>.app`, with shared contents in `shared1/` and `content.map`, plus `title.tmd`,
`ticket/<hi>/<lo>.tik` and a `sys/uid.sys` entry. That is the layout neek2o, uneek, WiiFlow and Dolphin read. Save data in the
emunand is kept, and the real NAND isn't touched.

### To the SD card (wad2bin)

Pick *Install to SD card*. The WAD is converted the way [wad2bin](https://github.com/DarkMatterCore/wad2bin) does it, using this
console's own keys (read from the OTP, so start from the Homebrew Channel).

* A **channel** becomes `private/wii/title/<ID>/content.bin` and runs from the SD card menu (System Menu 4.0 or later)
  without taking NAND space.
* **DLC** (any title) becomes `private/wii/data/<ID>/<index>.bin`. You choose the encryption: this console's key, or a **null key**
  for older games that read it through the Disc Channel, such as Rock Band 2.
* The game a DLC belongs to is written into each file. Rock Band 3's (`sZJ` to `sZM`) is `SZB`, Guitar Hero: World Tour's `sYO` is
  `SXA`, Guitar Hero 5's `sXF` to `sXH` are `SXE` (as in wad2bin's list). Any other is the DLC's own ID in upper case.
* The ticket and TMD then go on the NAND (Konami code), and a `wad/<title id>_bogus.wad` with just those is left for other WAD
  managers. A title that is already installed keeps what it has.
* Every content is checked against its TMD hash, and both signatures are verified before the file is kept. System titles (IOS,
  System Menu...) can't run from the SD card and are refused.
* The files only work on the Wii that made them. A ticket that isn't for this console needs a System Menu IOS with the signing bug
  (a cIOS for DLC), as with wad2bin.

### DLC WADs with missing pieces, and splitting DLC

A DLC WAD doesn't have to hold every content its TMD lists. Full DLC WADs, ones with pieces missing, and the small WADs that
[dlc-wad-splitter](https://github.com/DarkMatterCore/dlc-wad-splitter) makes all install. GodModeWii finds the contents that are
there by their size and SHA-1 and adds only those. Contents already on the NAND stay, so the small WADs of one DLC can be
installed one after another. Only optional contents (DLC, type `0x4001`) may be missing. A channel or IOS WAD with a content
left out is refused.

To **split** a DLC WAD, pick *Split DLC into small WADs* on a DLC `.wad` (or mark several) and confirm. Each small WAD holds the
TMD as it is (still signed), content 0 and one more content, so song DLC ends up with a song per WAD. The ticket is fakesigned if
it was bound to a console. They are named `<title id>_split_<n>.wad` and saved in `GodModeWii/wad-split/` on the SD card (or a USB
drive). It works on DLC WADs with pieces missing too, and on any DLC title (dlc-wad-splitter only takes the games on its list).

## Backing up titles and saves

### Titles as WADs (YABDM style)

In `nand:/title/<type>/` (or the same folder inside a BootMii `nand.bin`), put the cursor on a title's folder, or mark several, and
pick *Back up as WAD* in the Actions menu. It is also offered inside a title's folder and on a `content.bin` the System Menu wrote to
`private/wii/title/<ID>/`.

The WAD is built the way [Yet Another BlueDump MOD](https://github.com/DarkMatterCore/bluedump-mod) does it: the certificates from
`/sys/cert.sys`, the ticket, the TMD, every content encrypted again with the title key, and the first content as the footer. It
matches a NUS Downloader WAD. A System Menu with Priiloader is dumped from Priiloader's copy of the original.

* The **ticket is always fakesigned** (console ID and ECDH data wiped, a Korean or vWii title key re-encrypted with the Wii common
  key), so the WAD installs on any Wii whose IOS has the signing bug.
* The **TMD is never fakesigned**. It stays exactly as Nintendo signed it.
* Every content is checked against its TMD hash. A title with a damaged or missing content is refused.
* **DLC** asks first: *one WAD* with every part that was downloaded, or *split* into small WADs, one content each (a song each, for
  song DLC), named `<title id>-<ID>-v<version>_split_<n>.wad`.
* A **`content.bin`** is decrypted with the keys of the Wii that made it: this console's, or that Wii's BootMii `keys.bin` (or
  `otp.bin` / `nand.bin`) next to the `content.bin`, at the root of its drive or at the root of the SD card. Its ticket comes from
  this console's NAND or from `<title id>.tik` / `<hi>-<lo>.tik` next to the file, in `GodModeWii/tickets/` or in YABDM's
  `YABDM/Tickets/`.
* Disc games have no ticket on the NAND, and boot2 isn't stored as files, so neither can be backed up.

The WADs go in `GodModeWii/wad-dump/` on the SD card or a USB drive, named `<title id>-<ID>-v<version>.wad`. The NAND is only read.

### Saves

* **Export all saves** (HOME menu): every disc game, channel and WiiWare save goes to `GodModeWii/save-export/<date-time>/`, as plain
  folders, as `data.bin` files, or both. A `data.bin` is the Wii's own encrypted, signed SD card backup, made the way Dolphin makes
  one, so it needs this Wii's keys (start from the Homebrew Channel). Copy the `private` folder to the SD card's root and the Wii
  Menu's Data Management copies the saves back, on this Wii or another. A save without a `banner.bin` is listed as not made.
* **Import a `data.bin`:** press **A** on one → *Import save to the NAND* (or *Browse contents* to look inside). It works with any
  Wii's `data.bin`. The header's MD5 is checked, the save on the NAND is first copied to `GodModeWii/save-backups/`, then replaced,
  and every file gets the owner and permissions stored in the `data.bin`. The title must already have a save folder (install it,
  or start the game once). Needs the Konami code.

### The whole NAND

HOME menu → *Back up the NAND / keys*, using [nanddumper@IOS](https://github.com/Naim2000/nanddumper_ios)'s method.

* *Whole NAND* writes a 1:1 BootMii-format `nand.bin` (about 529 MB) to `/wii/backups/<YYMMDD>_<serial>_nand_<NN>.bin` on the SD
  card or a USB drive, with `<serial>_keys.bin`, a `.sha1` and a line in `sha1sums.txt` beside it. It works on a Wii, a Wii mini and
  vWii, with no patched IOS or BootMii needed. Bad blocks are written as zeros and ECC corrections are counted and shown at the
  end. It takes about 5 minutes; hold B to stop (the unfinished file is deleted).
* *Console keys only* writes just `<serial>_keys.bin` (BootMii's layout: OTP and SEEPROM). Keep both files private.
* IOS stays patched until it is next reloaded, so booting a game disc is refused until GodModeWii is restarted. Not available in
  Dolphin.

## Wii Menu themes

**Dangerous.** Press **A** on a `.csm` or `.mym` → *Install as the Wii Menu theme*. A theme replaces the System Menu's layout
archive on the NAND, and a bad one stops the Wii Menu from starting. The checks of
[csm-installer](https://github.com/Naim2000/csm-installer) apply:

* Priiloader must be installed (it is how you get back to homebrew if the Wii Menu doesn't start).
* The System Menu is identified from its TMD, and the theme must be a U8 archive with the menu's files, this region's
  `iplsetting.ash` and a `main.sel` naming the same System Menu major version and platform (Wii, vWii or Wii mini).
* On vWii the WiiWare 4:3 database is patched for the WiiConnect24 channels, as csm-installer does.

Before anything is written, the stock archive (hash checked against the TMD), or an installed custom theme, is saved to
`GodModeWii/themes/original/` or `GodModeWii/themes/backup/`. No backup, no install. The theme is then written to the NAND's `/tmp`,
read back and hash checked, moved over the old archive and checked again. It asks twice, and for the Konami code. To go back,
install the saved `.csm` the same way. If the archive went missing from the NAND, installing a theme puts one back.

A **`.mym`** (ThemeMii) is first built into a `.csm` from the stock archive, the way ThemeMii and cmyksoda's mym-to-csm do it (for
System Menu 4.3 on a Wii only). The `.csm` is kept in `GodModeWii/themes/` and goes through the same checks. *Build a .csm on the SD
card* does only that, without touching the NAND. Entries whose target isn't in this menu are skipped and listed.

The stock archive comes from `GodModeWii/themes/original/` (or csm-installer's `themes/original_backup/`). With neither, it is
downloaded from Nintendo's update server (it asks first), decrypted with the System Menu's ticket, hash checked, and kept for next
time.

## Launching things

* **Homebrew:** press **A** on a `.dol` or `.elf`. Arguments from a `meta.xml` next to it are passed on, and HW_AHBPROT access is
  kept. Channel contents (`0000000N.app`) are browsed, not launched.
* **Wii game disc:** HOME menu → *Boot game disc* starts the disc in the drive with TinyLoad, region free. GodModeWii closes and the
  game runs. No patches, no updates, no burned discs.

## HOME menu

* **Miis:** the Wii's Miis, drawn on the Wii by MiiKit from the Wii Menu's face models and the Mii Channel's bodies (nothing goes
  online). They are exported as `/savemiis/<Name>.miigx`, the files SaveGame Manager GX writes and installs. **2** changes the
  expression. In Dolphin, turn off Graphics > Hacks > "Store EFB Copies to Texture Only".
* **Sketchpad:** draw with the Wii Remote pointer, a USB mouse, a pen tablet or a uDraw GameTablet (pressure sets the brush width).
  16 colours, four brush sizes, draw / fill / erase, undo and clear. Saved as `.jpg` in `GodModeWii/drawings/`.
* **Voice memos:** record with a Wii Speak or a USB microphone into `GodModeWii/voice/memo_NNN.wav`, then play or delete them.
* **Sound effects** (MoonShell 2.10's) and the **disc slot light** (as in GlowWii). Each can be turned off in HOME and is
  remembered.
* **Clock and calendar**, after MoonShell's: big digits with the seconds and AM / PM, the date, and the month with today marked
  (Left / Right change the month, **A** returns to today). The top bar of the browser shows the time too.
* **Alarm clock**, after MoonShell's Morning Timer: set a time in 12-hour AM / PM (L / R pick hour, minute, AM / PM or on / off, U / D
  change). When it comes, a beep and the disc slot light go off (even with the sounds turned off) until you stop it or snooze for
  five minutes. **Sleep** plays white noise for 15 to 90 minutes, or until you turn it off.
* **Calculator**, after MoonCalc: a scientific calculator with on-screen keys (trig and inverses, ln, log, sqrt, powers, percent, pi,
  e, last answer, DEG / RAD) that the pointer or mouse clicks and the D-Pad moves between.
* **Themes**, after macOS Terminal's default profiles: Basic, Grass, Homebrew, Man Page, Novel, Ocean, Pro, Red Sands and Silver
  Aerogel (or the original colours).
* **Audio player** fast-forward / rewind (WAV, MP3, FLAC, Ogg): Left / Right jump 5 seconds, and held they keep scrubbing.
* The hints along the bottom of a screen ("A Pause", "B Stop"...) can be clicked with the pointer or mouse.
* **FTP server**, **FTP install server** and **Wiiload**: see [Network](NETWORK.md).
* Lock the NAND again, remount drives, credits, exit, and **Restart Wii** (on a Wii U it returns to the Wii U Menu).

Settings are kept on the SD card in `apps/GodModeWii/` (`sfx_off`, `light_off`, `alarm`, `theme`).

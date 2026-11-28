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
* With exactly one file copied (**+**) and the cursor on another file, the Actions menu adds:
  * **Compare with the copied file:** both in hex side by side, the bytes that differ in red; **A** / **1** jump to the next /
    previous difference, and the top line counts them.
  * **Replace with the copied file:** this file's contents become the copied file's; the name and place stay.
  * **Inject the copied file at an offset** (GodMode9's "inject data"): written over this file from a hex offset you type, in
    place. Big files go through a temporary file that is swapped in only once it's whole.
* **Zip** (Actions menu): the marked items, or the one under the cursor, as `<name>.zip` beside them (on the SD card's top
  when that folder can't be written, such as the NAND or a disc). Folders go in with everything in them. Files are deflated,
  except ones that are compressed already (zip, 7z, PNG, JPEG, MP3, RVZ, WBFS...), which are stored. Each file and the zip
  must stay under 4 GB (no ZIP64). Hold **B** to stop; a part-made zip is removed.
* **Title info** (Actions menu, on a `.wad`, `.tmd`, `.tik`, `tmd.*`, `cetk` or a title's folder on the NAND): the title ID and
  name, version, IOS, region, access rights, boot index, every content (ID, index, type, size, SHA-1) and the ticket (title key
  index, console ID, time limits), with whether each signature is real, fakesigned or from a development console.
* **The brick guard:** whatever the Konami code or Risky Mode allows, GodModeWii refuses to delete or overwrite the Wii Menu, its
  IOS, their tickets, `/sys`, `/shared1`, `SYSCONF` and the NAND's top folders: those leave a Wii that can't start.

## Installing WADs

Press **A** on a `.wad` (or mark several). You can install to the NAND, to an emulated NAND, or to the SD card. WADs can also be
installed straight from Wiiload (see [Network](NETWORK.md)).

### To the NAND

Pick *Install to NAND*. The install and its brick checks come from YAWM ModMii Edition. It needs the Konami code. It refuses:

* boot2 and Wii Menu WADs (use YAWM ModMii Edition, which keeps Priiloader)
* a Wii IOS on vWii and the other way round
* stubs over the IOS of the Wii Menu, the EULA, region select or the Homebrew Channel
* a Wii Menu IOS that would brick a Wii with the Korean key
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

* A **channel** becomes `private/wii/title/<ID>/content.bin` and runs from the SD card menu (Wii Menu 4.0 or later)
  without taking NAND space.
* **DLC** (any title) becomes `private/wii/data/<ID>/<index>.bin`. You choose the encryption: this console's key, or a **null key**
  for older games that read it through the Disc Channel, such as Rock Band 2.
* The game a DLC belongs to is written into each file. Rock Band 3's (`sZJ` to `sZM`) is `SZB`, Guitar Hero: World Tour's `sYO` is
  `SXA`, Guitar Hero 5's `sXF` to `sXH` are `SXE` (as in wad2bin's list). Any other is the DLC's own ID in upper case.
* The ticket and TMD then go on the NAND (Konami code), and a `wad/<title id>_bogus.wad` with just those is left for other WAD
  managers. A title that is already installed keeps what it has.
* Every content is checked against its TMD hash, and both signatures are verified before the file is kept. System titles (IOS,
  Wii Menu...) can't run from the SD card and are refused.
* The files only work on the Wii that made them. A ticket that isn't for this console needs a Wii Menu IOS with the signing bug
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
pick *Back up as WAD* in the Actions menu. It is also offered inside a title's folder and on a `content.bin` the Wii Menu wrote to
`private/wii/title/<ID>/`.

The WAD is built the way [Yet Another BlueDump MOD](https://github.com/DarkMatterCore/bluedump-mod) does it: the certificates from
`/sys/cert.sys`, the ticket, the TMD, every content encrypted again with the title key, and the first content as the footer. It
matches a NUS Downloader WAD. A Wii Menu with Priiloader is dumped from Priiloader's copy of the original.

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

HOME menu → *Save manager*, after [SaveGame Manager GX](https://github.com/Cyan-Coding/savegame-manager-gx),
[GCMM](https://github.com/suloku/gcmm) and GerbilSoft's [rom-properties](https://github.com/GerbilSoft/rom-properties). **+**
switches between the Wii NAND, memory card A and B, the `data.bin` files on the SD card / USB drives, and `.gci` files. Every save
is listed with its icon, title and subtitle (or GameCube comment).

* **A** on a save: look at its banner and animated icon, export it (Wii: `data.bin` and/or a plain folder; GameCube: `.gci`), copy
  it to the other memory card, or delete it (Risky Mode; a Wii save is backed up to `GodModeWii/save-backups/` first).
* **A** on a `data.bin` / `.gci`: put it on the NAND / a memory card straight away; there is no "import it into the app first"
  step. A Wii save gets the owner and permissions the `data.bin` stores, the existing save is backed up first, and the header's
  MD5 is checked. It also works for a disc game that has never been started: the title's folder is created the way the Wii Menu
  does it (a `uid.sys` entry, backed up to `GodModeWii/uid.sys.bak` first, and the modes of the other titles' folders), with the
  maker code read from the disc in the drive (or 01). Needs the Konami code.
* **1** / **2**: the game's box / disc art from [GameTDB](https://www.gametdb.com) (downloaded once, kept in `GodModeWii/covers/`).
* **−**: export all Wii saves to `GodModeWii/save-export/<date-time>/` as folders, `data.bin` files, or both. A `data.bin` is the
  Wii's own encrypted, signed SD card backup, made the way Dolphin makes one, so it needs this Wii's keys (start from the Homebrew
  Channel). Copy the `private` folder to the SD card's root and the Wii Menu's Data Management copies the saves back.

### The whole NAND

HOME menu → *Back up the NAND / keys*, using [nanddumper@IOS](https://github.com/Naim2000/nanddumper_ios)'s method.

* *Whole NAND* writes a 1:1 BootMii-format `nand.bin` (about 529 MB) to `/wii/backups/<YYMMDD>_<serial>_nand_<NN>.bin` on the SD
  card or a USB drive, with `<serial>_keys.bin`, a `.sha1` and a line in `sha1sums.txt` beside it. It works on a Wii, a Wii mini and
  vWii, with no patched IOS or BootMii needed. Bad blocks are written as zeros and ECC corrections are counted and shown at the
  end. It takes about 5 minutes; hold B to stop (the unfinished file is deleted).
* *Console keys only* writes just `<serial>_keys.bin` (BootMii's layout: OTP and SEEPROM). Keep both files private.
* IOS stays patched until it is next reloaded, so booting a game disc is refused until GodModeWii is restarted. Not available in
  Dolphin.

### Console

HOME menu → *Console*.

* *Show console info*: the model (from `setting.txt`: Wii, Family Edition, Wii mini, NDEV, or vWii), region, serial number,
  console ID, boot2's version from ES, and the SEEPROM's two boot2 counters (version and update tag).
* *Check boot1 / boot2* reads the boot blocks (0–7) through `/dev/flash`, the way the NAND backup does: boot1's revision (a and b
  can run BootMii as boot2; c and d check boot2's signature), bad blocks and ECC trouble in those blocks, and what each boot2 copy
  is (boot2v1–v5, BootMii, RGD SDBoot or NANDBoot). Only reads. Not in Dolphin or vWii.
* *Edit setting.txt*: AREA, GAME, VIDEO, DVD, MODEL, CODE, SERNO and MPCH, with the known values to pick from. Each value is
  changed in place, the rest of the file stays as it was, and the old file is copied to `GodModeWii/setting-backups/` first
  (copy it back to undo). Needs the Konami code. A wrong region or serial can stop discs, channels or the Wii Shop working.
* *Disc drive stress test* (Risky Mode) reads the disc in the drive for as long as you choose: random seeks, full-stroke seeks
  from the inside to the outside edge, and a stop / spin-up every 200 reads, and counts errors, retries and the slowest read.
  A worn drive shows up as climbing retries. Any Wii or GameCube disc will do.
* *NAND write test* (Risky Mode) writes 4, 16 or 64 MB of a known pattern to `/tmp` on the NAND, reads it back, compares and
  deletes it. Only `/tmp` is touched, but it is still NAND wear.
* *Format a drive as FAT32* is offered only for an SD card or USB drive in a format the Homebrew Channel, RiftWii and the USB
  loaders can't use (exFAT, NTFS, ext, HFS+, WBFS, unreadable). It erases the whole drive (asked twice) and makes one FAT32
  partition from 1 MiB on, with the cluster size Windows would pick; 512- and 4096-byte-sector drives both work.
* *Install / update HBC Reborn* gets [HBC Reborn](https://github.com/Monsterray/hbc-reborn)'s latest release from GitHub and
  replaces the installed Homebrew Channel in place, under its own title ID: LULZ and OHBC from their WAD, an older JODI or HAXX
  from the LULZ WAD given that ID. With none or several installed, it asks. The WAD is checked against the release's
  `SHA256SUMS`, kept in `GodModeWii/hbc-reborn/`, and installed like any WAD (the Konami code, save data kept).
* On a Wii mini, the drive list and the network say that it has no SD card slot or Wi-Fi.

## Wii Menu themes

**Dangerous.** Press **A** on a `.csm` or `.mym` → *Install as the Wii Menu theme*. A theme replaces the Wii Menu's layout
archive on the NAND, and a bad one stops the Wii Menu from starting. The checks of
[csm-installer](https://github.com/Naim2000/csm-installer) apply:

* Priiloader must be installed (it is how you get back to homebrew if the Wii Menu doesn't start).
* The Wii Menu is identified from its TMD, and the theme must be a U8 archive with the menu's files, this region's
  `iplsetting.ash` and a `main.sel` naming the same Wii Menu major version and platform (Wii, vWii or Wii mini).
* On vWii the WiiWare 4:3 database is patched for the WiiConnect24 channels, as csm-installer does.

Before anything is written, the stock archive (hash checked against the TMD), or an installed custom theme, is saved to
`GodModeWii/themes/original/` or `GodModeWii/themes/backup/`. No backup, no install. The theme is then written to the NAND's `/tmp`,
read back and hash checked, moved over the old archive and checked again. It asks twice, and for the Konami code. To go back,
install the saved `.csm` the same way. If the archive went missing from the NAND, installing a theme puts one back.

A **`.mym`** (ThemeMii) is first built into a `.csm` from the stock archive, the way ThemeMii and cmyksoda's mym-to-csm do it (for
Wii Menu 4.3 on a Wii only). The `.csm` is kept in `GodModeWii/themes/` and goes through the same checks. *Build a .csm on the SD
card* does only that, without touching the NAND. Entries whose target isn't in this menu are skipped and listed.

The stock archive comes from `GodModeWii/themes/original/` (or csm-installer's `themes/original_backup/`). With neither, it is
downloaded from Nintendo's update server (it asks first), decrypted with the Wii Menu's ticket, hash checked, and kept for next
time.

## Old WBFS drives

A USB drive (or partition) in the old WBFS format opens read-only: each game shows up as `<ID6> <title>.iso`, and a note says to
copy them off. Nothing current needs WBFS; USB Loader GX, WiiFlow and others read `.wbfs` / `.iso` files from FAT32 or NTFS.

## Launching things

**Favorite apps.** On a `.dol`, an `.elf` or an app's folder (one with `boot.dol` or `boot.elf`), the Actions menu has *Add
to HOME menu favorites*. They are listed by their `meta.xml` name under HOME > Favorite apps (up to 16), and **A** starts one
with its arguments, as from the browser. The same Actions item removes one, as does *Remove a favorite...* in the list. They
are kept in `apps/GodModeWii/favorites.txt` on the SD card.

**Development console programs.** Programs built for a development console (RVT, dev-kit "NDEV" builds) stop on a retail Wii
with Error 002. As an ELF or DOL loads, the check behind it (`cmpwi r0,0 / bne / lis r3,0x8000`) is turned into a plain branch,
so the error never reaches the Wii, and the ELF's physical addresses are fixed and its `.bss` cleared. A development WAD is
converted as it installs: its title key re-encrypted with the retail common key, its ticket and TMD re-issued and fakesigned,
and the Wii's own certificates put in. The dev common key also opens development WADs and RVT-R discs for browsing.

* **Homebrew:** press **A** on a `.dol` or `.elf`. Arguments from a `meta.xml` next to it are passed on, and HW_AHBPROT access is
  kept. Channel contents (`0000000N.app`) are browsed, not launched.
* **Wii game disc:** HOME menu → *Boot game disc* starts the disc in the drive with TinyLoad, region free. GodModeWii closes and the
  game runs. No patches, no updates, no burned discs.
* **Dump a disc to ISO:** HOME menu → *Dump a disc to ISO*. From the Wii's drive: GameCube and Wii discs (dual layer too) and,
  on Wiis made before mid-2008, DVDs; from a USB CD / DVD drive: DVDs and data CDs. The ISO goes to `GodModeWii/dumps/` on the
  SD card or a USB drive: in 4 GB parts (`.part0.iso`, `.part1.iso`, ...) on FAT32, one file on exFAT or NTFS. A block that won't
  read is tried again after a drive reset, then sector by sector until two reads agree (CleanRip's way); a sector that still
  fails is filled with `0x55` and listed in `<name>.log` (offset and drive error), or stops the dump if you ask for that. Dual
  layer is found by itself, and the part size is fixed, so there is nothing to set. DVD-Video is decrypted (CSS) with libdvdcss.
  The `.txt` has the CRC32, MD5 and SHA-1 to check against redump.info. Hold B to stop.

## HOME menu

The menu is in groups: drives and network, discs, saves and backups, console and system, tools, settings, and exit and power.

* **Miis:** first pick where from: the Mii Channel's plaza, the **Mii Parade** (up to 10,000 Miis the Wii has met by mingling,
  on letters or from Wii Remotes; they keep no creator, birthday or favourite), or the ten Miis on **Wii Remote 1-4**. The list
  marks each Mii M or F, and the panel shows its name, creator, gender, birthday, favourite colour, height, weight and ID.
  **-** opens the Mii's actions: **Show as a QR code** (the Mii's 74 bytes, the `.miigx` file, so a phone can carry it), **Save the
  QR code as a PNG** (`GodModeWii/miis/<Name>.png`), **Copy to a Wii Remote** (its first free slot; the Remote's Mii memory is
  saved to `GodModeWii/wiimote-backups/` first, and both of its copies are written and read back), **Put on this Wii** (the plaza
  or the Parade, as an import), **Remove from this Wii Remote**, and **gold pants**. The Wii Remote must be synced in the Wii Menu;
  a Remote whose Mii memory doesn't check out is left alone.
* The Wii's Miis, drawn on the Wii by MiiKit from the Wii Menu's face models and the Mii Channel's bodies (nothing goes
  online). They are exported as `/savemiis/<Name>.miigx`, the files SaveGame Manager GX writes and installs. **2** changes the
  expression. **-** gives the Mii gold pants (a "special" Mii: the Mii Channel won't edit it, and it doesn't mingle) or takes
  them away; that changes its creation ID, so games that saved the Mii no longer know it, and the database is backed up first.
  In Dolphin, turn off Graphics > Hacks > "Store EFB Copies to Texture Only".
* **Sketchpad:** draw with the Wii Remote pointer, a USB mouse, a pen tablet or a uDraw GameTablet (pressure sets the brush width).
  16 colours, four brush sizes, draw / fill / erase, undo and clear. Saved as `.jpg` in `GodModeWii/drawings/`.
* **Voice memos:** record with a Wii Speak or a USB microphone into `GodModeWii/voice/memo_NNN.wav`, then play or delete them.
* **IR remote:** sends TV, receiver, set-top box (and fan, AC, projector...) codes from the sensor bar. **New preset** picks a
  device from `irdb.dat` (all 8,900 of Flipper-IRDB's remotes, by category and manufacturer), then each of its buttons gets a button on any controller: Wii Remote, Nunchuk, Classic, GameCube pad, Pro
  Controller, GamePad or DualShock 3, each by its own buttons, including HOME and the Wii Remote's Power button. A code can
  have several buttons. The buttons are listed A to Z; a name a remote has twice is "Name (2)". **Send it now** tries a code. In the remote, a mapped button sends
  its code and repeats it while held; **HOME + Left / Right** switches preset (the controller buzzes once per preset number and the
  disc slot light blinks, so it works without looking); **HOME** leaves (hold it a second when HOME sends a code). Presets can be renamed, moved and deleted, and are saved
  in `apps/GodModeWii/ir_presets.txt`. Hold **1** (Y on other pads) at start-up to open it straight away. Your own devices and
  buttons go in `apps/GodModeWii/irdb_custom.xml`, made on first use with commented examples to copy; updates never replace it. No sounds play in
  the remote, and the sensor bar is switched back on after each code so the pointer keeps working.
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
* **IR presets** can be deleted several at a time from a checklist. Codes go out the way OptimizeIR's blaster sends them:
  the frame worked out first, then whole carrier cycles timed from the time base, so the pulses don't drift.
* **Set the clock from the internet:** asks pool.ntp.org, time.google.com or time.cloudflare.com (SNTP), guesses the time zone
  from the Wii's own clock to the nearest 15 minutes and lets you change it in 30-minute steps, then sets the Wii's clock (the
  `IPL.CB` bias in `SYSCONF`, backed up to `GodModeWii/SYSCONF.bak` first).
* **Boot BootMii** (when its IOS is installed), **Priiloader's menu** or **the Wii Menu** (when Priiloader is installed, it is
  told which on the way out).
* **Console buttons:** with no controller, Power moves down, Reset is A (held a second: 2; three seconds: HOME), Power while
  holding Reset is B, and ejecting a disc opens HOME. *Settings* turns it on always, with no controller (the default), or off.
* **Screenshots:** **1** and **2** together save the screen as `GodModeWii/screenshots/shot_NNN.png`.
* Lock the NAND again, remount drives, credits, exit, and **Restart Wii** (on a Wii U it returns to the Wii U Menu).

Settings are kept on the SD card in `apps/GodModeWii/` (`sfx_off`, `light_off`, `alarm`, `theme`).

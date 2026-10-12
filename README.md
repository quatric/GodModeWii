# GodModeWii

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
> * To unlock it you must read the disclaimer, enter the **Konami code** and hold **A** for 3 seconds.
> * Even after unlocking, system areas (`/sys`, `/title/00000001`, `/ticket`, `/shared1`, `/shared2`, ...; everything except a short list of save-data / channel folders counts as a system area) ask for another 3-second hold.
> * **Make a full NAND backup with BootMii before you use risky mode**, and keep a copy of it off the console.
> * This software comes with **no warranty**. You are solely responsible for whatever you do with it.
>
> If you aren't sure what a file does, leave it alone.

## Features

* One text interface for all your storage, shown with free space and a usage bar in the file browser:
  | Drive | Path | Notes |
  | --- | --- | --- |
  | SD card | `sd:/` | read / write |
  | SD Gecko (slot A / B) | `gca:/`, `gcb:/` | original Wii only |
  | USB drives | `usb1:/` ... `usb4:/` | every USB port, every drive, every LUN of card readers |
  | Wii NAND | `nand:/` | read-only until unlocked with the Konami code |
  | SEEPROM | `seeprom:/` | read-only virtual drive containing `seeprom.bin` |
  | Disc | `disc:/` | read-only: browse the files on a Wii or GameCube disc like any folder, and copy them out |
* Copy, cut, paste and delete between any two writable devices, folders included, with a progress screen,
  transfer speed, overwrite / skip prompts and **hold B to cancel**. Half-written files are removed again.
* Multi-select, select all / invert, rename, new folder, folder size / info and a hex viewer.
* Konami-code gate and brick warnings for risky NAND operations.
* Works with the Wii Remote, Classic Controller, GameCube controller and a USB keyboard.
* Wii U (vWii) aware: the GameCube-only SD Gecko slots, the SEEPROM and the disc drive are skipped there.
* A built-in **FTP server** (HOME → *FTP server*), a sturdier take on ftpii's — see below.
* No scripts and no WAD installation, on purpose. It's a handy little tool.

## Controls

| Button | File browser |
| --- | --- |
| D-Pad Up / Down | Move (hold to scroll) |
| D-Pad Left / Right | Page up / down |
| **A** | Open folder / actions menu on a file / paste on the green `Paste` row |
| **B** | Go up one folder (back to the drive list at the top) |
| **1** | Mark / unmark the item and move down |
| **2** | Actions menu (paste, copy, cut, delete, rename, new folder, info, hex viewer, selection tools) |
| **+** | Copy the marked items (or the item under the cursor) |
| **-** | Cut the marked items |
| **HOME** | System menu: unlock / lock risky options, remount drives, credits, exit |

Copy or cut something, browse to the destination and press **A** on the green `Paste` row at the top of the list
(or use *Paste* in the actions menu).

Classic Controller: ZR / + = **+**, ZL / - = **-**, X = **2**, Y = **1**. GameCube pad: R = **+**, L = **-**, X = **2**, Y = **1**,
Start = **HOME**. USB keyboard: arrows, Enter = A, Backspace = B, X = 1, Y = 2, C = copy, V = cut, Esc = HOME.

### Konami code

HOME → *Unlock risky options*, then enter (D-Pad, then B and A):

`UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A`

## FTP server

HOME → *FTP server (read-only)* or *(read/write)* opens a status screen with the address, the user name (`godmode`) and a
fresh random password. Connect with any FTP client; every mounted drive shows up as a folder (`/sd`, `/usb1`, `/nand`, `/disc`, ...).

It is meant to be harder to break and harder to abuse than the original ftpii:

* **Login is always required**; the password is random per run. Failed logins are throttled and a burst of them locks the server for a minute.
* **LAN only**: peers outside private / link-local ranges are refused.
* **No FTP bounce / hijack**: `PORT` must point back at the client (port >= 1024) and passive data connections must come from the client's IP.
* **Sandboxed paths**: every path is canonicalised inside the virtual root, so `..` tricks, odd characters and unmounted drives go nowhere.
* **Read-only by default**, and the SEEPROM / disc drives are always read-only.
* **Same NAND rules as the browser**: writes need the Konami unlock, and writes to system areas pop the 3-second hold-A confirmation on the console.
* **Safe uploads**: files are written to a temporary name and only swapped in when complete (the old file is restored if the swap fails), so a dropped connection can't leave a half-written file.
* `RMD` only removes empty folders; nothing is deleted recursively over the network.
* Single-threaded and non-blocking with bounded buffers, command lines, sessions (4) and timeouts, so a stuck client can't hang the console.

It speaks plain FTP (no TLS): only use it on a network you trust. Supported commands include `LIST`, `NLST`, `MLSD`, `RETR`
(with `REST`), `STOR`, `DELE`, `MKD`, `RMD`, `RNFR` / `RNTO`, `SIZE`, `PASV`, `EPSV`, `PORT`.

## Installation

Copy `boot.dol` and `meta.xml` to `SD:/apps/GodModeWii/` and start it from the Homebrew Channel.
**Use the Homebrew Channel (IOS58 with HW_AHBPROT disabled)**: that is what gives GodModeWii full NAND permissions and
access to the SEEPROM and the disc drive. Without it you can still use SD / USB, and the NAND is browsable with
limited permissions.

### Wii U (vWii)

Run it from the vWii Homebrew Channel. NAND access works the same way as in FSToolBox-Mod; the SD card and the USB ports
are supported. SD Gecko, SEEPROM and the disc drive don't exist from within vWii and are shown as unavailable.
The vWii NAND is as brick-prone as the Wii's, and a bricked vWii may need the Wii U's own recovery tools.

## Building

You need [devkitPro](https://devkitpro.org/) with devkitPPC, libogc and libfat:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
make
```

The result is `GodModeWii.dol`. CI builds are produced by `.github/workflows/build.yml`.

## Status / honesty section

Developed against devkitPPC / libogc and exercised in the Dolphin emulator only. These parts are **not yet verified on real
hardware** and may need fixes — please report what you see:

* multi-drive / multi-LUN USB mounting and SD Gecko,
* the SEEPROM bit-banged reader (read-only, touches only the four EEPROM GPIO pins and restores them),
* browsing the disc (GameCube file table, or the Wii data partition opened through IOS; needs a readable disc),
* running inside vWii.

Known limitations: only FAT / FAT32 is supported (libfat), and only the first FAT partition of a drive is mounted.
Files over 4 GiB can't be written to FAT.
The disc drive shows files only (no raw image dumping, by design) and only the Wii data partition, not the update partition.

## Credits

GodModeWii stands on a lot of other people's work. Thank you!

* **[GodMode9](https://github.com/d0k3/GodMode9)** by d0k3 — the look, feel and workflow this tool is modelled on.
* **[GodMode9i](https://github.com/DS-Homebrew/GodMode9i)** by the DS-Homebrew team — the DSi / DS take on the same idea.
* **[ftpii](https://code.google.com/p/ftpii/source/list)** by Joseph Jordan and contributors — Wii file-management homebrew that
  inspired this project (no FTP here, but the spirit is the same).
* **[WiiXplorer](https://github.com/niuus/wiixplorer)** by Dimok and contributors, and the niuus fork — the Wii file-manager reference.
* **[FSToolBox-Mod](https://github.com/WiiDatabase/FSToolBox-Mod)** by the WiiDatabase.de team, based on **FSToolBox by nickasa**
  and the **AHBPROT mod by Lupo96** — the approach for NAND access on the Wii and the Wii U's vWii mode
  (staying on the loader's IOS, ISFS permission patch). Parts of its code are included in `source/runtimeiospatch.c/.h`.
* **libruntimeiospatch** by Nano (Christopher Bratusek), with code by Joseph Jordan, damysteryman, DarkMatterCore, megazig
  and FIX94, as shipped in FSToolBox-Mod. GodModeWii adds two tiny single-patch entry points (`IosPatch_ISFS`, `IosPatch_DIReadLimit`).
* **GlowWii** — the controller input layer (Wii Remote / Classic / GameCube pad / USB keyboard mapping) comes from its `main.c`.
* **Priiloader** (Dacoslove's `Input.cpp` and the vWii detection) — USB keyboard handling and the vWii check, via GlowWii.
* **[devkitPro](https://devkitpro.org/)** — devkitPPC, **libogc** (including libdi, wiiuse, ISFS) and **libfat**.
* **[WiiBrew](https://wiibrew.org/)** — Hollywood register / GPIO documentation and the SEEPROM layout used for the SEEPROM reader.
* The **Dolphin** team, for the emulator used during development.

## License

GodModeWii is released under the **GNU General Public License v2.0** (see `LICENSE`), the license of the FSToolBox-Mod /
libruntimeiospatch code it includes.

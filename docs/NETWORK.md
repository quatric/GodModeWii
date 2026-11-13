# Network: FTP, FTP install and Wiiload

All three servers only accept computers on your local network, and speak plain, unencrypted protocols: use them on a network you trust.

## FTP server

HOME → *FTP server (read-only)* or *(read/write)*. The screen shows the address; there is no user name or password (any is accepted). Every mounted drive is a folder (`/sd`, `/usb1`, `/nand`, `/disc` …).

It is built to be harder to break and abuse than ftpii:

* **No password:** only computers on your local network can connect, so run it on a network you trust.
* **No FTP bounce:** `PORT` must point back at the client (port ≥ 1024), and passive connections must come from the client.
* **Sandboxed paths:** `..` tricks, odd characters and unmounted drives go nowhere.
* **Read-only by default**; the SEEPROM and disc are always read-only.
* **Same NAND rules:** the first NAND change asks for the Konami code on the console (the FTP command waits).
* **Safe uploads:** written under a temporary name and swapped in when complete, so a dropped connection never leaves a half file.
* `RMD` only removes empty folders: nothing is deleted recursively over the network.
* Bounded buffers, sessions (4) and timeouts. A client that stops reading mid-download holds the server until it times out.

Commands: `LIST`, `NLST`, `MLSD`, `RETR` (with `REST`), `STOR`, `DELE`, `MKD`, `RMD`, `RNFR` / `RNTO`, `SIZE`, `PASV`, `EPSV`, `PORT`.

## FTP install server

HOME → *FTP install server (archives to SD)* or *(archives to USB)*. Upload a **`.zip`, `.7z` or `.rar`** and it is unpacked onto
the drive, then deleted:

| Archive holds | Goes to |
| --- | --- |
| Loose files | `apps/<archive name>/` |
| One program folder (`Foo/boot.dol`) | `apps/Foo/` |
| Only `.wad` files | `wad/` |
| The drive layout (`apps/…`, `wad/…`, `private/…`), with or without a wrapper folder | The drive root (wrapper dropped, top-level docs skipped) |

A failed or canceled install (hold **B**) keeps the archive. Big 7z / RAR dictionaries need RAM, so an archive can be refused
on low memory.

## Wiiload

HOME → *Wiiload (receive apps, WADs and files)* listens on TCP port **4299**, like the Homebrew Channel, so devkitPro's
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
| `.wad` | **WADs can be installed here.** You choose: *to the NAND* (as from the file browser: asks first, then for the Konami code; save data kept and backed up), *to the SD card* (wad2bin, see [Installing and launching](FEATURES.md)), or not at all. Removed from `GodModeWii/wiiload/` once installed, kept there otherwise |
| `.zip` / `.7z` / `.rar` with a program | The folder holding `boot.dol` / `boot.elf` (or another `.dol` / `.elf`) goes to `apps/<folder>/`, wherever it sits in the archive |
| `.zip` / `.7z` / `.rar` without a program (WADs, say) | Placed like the FTP install server does it; then each WAD in it (put in `wad/`) asks the same as a `.wad` |
| Anything else | Put at the root of the drive. If a file of that name is already there you're asked whether to replace it; if not, the upload stays in `GodModeWii/wiiload/` |

* Extra folders around the app don't matter: `MyApp-v1.2/apps/myapp/boot.dol` still ends up in `apps/myapp/`.
* Files outside the program folder (readmes …) are left out, a zip with several apps installs each one, and a program at the
  top of the zip goes into `apps/<zip name>/`.
* Files are recognized by their contents, not their names. Uploads are unpacked as they arrive (up to 512 MiB, or the drive's
  free space). Hold **B** to cancel.

# Drives and file systems

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

## File systems

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

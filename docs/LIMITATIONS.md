# Status and limitations

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
  its UV scale and offset) or its color, in the bind pose, without animation; a `.dat` with no model in it (a fighter's
  `Pl*.dat`, TV no Tomo's 2D scenes) opens as an archive. HSF models show each material's
  first texture (with its UV scale and offset) or its diffuse color, without multi-texture blending, skinning or animation.
* **PDF:** pages aren't drawn, only their text and images come out. Text is in drawing order with a line break wherever it moves
  to another line, which reads well for most documents but not for multi-column layouts. Read: FlateDecode (with predictors), LZW,
  ASCIIHex, ASCII85 and RunLength streams, object streams, ToUnicode maps, WinAnsi / MacRoman encodings and glyph-name
  differences; images in gray, RGB, CMYK, ICC-based and indexed color at 1 – 16 bits. Not read: encrypted documents, JPEG 2000,
  JBIG2 and CCITT images, and text in a composite font without a ToUnicode map (marked in the text). Up to 32 MiB per PDF.
* **EPUB:** the book isn't laid out, only its text comes out: a line break at each block, a blank line after each paragraph and
  heading, list items bulleted, and `[image: name]` where a picture was (find it under `images/`). Books with DRM (an
  `encryption.xml`) can't be read. Up to 32 MiB per book.
* **WFS** is read the way wfslib does (no block hashes checked, areas assumed contiguous, links not shown), tested against
  generated images and one real Wii U drive. Large folders take a moment to list.

# Building

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

## Optional features

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

## Testing in Dolphin

```sh
make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1
scripts/autotest.sh out/ some.brlyt game.rvz::DATA/files/x.brres
```

The test build opens each file listed in `sd:/gmwtest.txt` and saves a screenshot of every viewer; `scripts/autotest.sh` runs it
in Dolphin with those files on a temporary SD card image and turns the screenshots into PNGs. `GFX=Metal` / `Vulkan` / `OGL`
renders the GX viewers, `AUDIO=True` dumps the sound, and `@wiiload` waits for a wiiload upload.
`@theme install <path>` / `@theme convert <path>` run the theme installer with every question answered yes (log in
`gmwtest.log`); give it a copy of an emulated NAND with `NAND=~/Library/Application\ Support/Dolphin/Wii` and put files on the
card with `@put:LOCAL:/SDPATH`. `@colors <n>` applies colour theme n (0 is Classic) and saves a sample screen. `scripts/mym_host_test.sh theme.mym menu.app [mym-to-csm]` builds the mym converter for the PC
and compares its csm with mym-to-csm's.

## CI

GitHub Actions builds every push (download the `GodModeWii` artifact: a zip ready to extract to your SD card). Pushing a tag
like `v0.1.0` publishes a GitHub Release with that zip.

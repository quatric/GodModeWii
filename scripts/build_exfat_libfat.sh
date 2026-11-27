#!/usr/bin/env bash
# Build Extrems' libdvm (FatFs with exFAT and GPT, the libfat for libogc2) into ./portlibs/exfat,
# with its headers (fat.h, dvm.h) so they take the place of libogc2's own libfat.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/portlibs/exfat"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
CC="$DEVKITPPC/bin/powerpc-eabi-gcc"
AR="$DEVKITPPC/bin/powerpc-eabi-ar"

git clone --quiet https://github.com/extremscorner/libdvm.git "$WORK/dvm"
git -C "$WORK/dvm" checkout --quiet 39a0c0b
git clone --quiet https://github.com/extremscorner/fatfs-mod.git "$WORK/fatfs"
git -C "$WORK/fatfs" checkout --quiet 6a655ec
# exFAT, and FF_LBA64 for GPT (what macOS uses on external drives)
for opt in FF_FS_EXFAT FF_LBA64; do
    grep -Eq "^#define $opt[[:space:]]+1" "$WORK/fatfs/source/ffconf.h" || { echo "$opt is off in ffconf.h" >&2; exit 1; }
done

mkdir -p "$WORK/obj"
CFLAGS="-O2 -Wall -Wno-unused -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D__wii__"
CFLAGS="$CFLAGS -DLIBDVM_BUFFER_ALIGN=32 -DLIBDVM_WITH_ALIGNED_ACCESS"
CFLAGS="$CFLAGS -I$WORK/dvm/include -I$WORK/dvm/source -I$WORK/fatfs/source -I$DEVKITPRO/libogc2/wii/include"
for src in "$WORK"/dvm/source/{dvm_disc,dvm_cache,dvm_volume,dvm_prober,dvm_libogc2,fat_wrappers,fat_driver}.c \
           "$WORK"/fatfs/source/{ff,ffunicode}.c; do
    "$CC" $CFLAGS -c "$src" -o "$WORK/obj/$(basename "$src" .c).o"
done

rm -rf "$PFX"
mkdir -p "$PFX/lib" "$PFX/include"
"$AR" rcs "$PFX/lib/libfat.a" "$WORK"/obj/*.o
cp "$WORK/dvm/include/fat.h" "$WORK/dvm/include/dvm.h" "$PFX/include/"

echo "Built libfat with exFAT in $PFX"

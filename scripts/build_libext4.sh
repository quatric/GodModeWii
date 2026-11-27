#!/usr/bin/env bash
# Build ext2/3/4 support into ./portlibs/ext4: lwext4 (BSD-3-Clause, its extents / xattr code GPL-2.0-or-later)
# and the ext4 driver of Extrems' libdvm (ZPL-2.1). It links against the libdvm in portlibs/exfat
# (scripts/build_exfat_libfat.sh), so build that first.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/portlibs/ext4"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
CC="$DEVKITPPC/bin/powerpc-eabi-gcc"
AR="$DEVKITPPC/bin/powerpc-eabi-ar"

git clone --quiet https://github.com/extremscorner/libdvm.git "$WORK/dvm"
git -C "$WORK/dvm" checkout --quiet 39a0c0b
git clone --quiet https://github.com/extremscorner/lwext4.git "$WORK/lwext4"
git -C "$WORK/lwext4" checkout --quiet d8c1c4e

mkdir -p "$WORK/obj"
CFG="-DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_BIG_ENDIAN=1 -DCONFIG_UNALIGNED_ACCESS=1 -DCONFIG_EXTENTS_ENABLE=1"
CFG="$CFG -DCONFIG_XATTR_ENABLE=1 -DCONFIG_JOURNALING_ENABLE=1 -DCONFIG_DEBUG_PRINTF=0 -DCONFIG_DEBUG_ASSERT=0"
CFG="$CFG -DCONFIG_HAVE_OWN_ERRNO=0 -DCONFIG_HAVE_OWN_OFLAGS=0 -DCONFIG_HAVE_OWN_ASSERT=0"
CFLAGS="-O2 -Wall -Wno-unused -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D__wii__ -DLIBDVM_BUFFER_ALIGN=32 $CFG"
CFLAGS="$CFLAGS -I$WORK/lwext4/include -I$WORK/dvm/include -I$WORK/dvm/source -I$DEVKITPRO/libogc2/wii/include"
for src in "$WORK"/lwext4/src/*.c "$WORK"/dvm/source/{ext4_driver,ext2_wrappers}.c; do
    "$CC" $CFLAGS -c "$src" -o "$WORK/obj/$(basename "$src" .c).o"
done

rm -rf "$PFX"
mkdir -p "$PFX/lib" "$PFX/include"
"$AR" rcs "$PFX/lib/libext4.a" "$WORK"/obj/*.o
cp "$WORK/dvm/include/ext2.h" "$PFX/include/"

echo "Built libext4 in $PFX"

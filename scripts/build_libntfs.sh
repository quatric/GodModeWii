#!/usr/bin/env bash
# Build extremscorner's libntfs (NTFS-3G 2026.9.28, GPL-2.0) into ./portlibs/ntfs for devkitPro's libogc.
# It's written for libogc2, so its disc calls and SYS_IsDMAAddress are adapted here.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/portlibs/ntfs"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
CC="$DEVKITPPC/bin/powerpc-eabi-gcc"
AR="$DEVKITPPC/bin/powerpc-eabi-ar"

git clone --quiet https://github.com/extremscorner/libntfs.git "$WORK/libntfs"
git -C "$WORK/libntfs" checkout --quiet f73a4d1
SRC="$WORK/libntfs/source"

# libogc2 passes the interface to every disc call and has flush(); devkitPro's libogc does neither.
# devkitPro's discs finish every write before returning, so there's nothing to flush.
sed -i -E 's/->(readSectors|writeSectors)\(((fd|cache)->)?(interface|disc), ?/->\1(/' "$SRC"/*.c
sed -i -E 's/->(startup|isInserted|shutdown)\(interface\)/->\1()/' "$SRC"/*.c
sed -i -E 's/[a-z]+->(interface|disc)->flush\([a-z]+->(interface|disc)\)/true/' "$SRC"/*.c
# Its lock is taken again inside locked sections (stat closes its entry under the lock), so it must be recursive.
sed -i 's/LWP_MutexInit(&vd->lock, false)/LWP_MutexInit(\&vd->lock, true)/' "$SRC"/ntfsinternal.c
grep -q 'LWP_MutexInit(&vd->lock, true)' "$SRC"/ntfsinternal.c || { echo "volume lock not made recursive" >&2; exit 1; }
if grep -nE '->(readSectors|writeSectors|startup|isInserted|shutdown|flush)\(((fd|cache)->)?(interface|disc) ?[,)]|->flush\(' "$SRC"/*.c; then
    echo "libogc2-style disc calls left over" >&2
    exit 1
fi

mkdir -p "$WORK/shim" "$WORK/obj"
cat > "$WORK/shim/ntfs_shim.h" <<'EOF'
#include <stdint.h>
#include <gcbool.h>
#define SYS_IsDMAAddress(p, a) ((((uintptr_t)(p)) & ((a) - 1)) == 0)
/* Only for WSL device files, which a console never makes. */
#ifndef makedev
#define makedev(ma, mi) ((((ma) & 0xfff) << 8) | ((mi) & 0xff))
#define major(d) (((d) >> 8) & 0xfff)
#define minor(d) ((d) & 0xff)
#endif
EOF

CFLAGS="-O2 -Wall -Wno-unused -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D__wii__ -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -include $WORK/shim/ntfs_shim.h -I$WORK/libntfs/include -I$SRC -I$DEVKITPRO/libogc/include"
for src in "$SRC"/*.c; do
    "$CC" $CFLAGS -c "$src" -o "$WORK/obj/$(basename "$src" .c).o"
done

rm -rf "$PFX"
mkdir -p "$PFX/lib" "$PFX/include"
"$AR" rcs "$PFX/lib/libntfs.a" "$WORK"/obj/*.o
cp "$WORK/libntfs/include/ntfs.h" "$PFX/include/"

echo "Built libntfs in $PFX"

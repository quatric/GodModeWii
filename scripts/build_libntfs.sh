#!/usr/bin/env bash
# Build extremscorner's libntfs (NTFS-3G 2026.9.28, GPL-2.0) into ./portlibs/ntfs for libogc2.
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

# Its lock is taken again inside locked sections (stat closes its entry under the lock), so it must be recursive.
sed -i 's/LWP_MutexInit(&vd->lock, false)/LWP_MutexInit(\&vd->lock, true)/' "$SRC"/ntfsinternal.c
grep -q 'LWP_MutexInit(&vd->lock, true)' "$SRC"/ntfsinternal.c || { echo "volume lock not made recursive" >&2; exit 1; }
# statvfs read the whole cluster bitmap every time (64 MB on a 2 TB drive). Like NTFS-3G's own statfs, count it once
# and then trust the running counts it keeps, unless an I/O error left them impossible.
sed -i 's/if (ntfs_volume_get_free_space(vd->vol)) {/if ((!NVolFreeSpaceKnown(vd->vol) || vd->vol->free_clusters < 0 || vd->vol->free_clusters > vd->vol->nr_clusters) \&\& ntfs_volume_get_free_space(vd->vol)) {/' "$SRC"/ntfsdir.c
grep -q 'NVolFreeSpaceKnown(vd->vol) ||' "$SRC"/ntfsdir.c || { echo "statvfs free space patch failed" >&2; exit 1; }
mkdir -p "$WORK/shim" "$WORK/obj"
cat > "$WORK/shim/ntfs_shim.h" <<'EOF'
#include <stdint.h>
/* Only for WSL device files, which a console never makes. */
#ifndef makedev
#define makedev(ma, mi) ((((ma) & 0xfff) << 8) | ((mi) & 0xff))
#define major(d) (((d) >> 8) & 0xfff)
#define minor(d) ((d) & 0xff)
#endif
EOF

CFLAGS="-O2 -Wall -Wno-unused -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D__wii__ -DHAVE_CONFIG_H"
CFLAGS="$CFLAGS -include $WORK/shim/ntfs_shim.h -I$WORK/libntfs/include -I$SRC -I$DEVKITPRO/libogc2/wii/include"
for src in "$SRC"/*.c; do
    "$CC" $CFLAGS -c "$src" -o "$WORK/obj/$(basename "$src" .c).o"
done

rm -rf "$PFX"
mkdir -p "$PFX/lib" "$PFX/include"
"$AR" rcs "$PFX/lib/libntfs.a" "$WORK"/obj/*.o
cp "$WORK/libntfs/include/ntfs.h" "$PFX/include/"

echo "Built libntfs in $PFX"

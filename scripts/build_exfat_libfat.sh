#!/usr/bin/env bash
# Build devkitPro's libfat (libdvm + FatFs) with exFAT and GPT turned on into ./portlibs/exfat.
# The packaged libfat-ogc has FF_FS_EXFAT and FF_LBA64 off; this is the same release with both flipped.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/portlibs/exfat"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
CMAKE="$DEVKITPRO/portlibs/wii/bin/powerpc-eabi-cmake"

# libdvm v2.1.0 is what libfat-ogc 2.1.0 ships; fatfs-mod is the FatFs it fetches.
git clone --quiet https://github.com/devkitPro/libdvm.git "$WORK/libdvm"
git -C "$WORK/libdvm" checkout --quiet 3006b93714
git clone --quiet https://github.com/devkitPro/fatfs-mod.git "$WORK/libdvm/fatfs"
git -C "$WORK/libdvm/fatfs" checkout --quiet 6d3e8f8

python3 - "$WORK/libdvm/fatfs/source/ffconf.h" <<'PY'
import re, sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
# FF_LBA64 is what lets FatFs follow GPT, which macOS uses for external drives.
for opt in ("FF_FS_EXFAT", "FF_LBA64"):
    s, n = re.subn(rf"^(#define {opt}\s+)0$", r"\g<1>1", s, flags=re.M)
    if n != 1:
        sys.exit(f"{opt} not found in ffconf.h")
p.write_text(s)
PY

"$CMAKE" -S "$WORK/libdvm" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
"$CMAKE" --build "$WORK/build" --parallel >/dev/null

mkdir -p "$PFX/lib"
cp "$WORK/build/libfat.a" "$PFX/lib/"

echo "Built libfat with exFAT in $PFX"

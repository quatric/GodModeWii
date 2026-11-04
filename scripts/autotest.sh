#!/bin/sh
# Opens files in GodModeWii's viewers inside Dolphin, with no window or controller, and saves what
# each viewer drew as a PNG.
#
#   scripts/autotest.sh OUTDIR FILE...
#
# Needs: a test build (make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1),
# mtools (mformat, mcopy) and Dolphin. DOLPHIN=/path/to/Dolphin overrides where Dolphin is.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=$1; shift
dol="$here/GodModeWiiTest.dol"
dolphin=${DOLPHIN:-/Applications/Dolphin.app/Contents/MacOS/Dolphin}
[ -f "$dol" ] || { echo "build the test DOL first" >&2; exit 1; }
mkdir -p "$out"
work="$out/.dolphin"
rm -rf "$work"; mkdir -p "$work/user/Config"
sd="$work/sd.img"

dd if=/dev/zero of="$sd" bs=1m count=256 2>/dev/null
mformat -i "$sd" -F ::
: > "$work/gmwtest.txt"
for f in "$@"; do
    name=$(basename "$f")
    mcopy -i "$sd" "$f" "::/$name"
    echo "sd:/$name" >> "$work/gmwtest.txt"
done
mcopy -i "$sd" "$work/gmwtest.txt" ::/gmwtest.txt

cat > "$work/user/Config/Dolphin.ini" <<EOF
[General]
WiiSDCardPath = $sd
[Core]
WiiSDCard = True
WiiSDCardAllowWrites = True
WiiSDCardEnableFolderSync = False
GFXBackend = Null
[Interface]
ConfirmStop = False
UsePanicHandlers = False
[DSP]
Backend = No Audio Output
[Analytics]
Enabled = False
PermissionAsked = True
EOF

"$dolphin" -u "$work/user" -b -e "$dol" -v Null >"$work/dolphin.log" 2>&1 &
pid=$!
i=0
while [ $i -lt 120 ]; do
    sleep 1
    i=$((i + 1))
    if mdir -i "$sd" ::/gmwtest.done >/dev/null 2>&1; then break; fi
done
sleep 1
kill $pid 2>/dev/null || true          # Dolphin asks for a second signal before it stops
sleep 1; kill $pid 2>/dev/null || true
sleep 2; kill -9 $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

i=0
while mcopy -o -i "$sd" "::/shot_$i.yuv" "$work/shot_$i.yuv" 2>/dev/null; do
    n=shot_$i; i=$((i + 1))
    python3 -I "$here/scripts/yuv2png.py" "$work/$n.yuv" "$out/$n.png"
    echo "$out/$n.png"
done

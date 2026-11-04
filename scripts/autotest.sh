#!/bin/sh
# Opens files in GodModeWii's viewers inside Dolphin, with no window or controller, and saves what
# each viewer drew as a PNG.
#
#   scripts/autotest.sh OUTDIR FILE...
#
# FILE::INNER opens a file inside an archive or disc image, e.g. game.rvz::DATA/files/x.brres
# (INNER may itself go through archives: a.rvz::DATA/files/b.szs|/c.brres). SDSIZE sets the
# card's size in MiB (256 by default; make it larger than the files).
#
# Needs: a test build (make AUTOTEST=1 BUILD=build_test TARGET=GodModeWiiTest NO_DOLXZ=1),
# mtools (mformat, mcopy) and Dolphin. DOLPHIN=/path/to/Dolphin overrides where Dolphin is.
# The 3D model viewer draws with the GPU: run it with GFX=Vulkan (or OGL / Metal) so Dolphin renders,
# and writes its frame copies to the emulated RAM the screen is saved from. Null is enough for the rest.
# @wiiload waits for one wiiload upload (send it to tcp:127.0.0.1 while this runs).
# AUDIO=True also saves what was played as OUTDIR/dspdump.wav.
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

dd if=/dev/zero of="$sd" bs=1m count=0 seek="${SDSIZE:-256}" 2>/dev/null
mformat -i "$sd" -F ::
: > "$work/gmwtest.txt"
for arg in "$@"; do
    case "$arg" in @*) echo "$arg" >> "$work/gmwtest.txt"; continue ;; esac   # a command, e.g. @wiiload
    f=${arg%%::*}
    inner=
    case "$arg" in *::*) inner="|/${arg#*::}" ;; esac
    name=$(basename "$f" | tr -c 'A-Za-z0-9._\n-' '_')      # plain names: mtools rewrites some characters
    mdir -i "$sd" "::/$name" >/dev/null 2>&1 || mcopy -i "$sd" "$f" "::/$name"
    echo "sd:/$name$inner" >> "$work/gmwtest.txt"
done
mcopy -i "$sd" "$work/gmwtest.txt" ::/gmwtest.txt

cat > "$work/user/Config/Dolphin.ini" <<EOF
[General]
WiiSDCardPath = $sd
[Core]
WiiSDCard = True
WiiSDCardAllowWrites = True
WiiSDCardEnableFolderSync = False
GFXBackend = ${GFX:-Null}
[Interface]
ConfirmStop = False
UsePanicHandlers = False
[DSP]
Backend = No Audio Output
DumpAudio = ${AUDIO:-False}
DumpAudioSilent = ${AUDIO:-False}
[Analytics]
Enabled = False
PermissionAsked = True
EOF
cat > "$work/user/Config/GFX.ini" <<EOF
[Hacks]
EFBToTextureEnable = False
XFBToTextureEnable = False
ImmediateXFBEnable = False
EOF

"$dolphin" -u "$work/user" -b -e "$dol" -v "${GFX:-Null}" >"$work/dolphin.log" 2>&1 &
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
mcopy -o -i "$sd" ::/gmwtest.log "$out/gmwtest.log" 2>/dev/null && echo "$out/gmwtest.log"
for f in "$work"/user/Dump/Audio/*.wav; do [ -f "$f" ] && cp "$f" "$out/" && echo "$out/$(basename "$f")"; done
exit 0

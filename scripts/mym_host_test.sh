#!/bin/sh
# Builds source/mym.c for the host and converts a mym, optionally comparing the result with
# mym-to-csm's build_csm.py (its own venv):
#   scripts/mym_host_test.sh <theme.mym> <stock menu .app> [path to mym-to-csm]
# Needs a C compiler and libpng (pkg-config or libpng-config).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
mym=$1; app=$2; ref=$3
[ -n "$mym" ] && [ -n "$app" ] || { echo "usage: $0 <theme.mym> <menu.app> [mym-to-csm dir]"; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if pkg-config --exists libpng 2>/dev/null; then
    png_cflags=$(pkg-config --cflags libpng); png_libs=$(pkg-config --libs libpng)
else
    png_cflags=$(libpng-config --cflags); png_libs=$(libpng-config --ldflags)
fi
cc -O2 -Wall -I"$here/source" $png_cflags -o "$work/mym_host" \
    "$here/scripts/mym_host.c" "$here/source/mym.c" "$here/source/ash.c" $png_libs -lm
mkdir "$work/mym"
unzip -q "$mym" -d "$work/mym"
"$work/mym_host" "$work/mym" "$app" "$work/c.csm"
if [ -n "$ref" ]; then
    "$ref/.venv/bin/python" "$ref/build_csm.py" "$mym" "$app" "$work/py.csm" 2>/dev/null
    if cmp -s "$work/c.csm" "$work/py.csm"; then
        echo "identical to build_csm.py's csm"
    else
        echo "DIFFERENT from build_csm.py's csm"; cmp "$work/c.csm" "$work/py.csm" | head -1; exit 1
    fi
fi

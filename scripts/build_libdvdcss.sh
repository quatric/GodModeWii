#!/usr/bin/env bash
# Build VideoLAN's libdvdcss 1.4.3 (GPL-2.0-or-later) into ./portlibs/dvdcss for the disc dumper.
# GodModeWii opens discs with dvdcss_open_stream() and its own reads, so libdvdcss never uses
# the drive's DVD ioctls (it cracks the title keys from the disc instead): they are stubbed here.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/portlibs/dvdcss"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
CC="$DEVKITPPC/bin/powerpc-eabi-gcc"
AR="$DEVKITPPC/bin/powerpc-eabi-ar"

git clone --quiet https://code.videolan.org/videolan/libdvdcss.git "$WORK/dvdcss"
git -C "$WORK/dvdcss" checkout --quiet 1.4.3
SRC="$WORK/dvdcss/src"

mkdir -p "$WORK/inc/sys" "$WORK/obj"
cat > "$WORK/inc/config.h" <<'EOF'
#define HAVE_ERRNO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_UIO_H 1
#define PACKAGE_VERSION "1.4.3"
#define VERSION "1.4.3"
#define WORDS_BIGENDIAN 1
EOF
sed 's/@DVDCSS_VERSION_MAJOR@/1/;s/@DVDCSS_VERSION_MINOR@/4/;s/@DVDCSS_VERSION_MICRO@/3/' \
    "$SRC/dvdcss/version.h.in" > "$WORK/inc/version.h"
cat > "$WORK/inc/sys/uio.h" <<'EOF'
#pragma once
#include <stddef.h>
#include <sys/types.h>
struct iovec { void *iov_base; size_t iov_len; };
ssize_t readv(int fd, const struct iovec *iov, int n);
EOF
# No DVD ioctls on the Wii: every one fails, as on a plain file.
{
    echo '#include <stdint.h>'
    echo '#include <unistd.h>'
    echo '#include <sys/uio.h>'
    echo '#include "ioctl.h"'
    echo '/* only for opening a device by path, which GodModeWii never does */'
    echo 'ssize_t readv(int fd, const struct iovec *iov, int n) { ssize_t t = 0; for (int i = 0; i < n; i++) {'
    echo '    ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len); if (r < 0) return t ? t : r; t += r;'
    echo '    if ((size_t)r < iov[i].iov_len) break; } return t; }'
    for f in 'ReadCopyright(int a, int b, int *c)' 'ReadDiscKey(int a, const int *b, uint8_t *c)' \
             'ReadTitleKey(int a, const int *b, int c, uint8_t *d)' 'ReportAgid(int a, int *b)' \
             'ReportChallenge(int a, const int *b, uint8_t *c)' 'ReportKey1(int a, const int *b, uint8_t *c)' \
             'ReportASF(int a, int *b)' 'InvalidateAgid(int a, int *b)' \
             'SendChallenge(int a, const int *b, const uint8_t *c)' 'SendKey2(int a, const int *b, const uint8_t *c)' \
             'ReportRPC(int a, int *b, int *c, int *d)'; do
        echo "int ioctl_$f { return -1; }"
    done
} > "$WORK/ioctl_none.c"

CFLAGS="-O2 -Wall -Wno-unused -Wno-format-overflow -DO_BINARY=0 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -D__wii__"
CFLAGS="$CFLAGS -I$WORK/inc -I$SRC -I$DEVKITPRO/libogc2/wii/include"
for src in "$SRC"/{libdvdcss,device,css,error}.c "$WORK/ioctl_none.c"; do
    "$CC" $CFLAGS -c "$src" -o "$WORK/obj/$(basename "$src" .c).o"
done

rm -rf "$PFX"
mkdir -p "$PFX/lib" "$PFX/include/dvdcss"
"$AR" rcs "$PFX/lib/libdvdcss.a" "$WORK"/obj/*.o
cp "$SRC/dvdcss/dvdcss.h" "$WORK/inc/version.h" "$PFX/include/dvdcss/"

echo "Built libdvdcss in $PFX"

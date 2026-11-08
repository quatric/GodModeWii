#!/bin/bash
# Builds res/tinyload.elf (TinyLoad 0.2 by Hector Martin, GPL-2.0; https://wiibrew.org/wiki/TinyLoad) with devkitPPC.
# The source here is the release's, unchanged except that main.c is built without -DDEBUG (no USB Gecko output).
set -e
cd "$(dirname "$0")"
B=${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin/powerpc-eabi-
CF=(-mcpu=750 -mno-eabi -mhard-float -DTINY -O2 -Wall -pipe -ffunction-sections -mno-sdata -ffreestanding \
    -Wno-incompatible-pointer-types -std=gnu89)
OUT=$(mktemp -d)
${B}gcc "${CF[@]}" -D_LANGUAGE_ASSEMBLY -DHW_RVL -c crt0.S -o $OUT/crt0.o
for f in ios debug usb utils di cache main eabi_stub; do ${B}gcc "${CF[@]}" -c $f.c -o $OUT/$f.o; done
${B}gcc -mcpu=750 -mhard-float -n -nostartfiles -nostdlib -Wl,-T,tinyload.ld -Wl,--gc-sections \
    $OUT/crt0.o $OUT/ios.o $OUT/debug.o $OUT/usb.o $OUT/utils.o $OUT/di.o $OUT/cache.o $OUT/main.o $OUT/eabi_stub.o \
    -o $OUT/tinyload_sym.elf
${B}strip -o ../res/tinyload.elf $OUT/tinyload_sym.elf
rm -rf $OUT
ls -l ../res/tinyload.elf

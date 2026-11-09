#!/usr/bin/env python3
"""Packs the TiVo interface sounds (the Series 1 - 3 .pcm set) into res/tivo.bin for the TiVo easter egg.

usage: make_tivo.py <folder of .pcm files> <out tivo.bin>

The .pcm files are raw 32 kHz stereo signed 16-bit big-endian, which ASND mixes as they are, so they are
copied untouched. tivo.bin has the layout of sfx.bin (see make_sfx.py): "SFX0", u32 count,
count * { u32 offset, u32 bytes }, then the sounds, each 32-byte aligned. The order is the TV_* list in
source/sfx.c. sine1hz.pcm (a 1 Hz test tone) is left out.
"""
import glob, os, struct, sys

NAMES = ["select", "deselect", "pageup", "pagedown", "updown", "thumbsup", "thumbsdown", "bonk", "error",
         "alert", "tagup", "tivo", "SC_Enter", "SC_Return", "speedup1", "speedup2", "speedup3", "speedup4",
         "slowdown1"]

def find(src, name):
    # the files carry a version suffix: select_2.pcm, SC_Enter_16bit_32kHz_3.pcm
    hits = sorted(glob.glob(os.path.join(src, name + "_*.pcm")) + glob.glob(os.path.join(src, name + ".pcm")))
    if not hits:
        sys.exit("missing %s.pcm in %s" % (name, src))
    return hits[0]

def main():
    src, dst = sys.argv[1], sys.argv[2]
    blobs = []
    for name in NAMES:
        b = open(find(src, name), "rb").read()
        blobs.append(b[:len(b) & ~3])               # whole stereo frames
    head = 8 + 8 * len(blobs)
    off = (head + 31) & ~31
    table, body = b"", b""
    for b in blobs:
        table += struct.pack(">II", off + len(body), len(b))
        body += b + b"\0" * (-len(b) % 32)
    data = b"SFX0" + struct.pack(">I", len(blobs)) + table
    data += b"\0" * (off - len(data)) + body
    open(dst, "wb").write(data)
    print(dst, len(data), "bytes")

main()

#!/usr/bin/env python3
"""Turn a YUY2 framebuffer dump (sd:/shot_N.yuv from an AUTOTEST build) into a PNG.

Decodes the way the Wii's video interface shows it: ITU-R BT.601 with the
16-235 luma / 16-240 chroma ranges.

    yuv2png.py shot_0.yuv shot_0.png [width]
"""
import struct
import sys
import zlib


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v + 0.5)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    data = open(src, "rb").read()
    height = len(data) // (width * 2)
    rows = []
    for y in range(height):
        row = bytearray([0])
        base = y * width * 2
        for x in range(0, width, 2):
            y0, u, y1, v = data[base + x * 2: base + x * 2 + 4]
            for yy in (y0, y1):
                c = 1.164 * (yy - 16)
                row += bytes((clamp(c + 1.596 * (v - 128)),
                              clamp(c - 0.392 * (u - 128) - 0.813 * (v - 128)),
                              clamp(c + 2.017 * (u - 128))))
        rows.append(bytes(row))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR",
                                       struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()

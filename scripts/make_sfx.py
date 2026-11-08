#!/usr/bin/env python3
"""Packs MoonShell 2.10's sound effects (misctools/sndeff/*.wav, GPL v2 or later) into res/sfx.bin.

usage: make_sfx.py <sndeff folder> <out sfx.bin>

Every sound becomes 48 kHz stereo signed 16-bit big-endian (what ASND mixes, so it plays untouched).
sfx.bin: "SFX0", u32 count, count * { u32 offset, u32 bytes }, then the sounds, each 32-byte aligned.
The order is the SFX_* enum in source/sfx.h.
"""
import struct, sys, wave

NAMES = ["snd_click", "snd_open", "snd_notify", "snd_poweroff", "snd_movepage", "snd_longtap",
         "snd_memoerase", "snd_memopendown", "snd_memoundo", "opening"]
OUT_RATE = 48000

def load(path):
    w = wave.open(path, "rb")
    ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    assert width == 1, "8-bit expected"
    fr = [[(raw[i * ch + c] - 128) * 256 for c in range(ch)] for i in range(n)]
    if ch == 1:
        fr = [[s[0], s[0]] for s in fr]
    return fr, rate

def resample(fr, rate):
    n = len(fr) * OUT_RATE // rate
    out = []
    for i in range(n):
        pos = i * rate / OUT_RATE
        a = int(pos)
        b = min(a + 1, len(fr) - 1)
        t = pos - a
        out.append([int(fr[a][c] * (1 - t) + fr[b][c] * t) for c in (0, 1)])
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    blobs = []
    for name in NAMES:
        fr, rate = load("%s/%s.wav" % (src, name))
        fr = resample(fr, rate)
        blobs.append(b"".join(struct.pack(">hh", l, r) for l, r in fr))
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

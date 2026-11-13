#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
"""Reads RiftLinux's panel output captures (RLOF: "RLOF", width, height, then RGBA half floats, sRGB encoded, rows
top down; what goes to the panel after Auto-HDR, before dither) and prints, per capture, the measures RiftLinux uses,
in 8-bit levels: brightest channel, mean of its brightest fifth, share at 255, median, over a region.
With --png it also writes a preview (half size), for looking at what the headset showed.

Usage: rlof.py [--region x,y,w,h] [--png DIR] capture.rlof...
Region default: the whole left eye (left half). Values are clipped to 0..1 for the preview only.
"""
import argparse
import os
import struct
import sys
import zlib

import numpy as np


def read_rlof(path):
    with open(path, "rb") as f:
        magic, width, height = struct.unpack("<III", f.read(12))
        if magic != 0x464F4C52:
            raise ValueError(f"{path}: not an RLOF capture")
        data = np.frombuffer(f.read(width * height * 8), dtype="<f2")
    return data.reshape(height, width, 4).astype(np.float32)


def write_png(path, rgb8):
    height, width, _ = rgb8.shape
    raw = b"".join(b"\x00" + rgb8[y].tobytes() for y in range(height))

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--region", help="x,y,w,h in panel pixels (default: the left eye)")
    parser.add_argument("--png", help="folder for half-size previews")
    parser.add_argument("captures", nargs="+")
    args = parser.parse_args()

    print(f"{'capture':<28} {'size':>10} {'region':>20} {'brightest fifth':>16} {'at 255':>8} {'median':>8} {'lit':>6}")
    for path in args.captures:
        img = read_rlof(path)
        height, width, _ = img.shape
        if args.region:
            x, y, w, h = (int(v) for v in args.region.split(","))
        else:
            x, y, w, h = 0, 0, width // 2, height
        levels = np.clip(img[y : y + h, x : x + w, :3], 0.0, 1.0).max(axis=2).ravel() * 255.0
        lit = levels[levels > 2.0]  # content, not the black around the windows
        if lit.size:
            top = np.sort(lit)[-max(1, lit.size // 5) :]
            stats = f"{top.mean():16.2f} {100.0 * (lit >= 254.5).mean():7.1f}% {np.median(lit):8.1f}"
        else:
            stats = f"{'(black)':>16} {'-':>8} {'-':>8}"
        print(f"{os.path.basename(path):<28} {width:>4}x{height:<5} {f'{x},{y},{w},{h}':>20} {stats} "
              f"{100.0 * lit.size / max(1, levels.size):5.1f}%")
        if args.png:
            os.makedirs(args.png, exist_ok=True)
            small = np.clip(img[::2, ::2, :3], 0.0, 1.0)
            out = os.path.join(args.png, os.path.splitext(os.path.basename(path))[0] + ".png")
            write_png(out, (small * 255.0 + 0.5).astype(np.uint8))
    return 0


if __name__ == "__main__":
    sys.exit(main())

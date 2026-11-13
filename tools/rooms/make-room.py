#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
"""Makes a Rift Home room (a Radiance HDR panorama, data/rooms/*.hdr) from an OpenEXR panorama: scaled to the room's
width by area averaging in linear light (a sharper filter rings around a bright sun), values kept as authored (no
exposure change: ImageMagick's conversion changes them), and optionally the land and water's deepest shadows lifted
below the skyline, the sky untouched. Prints the original's and the room's mean and peak luminance to check them.

Needs Python 3 with numpy, Pillow and OpenEXR (pip install OpenEXR).
Usage: make-room.py in.exr out.hdr [--width 5120] [--lift 0.75 --pivot 0.008 --sky 0.475 --ground 0.495]
  --lift L: shadows below the skyline gain up to 1 + L times (the darkest most; values far above --pivot hardly);
  --sky / --ground: fractions of the height where the lift starts and is full, blended smoothly between.
The twilight lake: --width 5120 --lift 0.75 --pivot 0.008 --sky 0.475 --ground 0.495.
"""
import argparse

import numpy as np
import OpenEXR
from PIL import Image


def read_exr(path):
    with OpenEXR.File(path) as f:
        channels = f.channels()
        group = next((k for k in ("RGB", "RGBA") if k in channels), None)
        if group:
            pixels = np.asarray(channels[group].pixels, dtype=np.float32)
            return [np.ascontiguousarray(pixels[..., i]) for i in range(3)]
        return [np.asarray(channels[c].pixels, dtype=np.float32) for c in ("R", "G", "B")]


def luminance(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def write_rgbe(path, rgb):
    """Flat (not run-length encoded) RGBE scanlines, top row first: what Rift Home's reader takes."""
    h, w = rgb.shape[:2]
    m = rgb.max(axis=-1)
    e = np.where(m > 1e-32, np.floor(np.log2(np.maximum(m, 1e-32))) + 1, 0).astype(np.int32)
    scale = np.where(m > 1e-32, 256.0 / np.exp2(e.astype(np.float64)), 0.0)
    mantissa = np.clip(np.floor(rgb * scale[..., None] + 0.5), 0, 255).astype(np.uint8)
    exponent = np.where(m > 1e-32, e + 128, 0).astype(np.uint8)
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {h} +X {w}\n".encode())
        f.write(np.concatenate([mantissa, exponent[..., None]], axis=-1).tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exr")
    ap.add_argument("hdr")
    ap.add_argument("--width", type=int, default=5120, help="the room's width (height: half); Rift Home shows up to 5120")
    ap.add_argument("--lift", type=float, default=0.0, help="shadow lift below the skyline (0: none)")
    ap.add_argument("--pivot", type=float, default=0.008, help="luminance the lift fades out above")
    ap.add_argument("--sky", type=float, default=0.475, help="where the lift starts (fraction of the height)")
    ap.add_argument("--ground", type=float, default=0.495, help="where the lift is full (fraction of the height)")
    a = ap.parse_args()

    planes = read_exr(a.exr)
    h, w = planes[0].shape
    print(f"original {w}x{h}: mean luminance {luminance(*planes).mean():.5f}, peak {luminance(*planes).max():.2f}")
    size = (a.width, a.width // 2)
    scaled = []
    for p in planes:
        p = np.clip(np.nan_to_num(p, nan=0.0, posinf=0.0), 0.0, None)
        scaled.append(np.asarray(Image.fromarray(p).resize(size, Image.Resampling.BOX), dtype=np.float32))
    rgb = np.clip(np.stack(scaled, axis=-1), 0.0, None)
    del planes, scaled
    if a.lift > 0.0:
        h = rgb.shape[0]
        y = (np.arange(h, dtype=np.float32) + 0.5) / h
        t = np.clip((y - a.sky) / (a.ground - a.sky), 0.0, 1.0)
        weight = (t * t * (3.0 - 2.0 * t))[:, None]  # 0 in the sky, 1 on the ground, smooth between
        lum = np.maximum(luminance(rgb[..., 0], rgb[..., 1], rgb[..., 2]), 1e-9)
        rgb = rgb * (1.0 + weight * a.lift * a.pivot / (lum + a.pivot))[..., None]
    write_rgbe(a.hdr, rgb)
    lum = luminance(rgb[..., 0], rgb[..., 1], rgb[..., 2])
    print(f"room {rgb.shape[1]}x{rgb.shape[0]}: mean luminance {lum.mean():.5f}, peak {lum.max():.2f}")


if __name__ == "__main__":
    main()

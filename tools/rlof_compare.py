#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
"""Compares two RiftLinux panel output captures (RLOF) pixel by pixel, in 8-bit levels of the sRGB-encoded output:
over the pixels lit in either capture, the mean and 99th percentile of the absolute difference per channel, the
largest difference, and the share of pixels more than 1 level apart. Writes a difference image with --png (x8).

Usage: rlof_compare.py a.rlof b.rlof [--png diff.png]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rlof import read_rlof, write_png  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--png")
    args = parser.parse_args()
    a = np.clip(read_rlof(args.a)[..., :3], 0.0, 1.0) * 255.0
    b = np.clip(read_rlof(args.b)[..., :3], 0.0, 1.0) * 255.0
    if a.shape != b.shape:
        print(f"different sizes: {a.shape} vs {b.shape}")
        return 1
    lit = (a.max(axis=2) > 2.0) | (b.max(axis=2) > 2.0)
    diff = np.abs(a - b)
    d = diff[lit]
    print(f"lit pixels {lit.sum()} ({100.0 * lit.mean():.1f} %), lit in a only {((a.max(axis=2) > 2) & ~(b.max(axis=2) > 2)).sum()}, "
          f"in b only {((b.max(axis=2) > 2) & ~(a.max(axis=2) > 2)).sum()}")
    print(f"abs difference (8-bit levels): mean {d.mean():.3f}, p99 {np.percentile(d, 99):.2f}, max {d.max():.2f}, "
          f"pixels > 1 level apart {100.0 * (d.max(axis=1) > 1.0).mean():.2f} %")
    if args.png:
        write_png(args.png, np.clip(diff[::2, ::2] * 8.0, 0, 255).astype(np.uint8))
    return 0


if __name__ == "__main__":
    sys.exit(main())

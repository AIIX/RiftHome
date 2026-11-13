#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
"""Converts Rift Home's debug window dumps (RHWD: "RHWD", width, height, stride, DRM format, then the rows) to PNG.
Usage: rhwd.py dump.rhwd... (writes dump.png next to each)"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rlof import write_png  # noqa: E402

for path in sys.argv[1:]:
    with open(path, "rb") as f:
        magic, width, height, stride, fmt = struct.unpack("<5I", f.read(20))
        rows = np.frombuffer(f.read(stride * height), dtype=np.uint8).reshape(height, stride)
    px = rows[:, : width * 4].reshape(height, width, 4)
    name = struct.pack("<I", fmt).decode("ascii", "replace")
    rgb = px[..., [2, 1, 0]] if name in ("XR24", "AR24") else px[..., :3]  # little-endian BGRx vs RGBx
    out = os.path.splitext(path)[0] + ".png"
    write_png(out, np.ascontiguousarray(rgb))
    print(f"{path}: {width}x{height} {name} -> {out}")

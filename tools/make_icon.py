#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 FruitSlicer contributors
"""Draws the FruitSlicer app icon (the rail logo: a citrus slice) and packs
it as a macOS .icns. Usage: make_icon.py <out_dir>  (needs Pillow)."""
import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ACCENT = (255, 143, 63, 255)   # #FF8F3F
DARK = (14, 16, 20, 255)       # #0E1014
BG_TOP = (31, 35, 43, 255)     # #1F232B
BG_BOTTOM = (17, 19, 23, 255)  # #111317


def draw(size: int) -> Image.Image:
    s = 4  # supersampling
    n = size * s
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    # macOS icon grid: the rounded square covers ~80 % of the canvas.
    pad = round(n * 0.1)
    box = (pad, pad, n - pad, n - pad)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=round((box[2] - box[0]) * 0.225), fill=255)
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / (n - 1)
        gd.line([(0, y), (n, y)], fill=tuple(round(BG_TOP[i] * (1 - t) + BG_BOTTOM[i] * t) for i in range(4)))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # The slice, from the 26-unit rail logo: half-disc below y=15, radius 10.
    # The logo's 20 x 10 unit half-disc, centred in the rounded square.
    unit = (box[2] - box[0]) / 28.0
    ox = (box[0] + box[2]) / 2 - 13 * unit
    oy = (box[1] + box[3]) / 2 - 20 * unit

    def p(x, y):
        return (ox + x * unit, oy + y * unit)

    cx, cy = p(13, 15)
    r = 10 * unit
    d.pieslice((cx - r, cy - r, cx + r, cy + r), 0, 180, fill=ACCENT)
    r2 = 7 * unit
    w = max(1, round(1.5 * unit))
    d.arc((cx - r2, cy - r2, cx + r2, cy + r2), 0, 180, fill=DARK, width=w)
    for sx in (10, 16):
        x, y = p(sx, 18)
        rr = 1.0 * unit
        d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=DARK)
    return img.resize((size, size), Image.LANCZOS)


def png_bytes(img: Image.Image) -> bytes:
    b = io.BytesIO()
    img.save(b, format="PNG", optimize=True)
    return b.getvalue()


def main() -> None:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    big = draw(1024)
    big.save(out / "icon_1024.png", optimize=True)
    # icns entries: type codes for PNG payloads.
    entries = [("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512),
               ("ic10", 1024), ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512)]
    chunks = b""
    for code, px in entries:
        data = png_bytes(draw(px))
        chunks += code.encode() + struct.pack(">I", len(data) + 8) + data
    (out / "FruitSlicer.icns").write_bytes(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


if __name__ == "__main__":
    main()

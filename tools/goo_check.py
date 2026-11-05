#!/usr/bin/env python3
"""Independent .goo V3 checker, written separately from the Rust crate.

Walks a file using only the byte layout in ELEGOO's "Goo Format Spec V1.2",
verifies every constant, delimiter, checksum and pixel count, and reports the
lit-pixel bounding boxes of one layer in mm. Standard library only.

    python3 tools/goo_check.py file.goo [layer]
"""
import struct
import sys

MAGIC = bytes([0x07, 0, 0, 0, 0x44, 0x4C, 0x50, 0x00])
DELIM = b"\r\n"
ENDING = b"\x00\x00\x00" + MAGIC
HEADER_SIZE = 195477


def fail(msg):
    sys.exit(f"FAIL: {msg}")


def decode(data, width, height):
    """Returns per-row lists of (x0, x1) spans of non-zero pixels."""
    if data[0] != 0x55:
        fail("layer magic")
    body, stored = data[1:-1], data[-1]
    if (~sum(body)) & 0xFF != stored:
        fail("layer checksum")
    rows = [[] for _ in range(height)]
    pos, prev, i = 0, 0, 0
    while i < len(body):
        b0 = body[i]; i += 1
        kind = b0 >> 6
        if kind == 2:
            delta = b0 & 0xF
            val = prev - delta if b0 & 0x20 else prev + delta
            if b0 & 0x10:
                n = body[i]; i += 1
            else:
                n = 1
        else:
            val = {0: 0, 3: 255}.get(kind)
            if kind == 1:
                val = body[i]; i += 1
            n = 0
            for _ in range((b0 >> 4) & 3):
                n = (n << 8) | body[i]; i += 1
            n = (n << 4) | (b0 & 0xF)
        if val:
            p = pos
            while p < pos + n:  # split the run at row boundaries
                y, x = divmod(p, width)
                end = min(pos + n, (y + 1) * width)
                rows[y].append((x, x + end - p))
                p = end
        pos += n
        prev = val
    if pos != width * height:
        fail(f"layer has {pos} pixels, expected {width * height}")
    return rows


def main():
    path = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    buf = open(path, "rb").read()
    if buf[4:12] != MAGIC:
        fail("header magic")
    o = 4 + 8 + 32 + 24 + 24 + 32 + 32 + 32 + 6  # strings + AA/grey/blur
    o += 116 * 116 * 2
    if buf[o:o + 2] != DELIM: fail("small preview delimiter")
    o += 2 + 290 * 290 * 2
    if buf[o:o + 2] != DELIM: fail("big preview delimiter")
    o += 2
    layers, rx, ry = struct.unpack_from(">IHH", buf, o)
    sx, sy, sz, lh = struct.unpack_from(">ffff", buf, o + 10)
    # Offset of field 59: after field 58 (price unit), 8 bytes before the end.
    off = struct.unpack_from(">I", buf, HEADER_SIZE - 7)[0]
    if off != HEADER_SIZE: fail(f"layer content offset {off}")
    print(f"{buf[:4].decode()} {rx}x{ry} px, {sx:.2f}x{sy:.2f}x{sz:.0f} mm, {layers} layers of {lh:.3f} mm")
    p = HEADER_SIZE
    for n in range(layers):
        z = struct.unpack_from(">f", buf, p + 6)[0]
        p += 2 + 15 * 4 + 2
        if buf[p:p + 2] != DELIM: fail(f"layer {n} def delimiter")
        size = struct.unpack_from(">I", buf, p + 2)[0]
        data = buf[p + 6:p + 6 + size]
        p += 6 + size
        if buf[p:p + 2] != DELIM: fail(f"layer {n} data delimiter")
        p += 2
        if abs(z - (n + 1) * lh) > 1e-4: fail(f"layer {n} z={z}")
        rows = decode(data, rx, ry)
        if n == want:
            report(rows, rx, ry, sx, sy)
    if buf[p:] != ENDING: fail("ending string")
    print(f"OK: {layers} layers, all checksums, delimiters and pixel counts valid")


def report(rows, rx, ry, sx, sy):
    px, py = sx / rx, sy / ry
    done, open_ = [], {}
    for y, spans in enumerate(rows):
        for x0, x1 in spans:
            b = open_.get(x0)
            if b is None or b[3] != y - 1:  # a gap in rows starts a new region
                if b is not None:
                    done.append(b)
                b = open_[x0] = [x0, x1, y, y]
            b[3] = y
    print("lit regions (mm, centre-relative):")
    for x0, x1, y0, y1 in sorted(done + list(open_.values()), key=lambda b: (b[2], b[0])):
        w, h = (x1 - x0) * px, (y1 + 1 - y0) * py
        cx, cy = ((x0 + x1) / 2 - rx / 2) * px, ((y0 + y1 + 1) / 2 - ry / 2) * py
        print(f"  {w:6.2f} x {h:6.2f} at ({cx:+7.2f}, {cy:+7.2f})")


if __name__ == "__main__":
    main()

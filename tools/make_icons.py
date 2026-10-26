#!/usr/bin/env python3
"""Draws the tray icons (tray/res/*.ico): a speaker in the state colour, with sound waves (streaming),
three dots (waiting, HDMI released) or a cross (service not running).

usage: make_icons.py [output_dir]      (default: tray/res next to this script's parent)
Pure Python, no dependencies. Rerun after changing the shapes; the .ico files are committed.
"""
import math
import os
import struct
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
STATES = {
    "ok": ((0x2E, 0xA0, 0x43), "waves"),
    "wait": ((0xD2, 0x99, 0x22), "dots"),
    "off": ((0x8C, 0x95, 0x9F), "cross"),
}

# Shapes in a unit square, y pointing down.
BODY = (0.06, 0.36, 0.28, 0.64)                                  # x0, y0, x1, y1
CONE = [(0.28, 0.36), (0.50, 0.14), (0.50, 0.86), (0.28, 0.64)]  # convex, clockwise on screen


def in_cone(x, y):
    n = len(CONE)
    for i in range(n):
        (x0, y0), (x1, y1) = CONE[i], CONE[(i + 1) % n]
        if (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0) < 0:
            return False
    return True


def near_segment(x, y, a, b, half_width):
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    px, py = ax + t * dx, ay + t * dy
    return (x - px) ** 2 + (y - py) ** 2 <= half_width ** 2


def inside(x, y, extra):
    if BODY[0] <= x <= BODY[2] and BODY[1] <= y <= BODY[3]:
        return True
    if in_cone(x, y):
        return True
    if extra == "waves":
        dx, dy = x - 0.50, y - 0.50
        r = math.hypot(dx, dy)
        if dx > 0 and abs(math.degrees(math.atan2(dy, dx))) <= 50:
            return 0.16 <= r <= 0.25 or 0.33 <= r <= 0.42
    elif extra == "dots":
        return any((x - cx) ** 2 + (y - 0.5) ** 2 <= 0.06 ** 2 for cx in (0.64, 0.78, 0.92))
    elif extra == "cross":
        return near_segment(x, y, (0.62, 0.36), (0.92, 0.64), 0.055) or \
            near_segment(x, y, (0.62, 0.64), (0.92, 0.36), 0.055)
    return False


def render(size, colour, extra):
    """Returns rows (top-down) of RGBA tuples with straight alpha."""
    ss = 2 if size > 64 else 4
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) / size
                    y = (py + (sy + 0.5) / ss) / size
                    hits += inside(x, y, extra)
            row.append((*colour, round(255 * hits / (ss * ss))))
        rows.append(row)
    return rows


def png(rows):
    size = len(rows)
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rows):
    size = len(rows)
    xor = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(stride)
        for x, px in enumerate(row):
            if px[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + xor + bytes(mask)


def ico(images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, blobs = b"", b""
    for size, blob in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(blobs))
        blobs += blob
    return header + entries + blobs


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "tray", "res")
    os.makedirs(out, exist_ok=True)
    for name, (colour, extra) in STATES.items():
        images = []
        for size in SIZES:
            rows = render(size, colour, extra)
            images.append((size, png(rows) if size >= 256 else dib(rows)))
        with open(os.path.join(out, f"{name}.ico"), "wb") as f:
            f.write(ico(images))
        print(f"{name}.ico")


if __name__ == "__main__":
    main()

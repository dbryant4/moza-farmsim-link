#!/usr/bin/env python3
"""Generate the mod icon: a 512x512 DXT1 (BC1) .dds with a single mip level.

That is the layout of the icons of the FS25 script mods this was checked
against (131200 bytes: 128 byte header + 512*512/2 bytes of 4x4 blocks).
No third-party modules needed.

    python3 fs-mod/tools/make_icon.py fs-mod/FS25_MozaYokeLink/icon_MozaYokeLink.dds
"""
import math
import struct
import sys

SIZE = 512
BACKGROUND = (30, 64, 44)     # dark green
FOREGROUND = (236, 226, 190)  # warm off-white


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def inside(x, y):
    """Shape: a yoke -- an open-topped wheel rim with a hub and two spokes."""
    cx = cy = SIZE / 2
    dx, dy = x - cx, y - cy
    r = math.hypot(dx, dy)
    angle = math.degrees(math.atan2(-dy, dx))  # 0 = right, 90 = up
    rim = 150 <= r <= 190 and not (40 < angle < 140)
    hub = r <= 48
    spoke = abs(dy) <= 20 and 48 <= abs(dx) <= 160
    stem = abs(dx) <= 20 and 48 <= dy <= 160
    return rim or hub or spoke or stem


def coverage(px, py):
    """Fraction of the pixel covered by the shape (3x3 supersampling)."""
    hits = 0
    for sy in range(3):
        for sx in range(3):
            if inside(px + (sx + 0.5) / 3, py + (sy + 0.5) / 3):
                hits += 1
    return hits / 9


def build():
    fg, bg = rgb565(FOREGROUND), rgb565(BACKGROUND)
    # color0 > color1 selects the four-colour mode: index 0 = color0,
    # 1 = color1, 2 = 2/3 color0 + 1/3 color1, 3 = 1/3 color0 + 2/3 color1.
    assert fg > bg
    levels = ((1.0, 0), (2 / 3, 2), (1 / 3, 3), (0.0, 1))

    blocks = bytearray()
    for by in range(0, SIZE, 4):
        for bx in range(0, SIZE, 4):
            bits = 0
            for y in range(4):
                for x in range(4):
                    c = coverage(bx + x, by + y)
                    index = min(levels, key=lambda lv: abs(lv[0] - c))[1]
                    bits |= index << (2 * (4 * y + x))
            blocks += struct.pack("<HHI", fg, bg, bits)

    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH = 0x1, 0x2, 0x4
    DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x1000, 0x20000, 0x80000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE
    DDPF_FOURCC = 0x4
    DDSCAPS_TEXTURE = 0x1000

    header = b"DDS " + struct.pack(
        "<7I44x",
        124,            # dwSize
        flags,
        SIZE,           # height
        SIZE,           # width
        len(blocks),    # linear size of the top level
        1,              # depth
        1,              # mip map count
    )
    header += struct.pack("<II4s5I", 32, DDPF_FOURCC, b"DXT1", 0, 0, 0, 0, 0)  # pixel format
    header += struct.pack("<5I", DDSCAPS_TEXTURE, 0, 0, 0, 0)                   # caps + reserved
    assert len(header) == 128, len(header)
    return header + bytes(blocks)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "icon_MozaYokeLink.dds"
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}: {len(data)} bytes")

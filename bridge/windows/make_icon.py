#!/usr/bin/env python3
"""Draws app.ico: a steering-wheel ring on a field-green disc. Needs Pillow. Run when the design changes; the .ico is committed."""
from PIL import Image, ImageDraw

def draw(size):
    scale = 8  # supersample, then shrink, for smooth edges at 16 px
    n = size * scale
    image = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    d.ellipse([0, 0, n - 1, n - 1], fill=(46, 125, 50, 255))                 # disc
    rim_outer, rim_inner = 0.80, 0.56
    c = n / 2
    d.ellipse([c - c * rim_outer, c - c * rim_outer, c + c * rim_outer, c + c * rim_outer], fill=(255, 255, 255, 255))
    d.ellipse([c - c * rim_inner, c - c * rim_inner, c + c * rim_inner, c + c * rim_inner], fill=(46, 125, 50, 255))
    spoke = max(scale, int(n * 0.13))
    d.rectangle([c - c * rim_inner - 2, c - spoke / 2, c + c * rim_inner + 2, c + spoke / 2], fill=(255, 255, 255, 255))   # left-right spoke
    d.rectangle([c - spoke / 2, c, c + spoke / 2, c + c * rim_inner + 2], fill=(255, 255, 255, 255))                        # lower spoke
    hub = c * 0.24
    d.ellipse([c - hub, c - hub, c + hub, c + hub], fill=(255, 255, 255, 255))
    return image.resize((size, size), Image.LANCZOS)

sizes = [16, 20, 24, 32, 40, 48, 64, 256]
draw(256).save("app.ico", format="ICO", sizes=[(s, s) for s in sizes])
print("wrote app.ico with sizes", sizes)

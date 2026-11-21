#!/usr/bin/env python3
"""
Generate a few example home-screen wallpapers for the ``wallpaper`` command.

These are simple, self-contained Pillow gradients — no vendor bytes — written to
``samples/`` at the stock panel size (1024x600). ``samples/*.jpg`` is gitignored
(the folder is where you drop your own pictures, and extracted firmware images
must never be committed), so this script is the repo's committed way to produce
ready-to-use demos on demand::

    python3 scripts/make_sample_wallpapers.py
    headunit wallpaper stock/LTTF133.img all samples/wallpaper-ocean.jpg --out build

Tweak ``THEMES`` or pass ``WIDTHxHEIGHT`` as the first argument for another size.
"""
from __future__ import annotations

import os
import sys

from PIL import Image

W, H = 1024, 600  # stock wallpaper dimensions on this panel

# name -> (top color, bottom color). A vertical linear blend between the two.
THEMES: dict[str, tuple[tuple[int, int, int], tuple[int, int, int]]] = {
    "ocean":    ((16, 54, 98),  (10, 140, 160)),
    "sunset":   ((38, 22, 64),  (230, 120, 70)),
    "charcoal": ((28, 28, 32),  (60, 62, 70)),
}


def gradient(size: tuple[int, int], top, bottom) -> Image.Image:
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        r = round(top[0] + (bottom[0] - top[0]) * t)
        g = round(top[1] + (bottom[1] - top[1]) * t)
        b = round(top[2] + (bottom[2] - top[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    return img


def main(size: tuple[int, int]) -> int:
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "samples")
    os.makedirs(out_dir, exist_ok=True)
    for name, (top, bottom) in THEMES.items():
        path = os.path.join(out_dir, f"wallpaper-{name}.jpg")
        gradient(size, top, bottom).save(path, "JPEG", quality=92,
                                         optimize=True, progressive=False)
        print(f"wrote {path}  ({size[0]}x{size[1]})")
    return 0


if __name__ == "__main__":
    size = (W, H)
    if len(sys.argv) == 2:
        try:
            w, h = sys.argv[1].lower().split("x")
            size = (int(w), int(h))
        except ValueError:
            sys.exit("usage: make_sample_wallpapers.py [WIDTHxHEIGHT]")
    sys.exit(main(size))

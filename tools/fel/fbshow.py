#!/usr/bin/env python3
"""fbshow.py: put an image on the LCD over FEL.

    tools/fel/fbshow.py picture.png        # any size; scaled to fit 1280x720, letterboxed
    tools/fel/fbshow.py --test             # orientation/colour test card

Needs the display pipeline running: run `felrun.py dram/dsi4.bin` once after
catching FEL. The framebuffer is at 0x41000000 (720x1280 XRGB8888, portrait in
memory). The screen is viewed landscape, 1280x720:
    screen (sx, sy)  ->  buffer (x = 719 - sy, y = sx)
i.e. rotate the landscape image 90 degrees clockwise into the buffer.
"""
import argparse, os, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw

XFEL = os.path.expanduser("~/src/xfel/xfel")
FB = 0x41000000
W, H = 1280, 720            # landscape, as seen

def test_card():
    im = Image.new("RGB", (W, H), (24, 24, 24))
    d = ImageDraw.Draw(im)
    for i, c in enumerate([(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(0,0,0)]):
        d.rectangle([i * W // 8, 0, (i + 1) * W // 8 - 1, H // 2], fill=c)
    for x in range(W):
        g = x * 255 // (W - 1)
        d.line([x, H // 2 + 20, x, H - 120], fill=(g, g, g))
    d.rectangle([0, 0, W - 1, H - 1], outline=(255, 255, 255), width=4)
    d.text((40, H - 90), "headunit-toolkit: our own pixels, 1280x720", fill=(255, 255, 255), font_size=48)
    return im

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?")
    ap.add_argument("--test", action="store_true")
    a = ap.parse_args()
    if a.test or not a.image:
        im = test_card()
    else:
        src = Image.open(a.image).convert("RGB")
        src.thumbnail((W, H))
        im = Image.new("RGB", (W, H))
        im.paste(src, ((W - src.width) // 2, (H - src.height) // 2))
    buf = im.transpose(Image.Transpose.ROTATE_270)       # (sx,sy) -> (719-sy, sx): 720 wide, 1280 tall
    assert buf.size == (720, 1280)
    raw = buf.convert("RGBA").tobytes("raw", "BGRA")     # XRGB8888 little-endian
    with tempfile.NamedTemporaryFile(suffix=".fb", delete=False) as f:
        f.write(raw)
    t = time.time()
    r = subprocess.run([XFEL, "write", hex(FB), f.name], capture_output=True, text=True)
    os.unlink(f.name)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    print(f"wrote {len(raw)} bytes in {time.time() - t:.1f} s")

main()

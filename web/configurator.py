"""
Configurator orchestration — the one function the web UI calls.

Kept as a plain Python module (not trapped in browser JS) so it runs **both**
under Pyodide in the browser and under CPython for tests. It takes the stock
image bytes plus a set of safe, verified edits and returns the patched image
bytes. The firmware is never written to a server — in the browser this runs
entirely client-side in the Pyodide VM.

Only the low-risk, reversible operations are exposed here (boot splash,
wallpaper, menu-typo fixes). Boot-chain / module work is deliberately out of
scope for the configurator.
"""
from __future__ import annotations

import os
import tempfile

from headunit.imagewty import ImageWTY
from headunit.logo import swap_logo
from headunit.minfs import MinFS
from headunit.strings import fix_strings
from headunit.wallpaper import SLOTS, set_all_wallpapers, set_wallpaper


def extract_previews(image_bytes: bytes) -> dict:
    """Read the *current* boot splash and wallpapers out of a stock image so the
    UI can show what's already on the device. Returns raw JPEG bytes:

        {"splash": bytes|None, "wallpapers": [bytes|None, ...]}  # 4 slots

    Values are ``None`` when a slot isn't present. Nothing is modified.
    """
    img = ImageWTY(bytes(image_bytes))
    fs = MinFS(img.read(img.get("data_udisk.fex")))

    def rd(path: str):
        try:
            data = bytes(fs.read(path)).rstrip(b"\x00")  # slots are null-padded
            return data if data[:2] == b"\xff\xd8" else None  # must be a JPEG
        except Exception:
            return None

    splash = rd("/Logo/logo1.jpg") or rd("/logo1.jpg")
    walls = [rd(f"/WallPaper/{i}.jpg") for i in range(SLOTS)]
    return {"splash": splash, "wallpapers": walls}


def preview_data_urls(image_bytes: bytes) -> dict:
    """Same as :func:`extract_previews` but as ``data:`` URLs (or ``None``) so
    the browser can drop them straight into ``<img src>``."""
    import base64

    def url(b):
        return "data:image/jpeg;base64," + base64.b64encode(b).decode() if b else None

    p = extract_previews(image_bytes)
    return {"splash": url(p["splash"]),
            "wallpapers": [url(w) for w in p["wallpapers"]]}


def _tmp(data: bytes) -> str:
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".upload")
    f.write(bytes(data))
    f.close()
    return f.name


def apply_edits(image_bytes: bytes, *, splash: bytes | None = None,
                wallpapers: list | None = None) -> tuple[bytes, list[str]]:
    """Apply per-slot image edits and return ``(patched_bytes, log_lines)``.

    ``splash`` replaces the boot logo. ``wallpapers`` is a list of up to 4
    entries (``bytes`` to replace that slot, ``None`` to leave it); index i maps
    to ``/WallPaper/i.jpg``. Each edit rewrites the MINFS partition with a
    corrected checksum, so they compose. Nothing is uploaded.
    """
    log: list[str] = []
    img = ImageWTY(bytes(image_bytes))
    log.append(f"Loaded image — {img.num_files} partitions.")

    for i, wp in enumerate(wallpapers or []):
        if wp is None:
            continue
        if not 0 <= i < SLOTS:
            continue
        path = _tmp(wp)
        try:
            set_wallpaper(img, i, path)
            log.append(f"Wallpaper {i + 1} — replaced.")
        finally:
            os.unlink(path)

    if splash is not None:
        path = _tmp(splash)
        try:
            jpg = swap_logo(img, path)
            log.append(f"Boot splash — replaced ({len(jpg)} bytes).")
        finally:
            os.unlink(path)

    return bytes(img.to_bytes()), log


def apply(image_bytes: bytes, *, fix_typos: bool = False,
          splash: bytes | None = None,
          wallpaper: bytes | None = None,
          wallpaper_slot: str = "all") -> tuple[bytes, list[str]]:
    """Apply the selected edits to ``image_bytes`` and return
    ``(patched_bytes, log_lines)``.

    Each edit re-reads and rewrites the MINFS partition with a corrected
    checksum, so they compose in any order. ``wallpaper_slot`` is ``"all"`` or
    ``"0".."3"``. Image inputs are raw bytes of any common picture format.
    """
    log: list[str] = []
    img = ImageWTY(bytes(image_bytes))
    log.append(f"Loaded image — {img.num_files} partitions.")

    if fix_typos:
        report = fix_strings(img)
        n = sum(r.count for r in report)
        log.append(f"Menu typo fixes — {n} substitution(s)."
                   if n else "Menu typo fixes — nothing matched this build.")

    if wallpaper is not None:
        path = _tmp(wallpaper)
        try:
            if wallpaper_slot == "all":
                made = set_all_wallpapers(img, path)
                log.append(f"Wallpaper — set all {len(made)} slots.")
            else:
                idx = int(wallpaper_slot)
                if not 0 <= idx < SLOTS:
                    raise ValueError(f"wallpaper slot must be 0..{SLOTS - 1}")
                set_wallpaper(img, idx, path)
                log.append(f"Wallpaper — set slot {idx}.")
        finally:
            os.unlink(path)

    if splash is not None:
        path = _tmp(splash)
        try:
            jpg = swap_logo(img, path)
            log.append(f"Boot splash — {len(jpg)} byte image.")
        finally:
            os.unlink(path)

    return bytes(img.to_bytes()), log

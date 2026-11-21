"""
Boot/splash-logo replacement for the data_udisk MINFS partition.

The splash is a 1280x720 baseline JPEG stored twice in ``data_udisk.fex``:
``Logo/logo1.jpg`` (logo choice #1 in Settings) and ``stalogo1.jpg`` (the
startup logo). This rewrites both in place — no file moves — and fixes the
partition's additive checksum in ``Vdata_udisk.fex``.

The new JPEG is re-encoded until it fits the original slot, so the partition
layout is byte-for-byte identical except the two images and the checksum.
"""
from __future__ import annotations

import io
import struct

from PIL import Image, ImageOps

from .imagewty import ImageWTY
from .minfs import MAGIC as MINFS_MAGIC, checksum

W, H = 1280, 720
LOGO_NAMES = (b"logo1.jpg", b"stalogo1.jpg")


def _find_jpeg_entry(part: bytes, name: bytes):
    """Locate a MINFS directory entry for a JPEG by name. Returns
    (entry_off, data_off, stored_len)."""
    hits = []
    start = 0
    while (i := part.find(name + b"\0", start)) != -1:
        e = i - 0x14
        off, s1, s2, rl, attr, nl, _ = struct.unpack_from("<IIIHHHH", part, e)
        if nl == len(name) and s1 == s2 and not (attr & 1) and \
                part[off:off + 2] == b"\xff\xd8":
            hits.append((e, off, s1))
        start = i + 1
    if len(hits) != 1:
        raise ValueError(f"expected 1 entry for {name!r}, found {len(hits)}")
    return hits[0]


def encode_to_fit(src_path: str, limit: int) -> bytes:
    """Scale/crop ``src_path`` to 1280x720 and JPEG-encode under ``limit`` bytes."""
    im = ImageOps.exif_transpose(Image.open(src_path)).convert("RGB")
    im = ImageOps.fit(im, (W, H), Image.LANCZOS)
    for q in range(95, 29, -5):
        for sub in (0, 2):  # 4:4:4 like the vendor file, then 4:2:0
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=q, subsampling=sub,
                    optimize=True, progressive=False)
            if buf.tell() <= limit:
                return buf.getvalue()
    raise ValueError("image will not fit the logo slot; use a simpler picture")


def swap_logo(img: ImageWTY, src_path: str) -> bytes:
    """Replace both logo copies in ``img`` (modified in place). Returns the
    encoded JPEG bytes written (also useful as a preview)."""
    part_e = img.get("data_udisk.fex")
    v_e = img.get("Vdata_udisk.fex")
    part = bytearray(img.read(part_e))
    if part[:5] != MINFS_MAGIC:
        raise ValueError("data_udisk is not MINFS")
    if struct.unpack_from("<I", img.data, v_e.offset)[0] != checksum(part):
        raise ValueError("input partition checksum mismatch; aborting")

    entries = [_find_jpeg_entry(part, n) for n in LOGO_NAMES]
    limit = min(stored for _, _, stored in entries)
    jpg = encode_to_fit(src_path, limit)

    for entry_off, data_off, stored in entries:
        part[data_off:data_off + stored] = jpg + b"\0" * (stored - len(jpg))
        struct.pack_into("<II", part, entry_off + 0x04, len(jpg), len(jpg))

    img.data[part_e.offset:part_e.offset + part_e.length] = part
    struct.pack_into("<I", img.data, v_e.offset, checksum(part))
    return jpg

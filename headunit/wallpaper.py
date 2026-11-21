"""
Home-screen wallpaper replacement for the data_udisk MINFS partition.

The launcher ships four selectable wallpapers in ``data_udisk.fex`` at
``/WallPaper/0.jpg`` .. ``/WallPaper/3.jpg`` (the choices in Settings). This
replaces any of them with your own picture and fixes the partition's additive
checksum in ``Vdata_udisk.fex``.

Unlike :mod:`headunit.logo`, this does **not** squeeze the new JPEG under the
original slot size: :meth:`MinFS.repack` relocates a file that grows into the
partition's free tail, so the only constraint is the free arena's size. We do
still match each wallpaper's **original pixel dimensions** (read from the stock
JPEG) so the picture lands exactly as the launcher expects — on this panel the
stock wallpapers are 1024x600, not the 1280x720 of the splash logo.
"""
from __future__ import annotations

import io

from PIL import Image, ImageOps

from .imagewty import ImageWTY
from .minfs import MinFS, write_partition

PART = "data_udisk.fex"
SLOTS = 4  # /WallPaper/0.jpg .. /WallPaper/3.jpg


def _slot_path(index: int) -> str:
    if not 0 <= index < SLOTS:
        raise ValueError(f"wallpaper index must be 0..{SLOTS - 1}, got {index}")
    return f"/WallPaper/{index}.jpg"


def encode_to_size(src_path: str, size: tuple[int, int], quality: int = 92) -> bytes:
    """Scale/crop ``src_path`` to exactly ``size`` (w, h) and JPEG-encode it.

    Matches the vendor files: baseline (non-progressive) JPEG. No byte-size
    ceiling — ``MinFS.repack`` handles any length — so we keep a single high
    quality rather than stepping down like the logo encoder.
    """
    im = ImageOps.exif_transpose(Image.open(src_path)).convert("RGB")
    im = ImageOps.fit(im, size, Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True, progressive=False)
    return buf.getvalue()


def set_wallpaper(img: ImageWTY, index: int, src_path: str,
                  partition: str = PART) -> bytes:
    """Replace ``/WallPaper/<index>.jpg`` in ``img`` (modified in place) with
    ``src_path`` re-encoded to that slot's original pixel dimensions.

    Commits via :meth:`MinFS.repack` + :func:`write_partition` (same partition
    footprint, checksum fixed). Returns the encoded JPEG bytes (handy as a
    preview).
    """
    path = _slot_path(index)
    fs = MinFS(img.read(partition))
    node = fs.find(path)
    stock = Image.open(io.BytesIO(fs.read(node)))
    jpg = encode_to_size(src_path, stock.size)

    new_blob = fs.repack({path: jpg})
    write_partition(img, new_blob, partition, "V" + partition)
    return jpg


def set_all_wallpapers(img: ImageWTY, src_path: str,
                       partition: str = PART) -> dict[int, bytes]:
    """Set every wallpaper slot (0..3) to ``src_path``. Returns a mapping of
    index -> encoded JPEG bytes. Each slot keeps its own original dimensions."""
    fs = MinFS(img.read(partition))
    edits: dict[str, bytes] = {}
    out: dict[int, bytes] = {}
    for i in range(SLOTS):
        path = _slot_path(i)
        node = fs.find(path)
        stock = Image.open(io.BytesIO(fs.read(node)))
        jpg = encode_to_size(src_path, stock.size)
        edits[path] = jpg
        out[i] = jpg

    new_blob = fs.repack(edits)
    write_partition(img, new_blob, partition, "V" + partition)
    return out

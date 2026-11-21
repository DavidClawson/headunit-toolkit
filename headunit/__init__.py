"""Open Head Unit Toolkit — read/modify Allwinner F133 (Melis) car-stereo firmware.

See the top-level README and docs/ for the firmware format specs and the
modding / recovery workflow. Nothing here redistributes vendor firmware; the
tools operate on an image *you* supply.
"""
from .imagewty import ImageWTY, FileEntry
from .minfs import MinFS, Node, checksum
from .datav1 import DataV1, Widget

__all__ = [
    "ImageWTY", "FileEntry",
    "MinFS", "Node", "checksum",
    "DataV1", "Widget",
]
__version__ = "0.1.0"

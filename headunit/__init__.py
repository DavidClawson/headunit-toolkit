"""Open Head Unit Toolkit — read/modify Allwinner F133 (Melis) car-stereo firmware.

See the top-level README and docs/ for the firmware format specs and the
modding / recovery workflow. Nothing here redistributes vendor firmware; the
tools operate on an image *you* supply.
"""
from .imagewty import ImageWTY, FileEntry
from .minfs import MinFS, Node, checksum, write_partition
from .datav1 import DataV1, Widget
from .decompress import decompress, decompress_node, is_compressed, Decoded

__all__ = [
    "ImageWTY", "FileEntry",
    "MinFS", "Node", "checksum", "write_partition",
    "DataV1", "Widget",
    "decompress", "decompress_node", "is_compressed", "Decoded",
]
__version__ = "0.1.0"

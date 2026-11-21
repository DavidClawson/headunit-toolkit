"""Command-line entry point: ``python -m headunit ...`` (or ``headunit``)."""
from __future__ import annotations

import argparse
import os
import sys

from . import ImageWTY, MinFS, DataV1, __version__


def _load_img(path: str) -> ImageWTY:
    return ImageWTY.from_path(path)


def cmd_list(args):
    img = _load_img(args.image)
    print(f"IMAGEWTY v{img.header_version:#06x}  {img.num_files} files")
    for f in img.files:
        print(f"  {f.index:2}  {f.main_type:8} {f.sub_type:16} "
              f"{f.name:28} off={f.offset:#010x} len={f.length}")


def cmd_unpack(args):
    img = _load_img(args.image)
    os.makedirs(args.outdir, exist_ok=True)
    for f in img.files:
        with open(os.path.join(args.outdir, f"{f.index:02}_{f.name}"), "wb") as fh:
            fh.write(img.read(f))
    print(f"wrote {img.num_files} files to {args.outdir}")


def cmd_ls(args):
    img = _load_img(args.image)
    fs = MinFS(img.read(args.partition))
    for path, node in fs.walk():
        tag = "d" if node.is_dir else "-"
        extra = f"  ({node.stored_len}->{node.length})" if \
            node.stored_len != node.length else ""
        size = "" if node.is_dir else f"  {node.length}"
        print(f"{tag} {path}{size}{extra}")


def cmd_cat(args):
    img = _load_img(args.image)
    fs = MinFS(img.read(args.partition))
    data = fs.read(args.path)
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print(f"wrote {len(data)} bytes to {args.out}")
    else:
        sys.stdout.buffer.write(data)


def cmd_screen(args):
    img = _load_img(args.image)
    fs = MinFS(img.read(args.partition))
    scr = DataV1(fs.read(args.path))
    print(f"{args.path}: {len(scr.widgets)} widgets")
    print(f"{'widget':<26}{'kind':<8}{'x':>5}{'y':>5}{'w':>5}{'h':>5}  value")
    for w in scr.widgets:
        g = w.geom if w.geom else w.geom_raw
        note = "" if w.kind in ("Str",) else "*"
        print(f"{w.name:<26}{w.kind:<8}{g[0]:5}{g[1]:5}{g[2]:5}{g[3]:5}{note} "
              f"{w.value!r}")
    print("\n* geometry for non-Str widgets is raw (shifted by one field)")


def cmd_logo(args):
    from .logo import swap_logo
    img = _load_img(args.image)
    jpg = swap_logo(img, args.picture)
    out_img = os.path.join(args.outdir, "Update", "LTTF133.img")
    os.makedirs(os.path.dirname(out_img), exist_ok=True)
    with open(out_img, "wb") as fh:
        fh.write(img.to_bytes())
    with open(os.path.join(args.outdir, "preview_logo.jpg"), "wb") as fh:
        fh.write(jpg)
    print(f"wrote {out_img} ({len(jpg)} byte splash)")
    print("Copy the whole 'Update' folder to a FAT32 USB stick.")


def main(argv=None):
    p = argparse.ArgumentParser(prog="headunit", description=__doc__)
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("list", help="list IMAGEWTY sub-files")
    s.add_argument("image")
    s.set_defaults(func=cmd_list)

    s = sub.add_parser("unpack", help="extract all IMAGEWTY sub-files")
    s.add_argument("image")
    s.add_argument("outdir")
    s.set_defaults(func=cmd_unpack)

    s = sub.add_parser("ls", help="list files in a MINFS partition")
    s.add_argument("image")
    s.add_argument("--partition", default="data_udisk.fex")
    s.set_defaults(func=cmd_ls)

    s = sub.add_parser("cat", help="read one file out of a MINFS partition")
    s.add_argument("image")
    s.add_argument("path")
    s.add_argument("--partition", default="data_udisk.fex")
    s.add_argument("--out")
    s.set_defaults(func=cmd_cat)

    s = sub.add_parser("screen", help="decode a DATAV1.0 screen layout")
    s.add_argument("image")
    s.add_argument("path", help="e.g. RadioRds.data")
    s.add_argument("--partition", default="data_udisk.fex")
    s.set_defaults(func=cmd_screen)

    s = sub.add_parser("logo", help="build an update image with a custom splash")
    s.add_argument("image", help="stock LTTF133.img")
    s.add_argument("picture", help="your splash image (any format)")
    s.add_argument("outdir", help="output folder (gets an Update/ subfolder)")
    s.set_defaults(func=cmd_logo)

    args = p.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()

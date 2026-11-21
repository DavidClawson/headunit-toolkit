#!/usr/bin/env python3
"""Backward-compatible wrapper — prefer: python -m headunit logo <img> <pic> <out>

Builds an update image with a custom boot/splash logo.
Usage: python3 make_logo_update.py LTTF133.img my_picture.png OUT_DIR
"""
import sys

from headunit.cli import main

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(["logo", sys.argv[1], sys.argv[2], sys.argv[3]])

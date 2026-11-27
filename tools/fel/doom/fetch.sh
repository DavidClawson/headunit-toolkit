#!/bin/sh
# fetch.sh: get the third-party pieces of the Doom port (not committed here).
#   doomgeneric (GPLv2) at a pinned commit, and the shareware DOOM1.WAD (v1.9,
#   freely distributable; checked by SHA-1).
set -e
cd "$(dirname "$0")"
COMMIT=e257a09
if [ ! -d doomgeneric ]; then
	git clone -q https://github.com/ozkl/doomgeneric.git
	git -C doomgeneric checkout -q $COMMIT
fi
if [ ! -f doom1.wad ]; then
	curl -fL -o doom1.wad https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad
fi
echo "5b2e249b9c5133ec987b3ea77596381dc0d6bc1d  doom1.wad" | shasum -c -

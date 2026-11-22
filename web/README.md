# Head Unit Configurator (web prototype)

A browser front-end for the toolkit's **safe, reversible** edits — boot splash,
wallpaper, and menu-typo fixes. It runs the real `headunit` Python package
**client-side** via [Pyodide](https://pyodide.org) (Python compiled to
WebAssembly), so **your firmware never leaves the page** — nothing is uploaded.

## Run it

It must be served over HTTP from the **repo root** (the page fetches the
`headunit/` Python modules by relative path; opening `index.html` as a `file://`
won't work):

```sh
cd /path/to/this/repo
python3 -m http.server 8777
# then open http://localhost:8777/web/
```

Then: drop your stock `LTTF133.img` → tick the edits you want → **Build** →
download the patched `LTTF133.img`. Put it in an `Update/` folder on a FAT32 USB
stick and run the vendor update (see [`../docs/first-flash-bench.md`](../docs/first-flash-bench.md)).

## What it does / doesn't

- **Does:** custom boot splash (with a live 1280×720 crop preview), wallpaper
  (any/all four slots), menu typo fixes. These are resource edits, committed
  through the MINFS repacker with a corrected checksum — the output is a drop-in
  update image.
- **Doesn't:** anything touching the boot chain or proprietary modules. Those
  stay on the command line / bench unit on purpose, so a casual user can't brick
  a unit from a web button.

## How it's wired

- `index.html` — UI + Pyodide loader. Loads Pyodide + Pillow from the jsDelivr
  CDN, fetches the repo's `headunit/*.py` and `web/configurator.py` into the
  Pyodide filesystem, then calls `configurator.apply(...)`.
- `configurator.py` — the orchestration (one `apply()` function). Kept as a
  plain module so it runs **both** in Pyodide and under CPython; it's covered by
  the same verification the CLI commands use.

Hosting: because it's static and client-side, it deploys as-is to GitHub Pages
(serve the repo; the page lives at `/web/`).

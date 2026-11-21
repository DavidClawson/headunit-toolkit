# Legal & scope

Not legal advice. This is the project's self-imposed scope so contributors
stay on defensible ground. For anything you publish, have a lawyer review the
specific firmware EULA and your distribution model.

## The hard lines

1. **Do not redistribute vendor firmware** — not the `LTTF133.img`, not any
   `*.fex` partition, not extracted blobs. Tools operate on an image the user
   supplies. (`.gitignore` enforces this for the repo.)

2. **Do not redistribute the proprietary modules** — `carplay.mod`,
   `auto.mod`, `wireless.mod`, their license data, or anything derived from
   them. They are licensed middleware (CarPlay ultimately under Apple MFi).
   You have a *copy*; you do not own the copyright and cannot relicense it.

3. **No trademarks** — don't name the project or its features "CarPlay" /
   "Android Auto" or use their logos. Use neutral terms.

## The safer model (what this repo is)

Ship **code + knowledge**: format specs, reverse-engineering notes, parsers,
patch tools, and how-to guides. Any proprietary bytes stay on the user's own
machine/unit and are never committed or distributed. This mirrors how Android
custom ROMs handle vendor blobs: ship extraction/recombination tooling, not the
blobs.

## Still-grey areas to check before publishing

- **The firmware EULA** may forbid reverse engineering, modification, or use
  outside the original device — a contract question separate from copyright.
- **DMCA §1201 / anti-circumvention** if any step bypasses a technical
  protection measure. (This image has only a plain checksum — no crypto — which
  helps, but is not a clearance.)
- A **"blob-reuse" helper** (extract the user's own `carplay.mod` + license
  from their own firmware and splice into their own modded Melis build, all
  locally) is the LineageOS-style pattern and is plausibly OK — but it depends
  on the EULA, and such a helper must *never* ship the blobs itself.

## What this means for goals

- **Open OS** → no native CarPlay/AA (Apple/Google license to certified
  product makers, not to flashable third-party OSes; the stacks are
  confidential, so open-source + licensee are mutually exclusive).
- **Keep CarPlay/AA** → stay on stock Melis for this hardware and mod around
  it. This is the supported direction of the project.

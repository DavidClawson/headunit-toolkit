# Modding workflow

A reproducible, safe way to make and track firmware variants.

## Layout

```
stock/LTTF133.img        pristine vendor image — never edited, the base for everything
nor_backup/              full CH341A dumps from each bench unit
variants/
  01-custom-logo/        one folder per variant: inputs + a recipe + notes
  02-rds-ps-on-main/
build/                   generated images (gitignored)
```

`stock/`, `nor_backup/`, `build/`, and any `*.img`/`*.fex` are gitignored —
they're vendor material and must not be committed (see
[legal-and-scope.md](legal-and-scope.md)).

## Golden rules

1. **Never edit `stock/`.** Every variant is produced from it by a script, so
   any build can be regenerated and diffed.
2. **Recompute, don't hand-fix.** The MINFS partition checksum is fixed
   automatically by the tools; never patch a byte without updating it.
3. **Risk tiers:**
   - *Resource edits* (logos, strings, `*.data` screens) — low risk,
     reversible by reflashing `stock/`. Fine on your in-car unit once proven.
   - *Boot-chain edits* (u-boot, boot0, selector) — **bench unit only**, after
     a NOR dump and confirmed FEL.
4. **Diff every build.** Confirm a change touched only what you intended
   (the logo builder's self-check is the model: it reports the exact changed
   byte ranges).

## Current capabilities vs planned

| task | status |
|---|---|
| custom boot splash | ✅ `headunit logo` |
| browse / extract partitions & MINFS | ✅ `list` / `unpack` / `ls` / `cat` |
| decode a screen layout | ✅ `screen` |
| **edit** a screen (move/add a widget) | ⏳ planned — record format is known |
| retranslate / edit `*.txt` strings | ⏳ planned (UTF-16LE tab tables) |
| unpack modules / `init.axf` (memory image or ELF) | ✅ `decompress [--elf]` |
| u-boot boot selector | ⏳ designed (see boot-and-recovery.md) |

## Example: first real variant (proof that layout edits work)

Goal: show the RDS **station name** on the main FM screen. `Radio.data` has no
RDS field, but we know the `ViewShowStrRdsPs` record format from
`RadioRds.data`. The plan is to synthesize that record into `Radio.data` at a
free position, rebuild, and flash to the **bench** unit — a low-risk resource
edit that validates screen editing before anything ambitious. (Needs the
screen *editor*, which is the next tool to build.)

## The AI-assisted contribution model

Because the firmware is heavily data-driven, self-documenting (descriptive log
strings, leaked source paths), and fully recoverable, this project is a good
fit for a "knowledge base + recipes" model rather than monolithic code:
contributors point an agent at the format specs and guides here to generate a
specific mod, test it on a bench unit, and contribute the recipe back. The
maintainers curate knowledge and guardrails.

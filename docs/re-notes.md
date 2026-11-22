# Reverse-engineering working notes

Durable notes for the deeper static RE (loader ABI, module format). Nothing here
is firmware — just offsets, addresses, and the recipe to reproduce the analysis
from your own `stock/LTTF133.img`.

## Decompressing the payloads

`init.axf` and the `*.mod/.drv` modules are LZMA-packed (concatenated raw-LZMA1
streams). Use the toolkit:

```sh
headunit decompress stock/LTTF133.img init.axf  --out init_axf.bin   # exact
headunit decompress stock/LTTF133.img /auto.mod --out auto_mod.bin   # exact
# carplay.mod / *.plg decode only partially today (flagged complete=False)
```

## `init.axf` link base = `0xe90e2e04`

`init.axf` is **position-independent**, but its data holds **absolute pointers**
linked at a fixed base. Recovered base = **`0xe90e2e04`** (VA of file offset 0),
so `VA = 0xe90e2e04 + file_offset`.

How it was found (reproducible): the loader references module-path strings
(`d:\mod\*.mod`) through absolute pointers. Collect every aligned qword `V` and
every module-path string file-offset `O`; the base that satisfies `V = base + O`
for the most modules wins (8/13 here). The pointers sit in a rodata pointer pool
around file `0x1583ec..0x158704` interleaved with log-string and function
pointers (so it is *not* a clean array — the install loop walks it by base+index,
which is why individual slots have no direct code xref).

## Ghidra setup (headless)

Installed via Homebrew formula `ghidra` (12.1.2);
`analyzeHeadless` at `/opt/homebrew/Cellar/ghidra/<ver>/libexec/support/`.
Python scripts need PyGhidra on 12.x — **use Java `GhidraScript`s** in headless.

```sh
analyzeHeadless <proj_dir> initaxf \
  -import init_axf.bin \
  -processor RISCV:LE:64:default \
  -loader BinaryLoader -loader-baseAddr 0xe90e2e04 \
  -scriptPath <scripts> -postScript YourScript.java
```

(For a module like `auto.mod`, base `0` is fine for disassembly, but its own
absolute pointers — e.g. the descriptor pointer the entry stub returns — need
that module's link base recovered the same way before they resolve.)

Use absolute paths (Ghidra rejects `./`). Drop `-deleteProject` to keep the
project for fast `-process` re-runs without re-analyzing.

## Module ABI — what's known vs open

**Known:**
- Modules are raw PIC RISC-V code+data images, no ELF/magic header
  ([format-minfs.md](format-minfs.md)).
- Entry at offset 0 is a *get-descriptor stub*: `auipc a5,…; addi; ld a0,0(a5);
  ret` — returns a pointer (loaded from a relocated slot) to the module's
  descriptor/export structure.
- Loaded by `esMODS_*` from a fixed list of `d:\mod\*.mod` slots; 10 of 13 are
  unused → **resource-level injection** at an unused slot ([lua-host.md](lua-host.md)).

**Open (needs interactive Ghidra + ideally the bench unit):**
- The exact `esMODS_Install` call sequence and what it reads from the module.
- The module descriptor/export table layout (follow the entry stub's pointer,
  after recovering the module's own link base).
- The trailer relocation/symbol tables' format.

These are interactive-RE shaped (follow xrefs, rename, read the live decompiler),
so the efficient venue is the **Ghidra GUI** (import with the recipe above), and
hypotheses are cheaply confirmed by loading a test module on the **bench unit**.

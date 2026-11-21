"""
Menu/UI string tables and a curated typo-fix pass.

The ``*.txt`` files in ``data_udisk.fex`` (``Setup.txt``, ``Main.txt``, …) are
multi-language string tables: UTF-16LE, one string per CRLF-terminated row,
columns separated by TAB. Column 0 is a ``//`` marker, column 1 is the Chinese
source (``原始``), **column 2 is English** (``英文``), followed by ~31 more
languages. ``/Setup.txt`` and ``/Language/Setup.txt`` are two paths onto the
*same* physical MINFS entry.

The stock English is, on the whole, fine. This module ships a small, curated
set of fixes for the few genuine defects (typos and stray whitespace), applied
as exact UTF-16LE byte substitutions so no row structure can drift. Because a
fix may change a string's byte length, the edits are committed through
``MinFS.repack`` (which relocates a grown file into the partition's free tail
and corrects the checksum) rather than the length-preserving in-place editors.

Each fix is ``(find, replace)`` as plain text; both are encoded UTF-16LE before
substitution. Add rows here to extend the pass — keep them to defensible,
unambiguous corrections, not wording preferences.
"""
from __future__ import annotations

from dataclasses import dataclass

from .minfs import MinFS, write_partition

# Per-file curated fixes. Keys are MINFS file names (matched at the root path).
STRING_FIXES: dict[str, list[tuple[str, str]]] = {
    "Setup.txt": [
        ("botton", "button"),            # "Click panel botton…" — typo (x2)
        ("RDS swich", "RDS switch"),      # "RDS swich" — typo
        ("Mirroring\xa0", "Mirroring"),  # stray non-breaking space (x2)
    ],
}


@dataclass
class FixResult:
    file: str
    find: str
    replace: str
    count: int          # occurrences replaced (0 = not present, skipped)


def parse_table(blob: bytes) -> tuple[list[str], list[list[str]]]:
    """Parse a language ``*.txt`` table into (header_fields, data_rows).

    Rows are TAB-split; the first row is the column header. Useful for
    inspection; the fix pass itself works on raw bytes, not this view.
    """
    text = blob.decode("utf-16-le", errors="replace")
    rows = [r.split("\t") for r in text.split("\r\n") if r]
    if not rows:
        return [], []
    return rows[0], rows[1:]


def english_index(header: list[str]) -> int:
    """Column index of the English string (header label ``英文``; default 2)."""
    try:
        return header.index("英文")
    except ValueError:
        return 2


def build_string_edits(mfs: MinFS,
                       fixes: dict[str, list[tuple[str, str]]] = STRING_FIXES
                       ) -> tuple[dict[str, bytes], list[FixResult]]:
    """Compute ``{path: new_bytes}`` edits and a per-fix report, without
    modifying anything. A fix that matches nothing is reported with count 0 and
    skipped (not an error — the image may be a different build)."""
    edits: dict[str, bytes] = {}
    report: list[FixResult] = []
    for fname, pairs in fixes.items():
        path = "/" + fname
        try:
            data = mfs.read(path)
        except StopIteration:
            for find, repl in pairs:
                report.append(FixResult(fname, find, repl, 0))
            continue
        new = data
        for find, repl in pairs:
            fb = find.encode("utf-16-le")
            cnt = new.count(fb)
            if cnt:
                new = new.replace(fb, repl.encode("utf-16-le"))
            report.append(FixResult(fname, find, repl, cnt))
        if new != data:
            edits[path] = new
    return edits, report


def fix_strings(img, partition: str = "data_udisk.fex",
                v_partition: str = "Vdata_udisk.fex",
                fixes: dict[str, list[tuple[str, str]]] = STRING_FIXES
                ) -> list[FixResult]:
    """Apply the curated string fixes to ``img`` in place (repacking the MINFS
    partition and fixing its checksum). Returns the per-fix report. If nothing
    matched, the image is left unchanged."""
    mfs = MinFS(img.read(partition))
    edits, report = build_string_edits(mfs, fixes)
    if edits:
        write_partition(img, mfs.repack(edits), partition, v_partition)
    return report

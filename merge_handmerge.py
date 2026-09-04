#!/usr/bin/env python
"""Regenerate pyrtklib5/pyrtklib5.cpp = gen_rtk.py output + HANDMERGE blocks.

The generated file (pyrtklib5_pre.cpp) is never compiled. Everything written
by hand in pyrtklib5.cpp sits between

    /* ==== BEGIN HANDMERGE: <subject> ==== */
    ...
    /* ==== END HANDMERGE ==== */

and is anchored on the line that precedes its BEGIN marker. This script runs
the generator, re-inserts every block after its anchor in the fresh output and
rewrites pyrtklib5.cpp. It fails loudly (non-zero exit, nothing written) when
an anchor disappeared or became ambiguous, which is exactly what a header bump
that moves a binding looks like.

Usage: python merge_handmerge.py [--check]
  --check   regenerate into memory and report whether pyrtklib5.cpp would change
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CPP = HERE / "pyrtklib5" / "pyrtklib5.cpp"
PRE = HERE / "pyrtklib5" / "pyrtklib5_pre.cpp"
BEGIN = re.compile(r"^\s*/\* ==== BEGIN HANDMERGE: (.*?) ==== \*/\s*$")
END = re.compile(r"^\s*/\* ==== END HANDMERGE ==== \*/\s*$")


def extract_blocks(text):
    """[(subject, anchor_line, block_lines)] in file order."""
    lines = text.split("\n")
    blocks, i = [], 0
    while i < len(lines):
        m = BEGIN.match(lines[i])
        if not m:
            i += 1
            continue
        # anchor = nearest non-blank line above the marker; the blank lines in
        # between travel with the block so the layout is reproduced exactly
        a = i - 1
        while a >= 0 and not lines[a].strip():
            a -= 1
        if a < 0:
            sys.exit(f"HANDMERGE block {m.group(1)!r} has no anchor line above it")
        anchor = lines[a]
        gap = lines[a + 1 : i]
        j = i + 1
        while j < len(lines) and not END.match(lines[j]):
            if BEGIN.match(lines[j]):
                sys.exit(f"nested HANDMERGE block inside {m.group(1)!r}")
            j += 1
        if j >= len(lines):
            sys.exit(f"HANDMERGE block {m.group(1)!r} is never closed")
        blocks.append((m.group(1), anchor, gap + lines[i : j + 1]))
        i = j + 1
    return blocks


def inject(generated, blocks):
    lines = generated.split("\n")
    for subject, anchor, block in blocks:
        hits = [k for k, line in enumerate(lines) if line == anchor]
        if len(hits) != 1:
            sys.exit(
                f"anchor of HANDMERGE block {subject!r} found {len(hits)} times in the "
                f"generated file (need exactly 1):\n    {anchor}"
            )
        k = hits[0] + 1
        lines[k:k] = block
    return "\n".join(lines)


def main(argv):
    check = "--check" in argv
    current = CPP.read_text()
    blocks = extract_blocks(current)
    if not blocks:
        sys.exit("no HANDMERGE block found in pyrtklib5.cpp: refusing to overwrite it")
    subprocess.run([sys.executable, str(HERE / "gen_rtk.py")], cwd=HERE, check=True)
    merged = inject(PRE.read_text(), blocks)
    if merged == current:
        print(f"pyrtklib5.cpp unchanged ({len(blocks)} HANDMERGE blocks re-injected)")
        return 0
    if check:
        print("pyrtklib5.cpp would change (run without --check to rewrite it)")
        return 1
    CPP.write_text(merged)
    print(f"pyrtklib5.cpp rewritten ({len(blocks)} HANDMERGE blocks re-injected)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes scripts/switch_text_100.json: where each entry of the text sheets the Switch 1.05
update changed comes from in the Switch 1.0.0 sheet of the same name (#83, #85).

The 1.0.0 language archives (Unicorn_US/FR/DE/ES/IT.CPK of the base game) have other entry
counts in five sheets than the PS4 v01.04 game (UcOption 92 for 115, UcSysMsg 1400 for 1451, ...);
1.05 added entries in the middle, so the game reads text by the wrong numbers or past the end and
crashes at start. The layout is the same in every language: the map comes from the English
archives (matching text) and is checked against French, German, Spanish and Italian. The file
has entry numbers only, no text.

    switch_text_map.py <1.0.0 Unicorn_US.CPK> <1.05 Unicorn_US.CPK> [out.json]
"""
import difflib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import cpk  # noqa: E402
import fms  # noqa: E402


def sheets(path):
    archive = cpk.Archive(path)
    return {name: fms.parse(cpk.crilayla(archive.read(name)))
            for name in archive.files if name.casefold().endswith('.fms')}


def runs(old, new):
    """[[new index, old index, length], ...] for the entries of `new` that `old` has: equal
    text, or text changed in place (a replaced block of the same length)."""
    out = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
            if out and out[-1][0] + out[-1][2] == j1 and out[-1][1] + out[-1][2] == i1:
                out[-1][2] += j2 - j1
            else:
                out.append([j1, i1, j2 - j1])
    return out


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    old, new = sheets(sys.argv[1]), sheets(sys.argv[2])
    table = {name: {'from': len(old[name]), 'to': len(new[name]), 'runs': runs(old[name], new[name])}
             for name in sorted(new) if name in old and len(old[name]) != len(new[name])}
    out = Path(sys.argv[3]) if len(sys.argv) == 4 else ROOT / 'scripts' / 'switch_text_100.json'
    out.write_text(json.dumps(table, indent=1) + '\n')
    for name, entry in table.items():
        kept = sum(r[2] for r in entry['runs'])
        print(f'{name}: {entry["from"]} -> {entry["to"]} entries, {kept} kept, {entry["to"] - kept} new')


if __name__ == '__main__':
    main()

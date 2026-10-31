#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unicorn Overlord text sheets (MsgSheet/*.fms in the language archives) as editable JSON.

An FMSB file: a 32-byte header (magic, body size, header size, ..., entry count at 0x14), one
8-byte slot per entry (zero on disk; the game fills in pointers), the entries' UTF-8 strings in
order, each ending in a zero byte, zero padding to 16 bytes, and a 16-byte FEOC footer.

    fms.py export <game folder> <out folder> [--archive Unicorn_TW.CPK]

writes <out folder>/MsgSheet/<Sheet>.json for every sheet: {"<entry>": "<text>", ...}. Edit the
texts, delete the entries left as they are if you like, and put the folder in mods/ as a mod
(docs/MODS.md, Translations).
"""
import argparse
import json
from pathlib import Path
import struct
import sys

import cpk

HEADER = 0x20
FOOTER = b'FEOC' + struct.pack('<III', 0, 0x10, 0)


def parse(data):
    """The strings of an FMSB file."""
    if data[:4] != b'FMSB' or len(data) < HEADER + len(FOOTER):
        raise ValueError('not an FMSB text sheet')
    header, count = struct.unpack_from('<I', data, 8)[0], struct.unpack_from('<I', data, 0x14)[0]
    at, end = header + 8 * count, len(data) - len(FOOTER)
    if data[end:] != FOOTER or at > end:
        raise ValueError('FMSB text sheet is cut short')
    strings = []
    for _ in range(count):
        stop = data.find(b'\0', at, end)
        if stop < 0:
            raise ValueError('FMSB text sheet has fewer strings than entries')
        strings.append(data[at:stop].decode('utf-8'))
        at = stop + 1
    return strings


def build(strings, template):
    """An FMSB file of `strings`, with the header of `template` (the sheet it replaces)."""
    if len(strings) != struct.unpack_from('<I', template, 0x14)[0]:
        raise ValueError('a sheet keeps its number of entries')
    header = struct.unpack_from('<I', template, 8)[0]
    body = bytearray(template[:header])
    body += b'\0' * (8 * len(strings))
    for text in strings:
        encoded = text.encode('utf-8')
        if b'\0' in encoded:
            raise ValueError('a text cannot contain a zero character')
        body += encoded + b'\0'
    body += b'\0' * (-len(body) % 16)
    struct.pack_into('<I', body, 4, len(body) - HEADER)  # up to the footer
    return bytes(body) + FOOTER


def apply(data, overrides, name='sheet'):
    """`data` (an FMSB file) with the entries in `overrides` ({"<entry>": text}) replaced."""
    strings = parse(data)
    if not isinstance(overrides, dict):
        raise ValueError(f'{name}: expected an object of "<entry number>": "<text>"')
    for key, text in overrides.items():
        if not str(key).isdigit() or int(key) >= len(strings):
            raise ValueError(f'{name}: no entry {key} (the sheet has {len(strings)})')
        if not isinstance(text, str):
            raise ValueError(f'{name}: entry {key} is not text')
        strings[int(key)] = text
    return build(strings, data)


def export(game, out, archive_name='Unicorn_TW.CPK'):
    """Writes every sheet of the game's `archive_name` as JSON under out/MsgSheet/. Returns the
    number of sheets."""
    source = cpk.child_file(game, archive_name)
    archive = cpk.Archive(source)
    folder = Path(out) / 'MsgSheet'
    folder.mkdir(parents=True, exist_ok=True)
    count = 0
    for name in sorted(archive.files):
        if not (name.startswith('MsgSheet/') and name.endswith('.fms')):
            continue
        strings = parse(archive.extract(name))
        text = json.dumps({str(i): s for i, s in enumerate(strings)}, ensure_ascii=False, indent=1)
        (folder / (Path(name).stem + '.json')).write_text(text + '\n', encoding='utf-8')
        count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('export', help='write the game\'s text as JSON for a translation')
    command.add_argument('game', type=Path, help='the game folder (with Unicorn_TW.CPK)')
    command.add_argument('out', type=Path, help='the translation mod folder to write')
    command.add_argument('--archive', default='Unicorn_TW.CPK',
                         help='the language archive to read (default Unicorn_TW.CPK, the default text)')
    args = parser.parse_args()
    count = export(args.game, args.out, args.archive)
    print(f'{count} sheets written to {args.out / "MsgSheet"}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f'fms: {error}', file=sys.stderr)
        sys.exit(1)

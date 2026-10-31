#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add-on content (DLC) the game is told it owns.

Put the DLC as you have it in dlc/ in the data directory: the PS4 package
(HP0177-CUSA36381_00-PRIVILEGEDEMBLEM-A0000-V0100.pkg) or its extracted folder (with
sce_sys/param.sfo). Only its name is read: the content ID in the package header (offset 0x40,
not encrypted) or in param.sfo. The entitlement label is the content ID's last 16 characters.

Unicorn Overlord's DLC unlocks content that is already in the game (it imports no call to mount
add-on data), so the labels are all the game needs. run.py passes them in VW_ADDCONT; the
runtime lists them in sceAppContentGetAddcontInfoList (src/runtime_content.c).

    dlc.py <dlc folder> <title id>     prints the labels found
"""
import argparse
from pathlib import Path
import re
import sys

PKG_MAGIC = b'\x7fCNT'
ADDITIONAL_CONTENT = 0x1b  # the package header's content type for add-ons
CONTENT_ID = re.compile(r'[A-Z]{2}\d{4}-([A-Z]{4}\d{5})_\d{2}-([A-Z0-9]{16})')


def from_package(path):
    """(title ID, label) of an add-on package, or None."""
    with open(path, 'rb') as f:
        head = f.read(0x78)
    if len(head) < 0x78 or head[:4] != PKG_MAGIC or int.from_bytes(head[0x74:0x78], 'big') != ADDITIONAL_CONTENT:
        return None
    match = CONTENT_ID.fullmatch(head[0x40:0x64].decode('ascii', 'replace'))
    return match.groups() if match else None


def from_folder(path):
    """(title ID, label) of an extracted add-on (sce_sys/param.sfo with category ac), or None."""
    from prepare import sfo
    try:
        values = sfo((path / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError):
        return None
    match = CONTENT_ID.fullmatch(str(values.get('CONTENT_ID', '')))
    return match.groups() if match and values.get('CATEGORY') == 'ac' else None


def labels(folder, title_id):
    """{label: name of the file or folder it came from} for `title_id`, in name order. Folders
    are searched three levels down (the extractor writes CUSA36381/<LABEL>/sce_sys)."""
    found = {}
    folder = Path(folder)
    if not folder.is_dir():
        return found

    def visit(path, depth):
        for entry in sorted(path.iterdir(), key=lambda e: e.name.casefold()):
            if entry.is_file() and entry.suffix.casefold() == '.pkg':
                item = from_package(entry)
            elif entry.is_dir():
                item = from_folder(entry)
                if not item and depth < 3:
                    visit(entry, depth + 1)
            else:
                item = None
            if item and item[0] == title_id:
                found.setdefault(item[1], entry.relative_to(folder).as_posix())
    visit(folder, 1)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folder', type=Path)
    parser.add_argument('title_id')
    args = parser.parse_args()
    for label, source in labels(args.folder, args.title_id).items():
        print(f'{label}\t{source}')


if __name__ == '__main__':
    sys.exit(main())

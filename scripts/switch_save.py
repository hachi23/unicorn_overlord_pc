#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Switch saves on the PS4 port (issue #71): converts a Switch UCSAVEFILE*.DAT to the PS4
UCSAVEFILE.DATA and puts it in a save slot.

Both files are the game's save struct as it is in memory, with the same "UCSD" header and
layout version (0x1C50D5). The Switch build lays out a few arrays differently; everything else
is the same bytes, moved. Found by comparing the two builds' save init code (PS4 v01.04
0x24cf59, Switch 1.05 0x2c8854) and checked on a real Switch save (docs/dev/SWITCH_SAVES.md):

- 350 town records (save+0x20AEA4 on the PS4): 0xD8 bytes on the PS4, 0xF0 on the Switch.
  Each holds a 0x40-byte name and two state objects of 0x4C (PS4) / 0x58 (Switch) bytes:
  0x40 bytes, then a byte, a 16-bit value and a byte that the Switch aligns to 8 bytes.
- Two tables of 512 and 256 entries near the end are u32 on the PS4 and u64 on the Switch
  (only the low 20 and 8 bits are used).
- The Switch pads its arrays more: the rest of the file is 0x20E0-0x2CF0 bytes later there.

usage: switch_save.py <Switch save folder or file> <user folder (data/user)> [slot]
A folder imports UCSAVEFILE00_AUTOSAVE.DAT to slot 0 (Auto) and UCSAVEFILE0n.DAT to slot n;
a single file goes to the slot given. Close the game first. The saves are backed up first
(data/user/savedata_backups, the same backups the launcher makes before every start).
"""
import re
import struct
import sys
from pathlib import Path

PS4_SIZE, SWITCH_SIZE = 0x4DF720, 0x4E2410
MAGIC, VERSION = b'UCSD', 0x1C50D5
SUB = 0x1FB880              # the sub-struct holding the town records (same place on both)
TOWNS = 350
# (PS4 offset, Switch offset, bytes) copied as they are; the arrays between are converted.
SAME = (
    (0, 0, SUB + 0xAE20 + 0x4801),                                   # up to the town records
    (SUB + 0x21D74, SUB + 0x23E48, 0x419),                          # after the records
    (SUB + 0x22190, SUB + 0x24270, 0xCFE00),                        # the next block
    (SUB + 0xF3210, SUB + 0xF5300, 0x4D35F8 - (SUB + 0xF3210)),     # to the 512-entry table
    (0x4D35F8, 0x4D56E8, 0x40),                                     # its flags
    (0x4D3E38, 0x4D6728, 0x4D5538 - 0x4D3E38),                      # to the 256-entry table
    (0x4D5538, 0x4D7E28, 0x20),                                     # its flags
    (0x4D5958, 0x4D8648, PS4_SIZE - 0x4D5958),                      # to the end
)
PS4_TOWNS, SWITCH_TOWNS = SUB + 0xAE20 + 0x4804, SUB + 0xAE20 + 0x4808
# (PS4, Switch) offsets of the tables, entry count, Switch value mask.
TABLES = ((0x4D35F8 + 0x40, 0x4D56E8 + 0x40, 512, 0xFFFFF), (0x4D5538 + 0x20, 0x4D7E28 + 0x20, 256, 0xFF))


def is_switch_save(data):
    return len(data) == SWITCH_SIZE and data[4:8] == MAGIC and struct.unpack_from('<I', data, 8)[0] == VERSION


def convert(switch):
    """The PS4 save for a Switch save (bytes)."""
    if not is_switch_save(switch):
        raise ValueError('not a Switch Unicorn Overlord save (expected %d bytes with a UCSD header)' % SWITCH_SIZE)
    ps4 = bytearray(PS4_SIZE)
    for to, frm, size in SAME:
        ps4[to:to + size] = switch[frm:frm + size]
    struct.pack_into('<I', ps4, 0xC, PS4_SIZE)
    for i in range(TOWNS):
        p, s = PS4_TOWNS + i * 0xD8, SWITCH_TOWNS + i * 0xF0
        ps4[p:p + 0x40] = switch[s:s + 0x40]
        for po, so in ((0x40, 0x40), (0x8C, 0x98)):
            a, b = p + po, s + so
            ps4[a:a + 0x41] = switch[b:b + 0x41]                       # 0x40 bytes and a byte
            struct.pack_into('<I', ps4, a + 0x44, struct.unpack_from('<H', switch, b + 0x48)[0])
            ps4[a + 0x48] = switch[b + 0x50]
    for p, s, count, mask in TABLES:
        values = struct.unpack_from('<%dQ' % count, switch, s)
        if any(v & ~mask for v in values):
            raise ValueError('table at 0x%X has values the PS4 save cannot hold' % s)
        struct.pack_into('<%dI' % count, ps4, p, *values)
    return bytes(ps4)


# The Load list comes from GLOBALDATA/UCGLOBALDATA.DAT: 100 records of 0x48 bytes at 0x1818, one
# per slot; a used one is the first 0x48 bytes of the slot's save (folder SAVEDATA<n>).
TABLE, RECORD, SLOTS = 0x1818, 0x48, 100


def title_dir(user_dir):
    return Path(user_dir) / 'savedata' / '1' / 'CUSA36381'


class NoGlobalData(ValueError):
    """The game has not made its save data yet."""


def import_save(user_dir, slot, ps4_save):
    """Writes a PS4 save (bytes) to slot n and lists it in the Load screen."""
    if not 0 <= slot < SLOTS:
        raise ValueError('slot must be 0..%d' % (SLOTS - 1))
    glob = title_dir(user_dir) / 'GLOBALDATA' / 'UCGLOBALDATA.DAT'
    if not glob.is_file():
        raise NoGlobalData('Start the game once and reach the title screen first: '
                           'the port has no save data yet.')
    g = bytearray(glob.read_bytes())
    if g[4:8] != b'UCSG':
        raise ValueError('%s: no UCSG header' % glob)
    folder = title_dir(user_dir) / ('SAVEDATA%d' % slot)
    folder.mkdir(exist_ok=True)
    target = folder / 'UCSAVEFILE.DATA'
    target.write_bytes(ps4_save)
    g[TABLE + slot * RECORD:TABLE + (slot + 1) * RECORD] = ps4_save[:RECORD]
    glob.write_bytes(bytes(g))
    return target


SAVE_NAME = re.compile(r'UCSAVEFILE(\d\d)(?:_AUTOSAVE)?\.DAT', re.I)


def switch_files(source):
    """[(slot, file)] of the Switch saves in a folder (searched two levels down, as JKSV and
    other tools nest it), or of one file (slot from its name, else None)."""
    source = Path(source)
    if source.is_file():
        m = SAVE_NAME.fullmatch(source.name)
        return [(int(m.group(1)) if m else None, source)]
    found = {}
    for path in sorted(source.glob('*.DAT')) + sorted(source.glob('*/*.DAT')) + sorted(source.glob('*/*/*.DAT')):
        m = SAVE_NAME.fullmatch(path.name)
        if m and is_switch_save(path.read_bytes()):
            found.setdefault(int(m.group(1)), path)
    return sorted(found.items())


def import_all(source, user_dir, slot=None):
    """Converts and imports the Switch saves at `source`; returns [(slot, Switch file)]."""
    files = switch_files(source)
    if not files:
        raise ValueError('%s: no Switch save (UCSAVEFILE*.DAT) found' % source)
    if len(files) == 1 and slot is not None:
        files = [(slot, files[0][1])]
    if any(s is None for s, _ in files):
        raise ValueError('give the slot for %s' % files[0][1].name)
    converted = [(s, path, convert(path.read_bytes())) for s, path in files]  # all, before writing
    if not title_dir(user_dir).joinpath('GLOBALDATA', 'UCGLOBALDATA.DAT').is_file():
        raise NoGlobalData('Start the game once and reach the title screen first: '
                           'the port has no save data yet.')
    from save_backup import backup
    backup(user_dir)
    for s, path, data in converted:
        import_save(user_dir, s, data)
    return [(s, path) for s, path, _ in converted]


def main(argv):
    if len(argv) not in (3, 4):
        raise SystemExit(__doc__)
    try:
        done = import_all(argv[1], argv[2], int(argv[3]) if len(argv) == 4 else None)
    except ValueError as error:
        raise SystemExit(str(error))
    for slot, path in done:
        print('slot %d%s: %s' % (slot, ' (Auto)' if slot == 0 else '', path))


if __name__ == '__main__':
    main(sys.argv)

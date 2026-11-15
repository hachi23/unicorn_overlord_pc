"""Switch save conversion (scripts/switch_save.py) on a synthetic Switch save with markers."""
import struct
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401
import switch_save as ss

TOWNS_SW = 0x1FB880 + 0xAE20 + 0x4808


def switch_save(slot_byte=1):
    d = bytearray(ss.SWITCH_SIZE)
    struct.pack_into('<4sII', d, 4, b'UCSD', 0x1C50D5, ss.SWITCH_SIZE)
    d[0x10] = slot_byte
    struct.pack_into('<I', d, 0x20, 6962929)                    # gold
    d[TOWNS_SW:TOWNS_SW + 10] = b'Town_C18_1'                    # first town record's name
    s = TOWNS_SW + 0xF0 + 0x98                                   # second record, second object
    d[s] = 0xAA
    d[s + 0x40] = 3
    struct.pack_into('<H', d, s + 0x48, 0x1234)
    d[s + 0x50] = 2
    struct.pack_into('<Q', d, 0x4D56E8 + 0x40 + 8 * 5, 0xFFFFF)  # 512-entry table, entry 5
    struct.pack_into('<Q', d, 0x4D7E28 + 0x20 + 8 * 255, 0xFF)   # 256-entry table, last entry
    d[0x4DA398] = 0x77                                           # the last block
    d[-1] = 0x55
    return bytes(d)


def global_data():
    g = bytearray(0x1818 + 100 * 0x48)
    g[4:8] = b'UCSG'
    return g


def switch_global(cleared=1):
    g = bytearray(ss.SWITCH_GLOBAL_SIZE)
    struct.pack_into('<4sI', g, 4, b'UCSG', 0x1C50D5)
    g[0x3C] = cleared                     # cleared once: True Zenoiran
    g[0x4C0] = 2                          # a Library entry
    g[0x1010] = 7                         # last slot used: not part of the record
    g[0x1818 + 4:0x1818 + 8] = b'UCSD'    # a Load list entry: not copied either
    return bytes(g)


class ConvertTest(unittest.TestCase):
    def test_moves_every_block_to_its_ps4_place(self):
        ps4 = ss.convert(switch_save())
        self.assertEqual(len(ps4), 0x4DF720)
        self.assertEqual(struct.unpack_from('<4sII', ps4, 4), (b'UCSD', 0x1C50D5, 0x4DF720))
        self.assertEqual(struct.unpack_from('<I', ps4, 0x20)[0], 6962929)
        towns = 0x1FB880 + 0xAE20 + 0x4804
        self.assertEqual(ps4[towns:towns + 10], b'Town_C18_1')
        o = towns + 0xD8 + 0x8C
        self.assertEqual((ps4[o], ps4[o + 0x40], struct.unpack_from('<I', ps4, o + 0x44)[0], ps4[o + 0x48]),
                         (0xAA, 3, 0x1234, 2))
        self.assertEqual(struct.unpack_from('<I', ps4, 0x4D35F8 + 0x40 + 4 * 5)[0], 0xFFFFF)
        self.assertEqual(struct.unpack_from('<I', ps4, 0x4D5538 + 0x20 + 4 * 255)[0], 0xFF)
        self.assertEqual(ps4[0x4D76A8], 0x77)
        self.assertEqual(ps4[-1], 0x55)

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            ss.convert(b'\0' * 0x4DF720)


class ImportTest(unittest.TestCase):
    def test_folder_goes_to_matching_slots_and_load_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            switch = tmp / 'switch' / 'Unicorn Overlord' / 'save'
            switch.mkdir(parents=True)
            (switch / 'UCSAVEFILE00_AUTOSAVE.DAT').write_bytes(switch_save(0))
            (switch / 'UCSAVEFILE03.DAT').write_bytes(switch_save(3))
            (switch / 'UCGLOBALDATA.DAT').write_bytes(switch_global())
            user = tmp / 'user'
            title = user / 'savedata' / '1' / 'CUSA36381'
            (title / 'GLOBALDATA').mkdir(parents=True)
            (title / 'GLOBALDATA' / 'UCGLOBALDATA.DAT').write_bytes(global_data())

            done = ss.import_all(tmp / 'switch', user)

            self.assertEqual([(s, p.name) for s, p in done],
                             [(0, 'UCSAVEFILE00_AUTOSAVE.DAT'), (3, 'UCSAVEFILE03.DAT')])
            self.assertEqual(len((title / 'SAVEDATA3' / 'UCSAVEFILE.DATA').read_bytes()), 0x4DF720)
            g = (title / 'GLOBALDATA' / 'UCGLOBALDATA.DAT').read_bytes()
            self.assertEqual(g[0x1818 + 3 * 0x48 + 4:0x1818 + 3 * 0x48 + 0x11], b'UCSD\xd5P\x1c\x00 \xf7M\x00\x03')
            self.assertEqual(len(list((user / 'savedata_backups').iterdir())), 1)
            self.assertEqual((g[0x3C], g[0x4C0], g[0x1010]), (1, 2, 0))
            self.assertEqual(g[0x1818 + 0x48 + 4:0x1818 + 0x48 + 8], b'\0' * 4)

    def test_without_a_switch_global_file_the_record_stays(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'switch').mkdir()
            (tmp / 'switch' / 'UCSAVEFILE01.DAT').write_bytes(switch_save())
            (tmp / 'switch' / 'UCGLOBALDATA.DAT').write_bytes(b'\0' * 200888)  # no header: ignored
            title = tmp / 'user' / 'savedata' / '1' / 'CUSA36381'
            (title / 'GLOBALDATA').mkdir(parents=True)
            g = global_data()
            g[0x3C] = 5
            (title / 'GLOBALDATA' / 'UCGLOBALDATA.DAT').write_bytes(g)

            ss.import_all(tmp / 'switch', tmp / 'user')

            self.assertEqual((title / 'GLOBALDATA' / 'UCGLOBALDATA.DAT').read_bytes()[0x3C], 5)

    def test_needs_the_game_started_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            save = Path(tmp) / 'UCSAVEFILE01.DAT'
            save.write_bytes(switch_save())
            with self.assertRaises(ss.NoGlobalData):
                ss.import_all(save, Path(tmp) / 'user')


if __name__ == '__main__':
    unittest.main()

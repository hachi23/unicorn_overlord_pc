import struct
import tempfile
import unittest
from pathlib import Path

import paths
import sys
sys.path.insert(0, str(paths.ROOT / 'scripts'))
import dlc  # noqa: E402


def package(path, content_id, content_type=0x1b):
    """The first bytes of a PS4 package header: magic, content ID at 0x40, type at 0x74."""
    head = bytearray(0x80)
    head[:4] = b'\x7fCNT'
    head[0x40:0x40 + len(content_id)] = content_id.encode()
    head[0x74:0x78] = content_type.to_bytes(4, 'big')
    path.write_bytes(bytes(head) + b'\0' * 256)


def param_sfo(values):
    """A param.sfo of UTF-8 string values."""
    keys = b''.join(k.encode() + b'\0' for k in values)
    data = [v.encode() + b'\0' for v in values.values()]
    table, key_at, data_at = b'', 0, 0
    for k, v in zip(values, data):
        table += struct.pack('<HHIII', key_at, 0x204, len(v), len(v), data_at)
        key_at += len(k) + 1
        data_at += len(v)
    start = 20 + len(table)
    return (struct.pack('<4sIIII', b'\0PSF', 0x101, start, start + len(keys), len(values)) +
            table + keys + b''.join(data))


class DlcTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_packages_and_extracted_folders(self):
        package(self.root / 'HP0177-CUSA36381_00-PRIVILEGEDEMBLEM-A0000-V0100.pkg',
                'HP0177-CUSA36381_00-PRIVILEGEDEMBLEM')
        folder = self.root / 'extracted' / 'CUSA36381' / 'ARTBOOK000000000' / 'sce_sys'
        folder.mkdir(parents=True)
        (folder / 'param.sfo').write_bytes(param_sfo(
            {'CATEGORY': 'ac', 'CONTENT_ID': 'HP0177-CUSA36381_00-ARTBOOK000000000'}))
        self.assertEqual(dlc.labels(self.root, 'CUSA36381'), {
            'ARTBOOK000000000': 'extracted/CUSA36381/ARTBOOK000000000',
            'PRIVILEGEDEMBLEM': 'HP0177-CUSA36381_00-PRIVILEGEDEMBLEM-A0000-V0100.pkg'})

    def test_other_games_updates_and_junk_ignored(self):
        package(self.root / 'other.pkg', 'EP0001-CUSA00001_00-SOMEADDON0000000')
        package(self.root / 'update.pkg', 'HP0177-CUSA36381_00-UNICORNOVERLORD0', content_type=0x1a)
        (self.root / 'notes.pkg').write_bytes(b'not a package')
        self.assertEqual(dlc.labels(self.root, 'CUSA36381'), {})
        self.assertEqual(dlc.labels(self.root / 'missing', 'CUSA36381'), {})


if __name__ == '__main__':
    unittest.main()

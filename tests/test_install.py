import os
import struct
import tempfile
import unittest
from pathlib import Path

import paths
import sys
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402


def param_sfo(title_id):
    """A param.sfo holding only TITLE_ID."""
    key, value = b'TITLE_ID\0', title_id.encode() + b'\0'
    keys, values = 20 + 16, 20 + 16 + len(key)
    return (struct.pack('<4sIIII', b'\0PSF', 0x101, keys, values, 1) +
            struct.pack('<HHIII', 0, 0x204, len(value), len(value), 0) + key + value)


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.source, self.target = root / 'CUSA36381', root / 'data' / 'game'
        (self.source / 'sce_sys').mkdir(parents=True)
        (self.source / 'eboot.bin').write_bytes(b'e' * 1000)
        (self.source / 'sce_sys' / 'param.sfo').write_bytes(b's' * 10)
        (self.source / 'Unicorn.CPK').write_bytes(b'c' * 20000)

    def tearDown(self):
        self.temp.cleanup()

    def test_installs_every_file_and_reports_progress(self):
        seen = []
        self.assertTrue(vw_launcher.install_files(self.source, self.target, lambda d, t, l: seen.append((d, t))))
        self.assertEqual((self.target / 'Unicorn.CPK').read_bytes(), b'c' * 20000)
        self.assertEqual((self.target / 'sce_sys' / 'param.sfo').read_bytes(), b's' * 10)
        self.assertEqual(seen[-1], (21010, 21010))
        self.assertFalse(self.target.with_name('game.partial').exists())

    def test_cancelled_install_leaves_no_game_folder_and_resumes(self):
        calls = iter([False, True])
        self.assertFalse(vw_launcher.install_files(self.source, self.target, cancelled=lambda: next(calls, True)))
        self.assertFalse(self.target.exists())
        self.assertTrue(vw_launcher.install_files(self.source, self.target))
        self.assertEqual(sorted(p.name for p in self.target.rglob('*') if p.is_file()),
                         ['Unicorn.CPK', 'eboot.bin', 'param.sfo'])

    def test_reinstall_replaces_the_previous_game(self):
        self.target.mkdir(parents=True)
        (self.target / 'old.bin').write_bytes(b'old')
        self.assertTrue(vw_launcher.install_files(self.source, self.target))
        self.assertFalse((self.target / 'old.bin').exists())

    def test_repack_spelling_wins_over_the_base(self):
        # #58: the English patch V2 (an update package) spells its archives in lower case.
        # Copied over the base on Linux both stay; the game opens Unicorn_TW.CPK, so the
        # update's file is installed under that name and the base's is left out.
        (self.source / 'sce_sys' / 'param.sfo').write_bytes(param_sfo('CUSA36381'))
        (self.source / 'Unicorn_TW.CPK').write_bytes(b'base')
        (self.source / 'Unicorn_TW.cpk').write_bytes(b'update')
        (self.source / 'UnicornPatch.cpk').write_bytes(b'patch')
        self.assertEqual(vw_launcher.dump_files(self.source)['unicorn_tw.cpk'][0].name, 'Unicorn_TW.cpk')
        self.assertTrue(vw_launcher.install_files(self.source, self.target))
        self.assertEqual(sorted(p.name for p in self.target.iterdir() if p.is_file()),
                         ['Unicorn.CPK', 'UnicornPatch.CPK', 'Unicorn_TW.CPK', 'eboot.bin'])
        self.assertEqual((self.target / 'Unicorn_TW.CPK').read_bytes(), b'update')
        self.assertEqual((self.target / 'UnicornPatch.CPK').read_bytes(), b'patch')


if __name__ == '__main__':
    unittest.main()

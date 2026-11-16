# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import paths
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402


class PortableSettings(unittest.TestCase):
    """launcher.json keeps the game folders relative to the program, so a moved copy still works."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.program = Path(self.temp.name).resolve() / 'Program'
        self.data = self.program / 'data'
        frozen = mock.patch.object(vw_launcher, 'FROZEN', True)
        frozen.start()
        self.addCleanup(frozen.stop)

    def written(self):
        return json.loads((self.data / 'launcher.json').read_text(encoding='utf-8'))

    def test_folders_inside_the_program_are_saved_relative(self):
        vw_launcher.save_settings(self.data, {'game_dir': str(self.data / 'game'),
                                              'source_dir': str(self.data / 'game' / 'CUSA36381'),
                                              'dump': 'english'})
        self.assertEqual(self.written(), {'game_dir': 'data/game', 'source_dir': 'data/game/CUSA36381',
                                          'dump': 'english'})

    def test_other_folders_stay_absolute(self):
        elsewhere = str(Path(self.temp.name).resolve() / 'Dump' / 'CUSA36381')
        vw_launcher.save_settings(self.data, {'source_dir': elsewhere})
        self.assertEqual(self.written(), {'source_dir': elsewhere})

    def test_relative_folders_are_read_from_the_program_folder(self):
        self.data.mkdir(parents=True)
        # Both spellings: what the launcher writes and what a hand edit gives.
        (self.data / 'launcher.json').write_text(
            json.dumps({'game_dir': 'data/game', 'source_dir': './data/game'}), encoding='utf-8')
        settings = vw_launcher.load_settings(self.data)
        self.assertEqual(Path(settings['game_dir']), self.program / 'data' / 'game')
        self.assertEqual(Path(settings['source_dir']), self.program / 'data' / 'game')
        self.assertTrue(Path(settings['game_dir']).is_absolute())

    def test_round_trip_does_not_change_the_file(self):
        vw_launcher.save_settings(self.data, {'game_dir': str(self.data / 'game')})
        before = self.written()
        vw_launcher.save_settings(self.data, vw_launcher.load_settings(self.data))
        self.assertEqual(self.written(), before)

    def test_unreadable_file_gives_no_settings(self):
        self.data.mkdir(parents=True)
        (self.data / 'launcher.json').write_text('[1, 2]', encoding='utf-8')
        self.assertEqual(vw_launcher.load_settings(self.data), {})

    def test_source_tree_keeps_paths_as_they_are(self):
        with mock.patch.object(vw_launcher, 'FROZEN', False):
            vw_launcher.save_settings(self.data, {'game_dir': str(self.data / 'game')})
            self.assertEqual(self.written(), {'game_dir': str(self.data / 'game')})


if __name__ == '__main__':
    unittest.main()

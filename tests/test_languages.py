# SPDX-License-Identifier: GPL-2.0-or-later
"""The text languages are listed in C (src/probe.c), C++ (gpu/platform/vw_graphics.h, the menu) and
Python (scripts/game_profiles.py); this checks that the lists agree."""
import re
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
from game_profiles import PROFILES

ROOT = Path(__file__).resolve().parent.parent


class LanguagesTest(unittest.TestCase):
    def setUp(self):
        probe = (ROOT / 'src/probe.c').read_text(encoding='utf-8')
        table = re.search(r'languages\[\] = \{(.*?)\};', probe, re.S).group(1)
        self.values = {key: int(value) for key, value in re.findall(r'\{"(\w+)", (\d+)\}', table)}
        self.archives = re.findall(r'"(\w*)"', re.search(r'archives\[\] = \{(.*?)\};', probe, re.S).group(1))
        header = (ROOT / 'gpu/platform/vw_graphics.h').read_text(encoding='utf-8')
        self.menu = re.findall(r'"(\w+)"', re.search(r'TextLanguages = \{(.*?)\};', header, re.S).group(1))
        self.slots = PROFILES['CUSA36381']['mod_archive_merge']['slots']

    def test_every_menu_language_is_known_to_the_probe(self):
        self.assertEqual(self.menu[0], 'auto')
        self.assertEqual(set(self.menu[1:]), set(self.values))

    def test_the_western_archives_match_the_game_values(self):
        for key, archive in self.slots.items():
            self.assertEqual(f'Unicorn_{self.archives[self.values[key]]}.CPK', archive, key)


if __name__ == '__main__':
    unittest.main()

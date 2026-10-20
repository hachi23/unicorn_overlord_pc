import tempfile
import unittest
from pathlib import Path

import paths
import sys
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402


class DumpKindTest(unittest.TestCase):
    """The English patch puts one English text file in both Chinese slots."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.game = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_english_patch(self):
        (self.game / 'Unicorn_CN.cpk').write_bytes(b'english' * 100)  # the patch's lower-case name
        (self.game / 'Unicorn_TW.CPK').write_bytes(b'english' * 100)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'english')

    def test_asia(self):
        (self.game / 'Unicorn_CN.CPK').write_bytes(b'simplified' * 70)
        (self.game / 'Unicorn_TW.CPK').write_bytes(b'traditional' * 70)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'asia')
        (self.game / 'Unicorn_TW.CPK').write_bytes(b'traditiona!' * 70)  # same size, other text
        (self.game / 'Unicorn_CN.CPK').write_bytes(b'traditional' * 70)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'asia')

    def test_languages_exist_in_the_game(self):
        import graphics_options  # noqa: F401  (scripts/ is on the path through vw_launcher)
        keys = {'auto', 'japanese', 'traditional_chinese', 'simplified_chinese', 'korean'}
        for dump in vw_launcher.GAMES['unicorn']['dumps'].values():
            for key, _ in dump['languages']:
                self.assertIn(key, keys)  # gpu/shim/vw_graphics.h TextLanguages


if __name__ == '__main__':
    unittest.main()

import tempfile
import unittest
from unittest import mock
from pathlib import Path

import paths
import sys
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402

ENGLISH = b'FMSB' + b'\0' * 28 + b'Save the game?\0Load this file?\0Return to the title screen?\0' * 20
CHINESE = b'FMSB' + b'\0' * 28 + '儲存遊戲嗎？\0讀取此檔案嗎？\0Lv\0'.encode() * 20


class DumpKindTest(unittest.TestCase):
    """The fan English patch puts English text in the Chinese text archives."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.game = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, name, text):
        """A CPK holding MsgSheet/UcSysMsg.fms, stored as is."""
        sys.path.insert(0, str(paths.ROOT / 'scripts'))
        import cpk
        blob = self.game / (name + '.blob')
        blob.write_bytes(text)
        source = mock.Mock(path=blob, files={'MsgSheet/UcSysMsg.fms': (0, len(text), len(text))})
        cpk.write(self.game / name, {'MsgSheet/UcSysMsg.fms': (source, 'MsgSheet/UcSysMsg.fms')})
        blob.unlink()

    def test_english_patch_v1(self):
        # The same file in both slots, under the patch's lower-case name.
        self.archive('Unicorn_cn.cpk', ENGLISH)
        self.archive('Unicorn_TW.CPK', ENGLISH)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'english')

    def test_english_patch_v2(self):
        # Other files in each slot (#58): v1 compared the two archives byte for byte.
        self.archive('Unicorn_CN.CPK', ENGLISH)
        self.archive('Unicorn_TW.CPK', ENGLISH.replace(b'Save', b'Keep'))
        self.assertEqual(vw_launcher.detect_dump(self.game), 'english')
        self.assertEqual(vw_launcher.english_text(self.game), {'cn', 'tw'})

    def test_english_in_one_slot(self):
        self.archive('Unicorn_CN.CPK', ENGLISH)
        self.archive('Unicorn_TW.CPK', CHINESE)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'english')
        self.assertEqual(vw_launcher.english_text(self.game), {'cn'})

    def test_asia(self):
        self.archive('Unicorn_CN.CPK', CHINESE)
        self.archive('Unicorn_TW.CPK', CHINESE)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'asia')

    def test_not_archives(self):
        (self.game / 'Unicorn_CN.CPK').write_bytes(b'english' * 100)
        (self.game / 'Unicorn_TW.CPK').write_bytes(b'english' * 100)
        self.assertEqual(vw_launcher.detect_dump(self.game), 'asia')
        self.assertEqual(vw_launcher.detect_dump(self.game / 'missing'), 'asia')

    def test_languages_exist_in_the_game(self):
        import graphics_options  # noqa: F401  (scripts/ is on the path through vw_launcher)
        keys = {'auto', 'japanese', 'traditional_chinese', 'simplified_chinese', 'korean'}
        for dump in vw_launcher.GAMES['unicorn']['dumps'].values():
            for key, _ in dump['languages']:
                self.assertIn(key, keys)  # gpu/shim/vw_graphics.h TextLanguages


if __name__ == '__main__':
    unittest.main()

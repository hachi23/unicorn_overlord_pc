import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import paths  # noqa: F401  (scripts/ on the path)
import patches


class EbootBuild(unittest.TestCase):
    """Issue #14: another build of v01.04 turned the code hooks off without a word."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.game = Path(self.temp.name)
        (self.game / 'eboot.bin').write_bytes(b'supported build')
        known = {'CUSA36381': (hashlib.sha256(b'supported build').hexdigest(),)}
        for patcher in (mock.patch.object(patches, 'SUPPORTED_EBOOTS', known),
                        mock.patch.object(patches, 'game_title_id', return_value='CUSA36381')):
            self.addCleanup(patcher.stop)
            started = patcher.start()
        self.title = started

    def tearDown(self):
        self.temp.cleanup()

    def test_the_supported_build(self):
        self.assertIs(patches.eboot_supported(self.game), True)

    def test_another_build(self):
        (self.game / 'eboot.bin').write_bytes(b'official Asia build')
        self.assertIs(patches.eboot_supported(self.game), False)

    def test_unknown_game_or_missing_eboot(self):
        self.title.return_value = 'CUSA00000'
        self.assertIsNone(patches.eboot_supported(self.game))
        self.title.return_value = 'CUSA36381'
        (self.game / 'eboot.bin').unlink()
        self.assertIsNone(patches.eboot_supported(self.game))



class KnownBuilds(unittest.TestCase):
    def test_the_hashes_are_v0104_hong_kong_105_and_retail_v0104(self):
        self.assertEqual([h[:8] for h in patches.SUPPORTED_EBOOTS['CUSA36381']],
                         ['de8e4970', 'fff894f5', '67c1d561'])


class SetupRefusesAnotherBuild(unittest.TestCase):
    """Issue #14: setup no longer offers to use another build of v01.04; it asks for another folder."""

    def setUp(self):
        import sys
        sys.path.insert(0, str(paths.ROOT / 'launcher'))
        import vw_launcher
        self.launcher = vw_launcher
        self.temp = tempfile.TemporaryDirectory()
        self.game = Path(self.temp.name)
        (self.game / 'eboot.bin').write_bytes(b'supported build')
        known = {'CUSA36381': (hashlib.sha256(b'supported build').hexdigest(),)}
        for patcher in (mock.patch.object(patches, 'SUPPORTED_EBOOTS', known),
                        mock.patch.object(vw_launcher, 'game_title_id', return_value='CUSA36381'),
                        mock.patch.object(patches, 'game_title_id', return_value='CUSA36381'),
                        mock.patch.object(vw_launcher, 'game_app_version', return_value='01.04')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def tearDown(self):
        self.temp.cleanup()

    def test_the_supported_build_is_accepted(self):
        ok, message = self.launcher.check_game(self.launcher.GAMES['unicorn'], str(self.game))
        self.assertTrue(ok, message)

    def test_a_known_build_of_another_version_is_accepted(self):
        with mock.patch.object(self.launcher, 'game_app_version', return_value='01.00'):  # Hong Kong
            ok, message = self.launcher.check_game(self.launcher.GAMES['unicorn'], str(self.game))
        self.assertTrue(ok, message)
        (self.game / 'eboot.bin').write_bytes(b'Asia 1.00 build')
        with mock.patch.object(self.launcher, 'game_app_version', return_value='01.00'):
            ok, message = self.launcher.check_game(self.launcher.GAMES['unicorn'], str(self.game))
        self.assertFalse(ok)
        self.assertIn('version 01.00', message)

    def test_another_build_is_refused(self):
        (self.game / 'eboot.bin').write_bytes(b'Hong Kong 1.05 build')
        ok, message = self.launcher.check_game(self.launcher.GAMES['unicorn'], str(self.game))
        self.assertFalse(ok)
        self.assertIn('cannot run it', message)
        self.assertNotIn('anyway', message)


if __name__ == '__main__':
    unittest.main()

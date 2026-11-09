import os
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import prepare_cache


class PrepareCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.game, self.scripts, self.out = root / 'game', root / 'scripts', root / 'out'
        for folder in (self.game / 'sce_module', self.game / 'sce_sys', self.scripts, self.out):
            folder.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'eboot')
        (self.game / 'sce_module' / 'libc.prx').write_bytes(b'libc')
        (self.game / 'sce_sys' / 'param.sfo').write_bytes(b'sfo')
        (self.scripts / 'prepare.py').write_text('# v1\n', encoding='utf-8')
        for name in prepare_cache.OUTPUTS:
            (self.out / name).write_bytes(b'x')

    def tearDown(self):
        self.temp.cleanup()

    def key(self, sku='full'):
        return prepare_cache.key(self.game, self.scripts, sku)

    def test_unchanged_game_is_skipped_until_something_it_reads_changes(self):
        self.assertFalse(prepare_cache.valid(self.out, self.key()))
        prepare_cache.store(self.out, self.key())
        self.assertTrue(prepare_cache.valid(self.out, self.key()))

        self.assertFalse(prepare_cache.valid(self.out, self.key(sku='trial')))
        (self.scripts / 'prepare.py').write_text('# v2\n', encoding='utf-8')
        self.assertFalse(prepare_cache.valid(self.out, self.key()))

    def test_changed_module_or_missing_output_prepares_again(self):
        prepare_cache.store(self.out, self.key())
        (self.game / 'sce_module' / 'libc.prx').write_bytes(b'libc, other size')
        self.assertFalse(prepare_cache.valid(self.out, self.key()))

        prepare_cache.store(self.out, self.key())
        (self.out / 'boot-linked.bin').unlink()
        self.assertFalse(prepare_cache.valid(self.out, self.key()))

    def test_a_view_of_the_same_game_with_other_archives_is_the_same_image(self):
        (self.game / 'Unicorn_TW.CPK').write_bytes(b'chinese')
        prepare_cache.store(self.out, self.key())
        view = Path(self.temp.name) / 'mod-game-1234'
        view.mkdir()
        for entry in self.game.iterdir():
            if entry.name != 'Unicorn_TW.CPK':
                (view / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
        (view / 'Unicorn_US.CPK').write_bytes(b'english, written at this start')
        self.assertTrue(prepare_cache.valid(self.out, prepare_cache.key(view, self.scripts, 'full')))
        (self.game / 'eboot.bin').write_bytes(b'another eboot')
        self.assertFalse(prepare_cache.valid(self.out, prepare_cache.key(view, self.scripts, 'full')))

    def test_forget_and_environment_override(self):
        prepare_cache.store(self.out, self.key())
        prepare_cache.forget(self.out)
        self.assertFalse(prepare_cache.valid(self.out, self.key()))

        prepare_cache.store(self.out, self.key())
        os.environ['VW_PREPARE_ALWAYS'] = '1'
        try:
            self.assertFalse(prepare_cache.valid(self.out, self.key()))
        finally:
            del os.environ['VW_PREPARE_ALWAYS']

    def test_device_name_is_computed_once_per_vwport(self):
        probe = self.out / 'vwport'
        probe.write_bytes(b'v1')
        calls = []
        compute = lambda: calls.append(1) or 'AMD Radeon (RADV POLARIS11)'  # noqa: E731
        self.assertEqual(prepare_cache.cached_device_name(self.out, probe, compute), 'AMD Radeon (RADV POLARIS11)')
        self.assertEqual(prepare_cache.cached_device_name(self.out, probe, compute), 'AMD Radeon (RADV POLARIS11)')
        self.assertEqual(len(calls), 1)
        probe.write_bytes(b'version 2')
        prepare_cache.cached_device_name(self.out, probe, compute)
        self.assertEqual(len(calls), 2)


if __name__ == '__main__':
    unittest.main()

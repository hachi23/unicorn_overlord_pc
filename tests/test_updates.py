import hashlib
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import paths  # noqa: F401
import updates

ZIP = updates.ASSETS['windows']


def release(tag, assets=(ZIP, updates.SUMS), draft=False, body=''):
    return {'tag_name': tag, 'draft': draft, 'body': body,
            'assets': [{'name': name, 'browser_download_url': f'https://example.invalid/{tag}/{name}', 'size': 7}
                       for name in assets]}


class VersionTest(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(updates.parse_version('v0.1.0'), (0, 1, 0, 0))
        self.assertEqual(updates.parse_version('v1.12.3-4-gabc1234'), (1, 12, 3, 4))
        self.assertEqual(updates.parse_version('v1.2.3-4-gabc1234-dirty'), (1, 2, 3, 4))
        for text in ('latest', 'f4cc431', '', 'unknown', '1.2.3', 'v1.2'):
            self.assertIsNone(updates.parse_version(text), text)

    def test_newer(self):
        self.assertTrue(updates.is_newer('v0.2.0', 'v0.1.0'))
        self.assertTrue(updates.is_newer('v0.1.1', 'v0.1.0-12-gabcdef0'))
        self.assertFalse(updates.is_newer('v0.1.0', 'v0.1.0-12-gabcdef0'))  # a later build of it
        self.assertFalse(updates.is_newer('v0.1.0', 'v0.1.0'))
        self.assertFalse(updates.is_newer('v0.9.9', 'v1.0.0'))
        self.assertTrue(updates.is_newer('v0.1.0', 'f4cc431'))  # a test build: every release is newer
        self.assertFalse(updates.is_newer('latest', 'v0.1.0'))
        self.assertTrue(updates.is_newer('v1.6.1-shadps4', 'v1.6.0-shadps4-3-gabcdef0'))
        self.assertFalse(updates.is_newer('v1.6.0-shadps4', 'v1.6.0-shadps4-3-gabcdef0'))

    def test_pick_release(self):
        releases = [release('latest'), release('v0.3.0', draft=True), release('v0.2.0', body='notes'),
                    release('v0.2.5', assets=(ZIP,)), release('v0.1.5')]
        best = updates.pick_release(releases, 'v0.1.0', asset=ZIP)
        self.assertEqual((best['tag'], best['notes']), ('v0.2.0', 'notes'))
        self.assertEqual(best['url'], f'https://example.invalid/v0.2.0/{ZIP}')
        self.assertTrue(best['sums_url'].endswith('/SHA256SUMS'))
        self.assertIsNone(updates.pick_release(releases, 'v0.1.0', skipped='v0.2.0', asset=ZIP))
        self.assertIsNone(updates.pick_release(releases, 'v0.2.0', asset=ZIP))
        self.assertIsNone(updates.pick_release(releases, 'v0.1.0', asset='other.AppImage'))

    def test_sums(self):
        digest = 'a' * 64
        text = f'{"b" * 64}  other.zip\n{digest}  {ZIP}\nnot a line\n'
        self.assertEqual(updates.expected_sha256(text, ZIP), digest)
        self.assertEqual(updates.expected_sha256(f'{digest} *{ZIP}\n', ZIP), digest)
        self.assertIsNone(updates.expected_sha256(text, 'missing.zip'))


class FilesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_download_checks_the_hash(self):
        source = self.root / 'source.bin'
        source.write_bytes(b'x' * 3_000_000)
        good = hashlib.sha256(source.read_bytes()).hexdigest()
        seen = []
        target = self.root / 'out.bin'
        self.assertTrue(updates.download(source.as_uri(), target, good, lambda d, t: seen.append(d)))
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(seen[-1], 3_000_000)
        target.unlink()
        with self.assertRaises(ValueError):
            updates.download(source.as_uri(), target, '0' * 64)
        self.assertEqual(list(self.root.iterdir()), [source])  # nothing kept: no file, no .part
        self.assertFalse(updates.download(source.as_uri(), target, good, cancelled=lambda: True))
        self.assertFalse(target.exists())

    def test_unpack(self):
        archive = self.root / ZIP
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Unicorn Overlord PC/Unicorn Overlord.exe', 'new exe')
            z.writestr('Unicorn Overlord PC/_internal/runtime/VERSION', 'v0.2.0\n')
        program = updates.unpack(archive, self.root / 'update' / 'v0.2.0')
        self.assertEqual(program.name, 'Unicorn Overlord PC')
        self.assertEqual((program / 'Unicorn Overlord.exe').read_text(), 'new exe')
        evil = self.root / 'evil.zip'
        with zipfile.ZipFile(evil, 'w') as z:
            z.writestr('../outside.txt', 'no')
        with self.assertRaises(ValueError):
            updates.unpack(evil, self.root / 'update' / 'evil')
        self.assertFalse((self.root / 'update' / 'outside.txt').exists())

    def program(self, folder, text):
        (folder / '_internal' / 'runtime').mkdir(parents=True)
        (folder / 'Unicorn Overlord.exe').write_text(text)
        (folder / '_internal' / 'runtime' / 'VERSION').write_text(text)
        (folder / 'LICENSE').write_text('license')

    def test_install_keeps_data(self):
        target, new = self.root / 'install', self.root / 'new'
        self.program(target, 'old')
        (target / '_internal' / 'stale.dll').write_text('stale')
        (target / 'data' / 'user').mkdir(parents=True)
        (target / 'data' / 'vw.ini').write_text('vsync=0\n')
        self.program(new, 'new')
        updates.install(new, target)
        self.assertEqual((target / 'Unicorn Overlord.exe').read_text(), 'new')
        self.assertEqual((target / '_internal' / 'runtime' / 'VERSION').read_text(), 'new')
        self.assertFalse((target / '_internal' / 'stale.dll').exists())
        self.assertEqual((target / 'data' / 'vw.ini').read_text(), 'vsync=0\n')
        self.assertFalse((target / '.update-old').exists())

    def test_install_rolls_back(self):
        target, new = self.root / 'install', self.root / 'new'
        self.program(target, 'old')
        (target / 'data').mkdir()
        self.program(new, 'new')
        copy = updates.shutil.copy2
        calls = []

        def failing_copy(source, destination, **kwargs):
            calls.append(source)
            if len(calls) == 2:
                raise OSError('disk full')
            return copy(source, destination, **kwargs)
        with mock.patch.object(updates.shutil, 'copy2', failing_copy), self.assertRaises(OSError):
            updates.install(new, target)
        self.assertEqual((target / 'Unicorn Overlord.exe').read_text(), 'old')
        self.assertEqual((target / '_internal' / 'runtime' / 'VERSION').read_text(), 'old')
        self.assertTrue((target / 'data').is_dir())
        self.assertFalse((target / '.update-old').exists())

    def test_appimage(self):
        running, new = self.root / 'Game.AppImage', self.root / 'Game.AppImage.new'
        running.write_text('old')
        new.write_text('new')
        updates.replace_appimage(new, running)
        self.assertEqual(running.read_text(), 'new')
        self.assertTrue(os.access(running, os.X_OK) or os.name == 'nt')

    def test_setting(self):
        ini = self.root / 'vw.ini'
        with mock.patch.dict(os.environ, {'VW_UPDATES': '1'}):
            self.assertTrue(updates.setting_enabled(ini))  # no file: on
            ini.write_text('show_fps=0\ncheck_updates=0\n')
            self.assertFalse(updates.setting_enabled(ini))
            ini.write_text('check_updates=1\n')
            self.assertTrue(updates.setting_enabled(ini))
        with mock.patch.dict(os.environ, {'VW_UPDATES': '0'}):
            self.assertFalse(updates.setting_enabled(ini))


if __name__ == '__main__':
    unittest.main()

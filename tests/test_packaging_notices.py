# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
import tarfile
from unittest.mock import patch
from paths import ROOT

spec = importlib.util.spec_from_file_location('notices', ROOT / 'packaging/notices.py')
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)
package_spec = importlib.util.spec_from_file_location('port_package', ROOT / 'packaging/package.py')
package = importlib.util.module_from_spec(package_spec)
with patch.dict(sys.modules, {'notices': notices}):
    package_spec.loader.exec_module(package)


class NoticeTests(unittest.TestCase):
    def test_tcl_windows_fallback_and_unknown_series(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            folder = root / 'packaging/licenses'
            folder.mkdir(parents=True)
            source = folder / 'Tcl-8.6.txt'
            source.write_text('upstream 8.6 license terms')
            with patch.object(notices.Path, 'glob', return_value=[]):
                self.assertEqual(notices.tcl_notice('tcl', '8.6.18', root / 'tcl8.6', root), source)
                with self.assertRaises(FileNotFoundError):
                    notices.tcl_notice('tcl', '99.0.0', root / 'tcl99.0', root)

    def test_supported_tcl_tk_series_have_upstream_notices(self):
        with patch.object(notices.Path, 'glob', return_value=[]):
            for name in ('tcl', 'tk'):
                for version in ('8.6.18', '9.0.4'):
                    source = notices.tcl_notice(name, version, Path('/missing'))
                    self.assertIn('The authors hereby grant permission', source.read_text())

    def test_source_archive_excludes_untracked_game_data(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'runtime.c').write_text('/* source */')
            subprocess.run(['git', 'add', 'runtime.c'], cwd=root, check=True)
            subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                            'commit', '-qm', 'test source'], cwd=root, check=True)
            (root / 'eboot.bin').write_bytes(b'private game data')
            (root / 'data').mkdir()
            (root / 'data/save.bin').write_bytes(b'private save')
            (root / 'src-private-game').mkdir()
            (root / 'src-private-game/eboot.bin').write_bytes(b'private game data')
            (root / 'out/deps/src').mkdir(parents=True)
            (root / 'out/deps/src/private-game.tar.gz').write_bytes(b'private game archive')
            destination = root / 'source.tar.gz'
            with patch.object(package, 'ROOT', root), patch.object(package, 'OUT', root / 'out'):
                package.source_archive(destination)
            with tarfile.open(destination) as archive:
                self.assertIn('source/runtime.c', archive.getnames())
                self.assertFalse(any('eboot' in p or 'save.bin' in p for p in archive.getnames()))
                self.assertFalse(any('private-game' in p for p in archive.getnames()))

    def test_missing_component_fails_instead_of_shipping_incomplete_notices(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(FileNotFoundError):
                notices.collect_native(root / 'dist', root)

    def test_all_native_components_and_inline_notices_are_collected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = list(notices.VENDORED.values())
            paths += [f'out/deps/build/{name}/LICENSE' for name in notices.STATIC]
            paths += ['packaging/licenses/Apache-2.0.txt',
                      'gpu/platform/shaders/ffx_a.h', 'gpu/platform/shaders/ffx_fsr1.h']
            for source in paths:
                path = root / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f'original notice: {source}')
            destination = root / 'dist'
            notices.collect_native(destination, root)
            self.assertEqual((destination / 'LibAtrac9.txt').read_text(),
                             'original notice: third_party/LibAtrac9/LICENSE')
            self.assertTrue((destination / 'ffmpeg/LICENSE').is_file())
            self.assertTrue((destination / 'headers/Apache-2.0.txt').is_file())
            self.assertTrue((destination / 'ffx_a.h').is_file())

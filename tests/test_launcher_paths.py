# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import paths
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402
import run


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
        here = mock.patch.object(vw_launcher, 'HERE', self.program)
        here.start()
        self.addCleanup(here.stop)

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

    def test_nested_data_override_paths_are_relative_to_program(self):
        with mock.patch.dict(os.environ, {'VW_DATA_DIR': 'profiles/data', 'APPIMAGE': ''}):
            data = vw_launcher.data_dir(vw_launcher.GAMES['unicorn'])
            folder = data / 'game'
            vw_launcher.save_settings(data, {'game_dir': str(folder)})
            stored = json.loads((data / 'launcher.json').read_text(encoding='utf-8'))
            self.assertEqual(stored, {'game_dir': 'profiles/data/game'})
            self.assertEqual(vw_launcher.load_settings(data)['game_dir'], str(folder))

    def test_nested_data_override_migrates_same_layout(self):
        game_info = vw_launcher.GAMES['unicorn']
        with mock.patch.dict(os.environ, {'VW_DATA_DIR': 'profiles/data', 'APPIMAGE': '', 'VW_INSTALL': '1'}):
            data = vw_launcher.data_dir(game_info)
            suffix = Path('profiles/data/game')
            old = self.program.with_name('Old') / suffix
            local = self.program / suffix
            data.mkdir(parents=True)
            config = data / 'launcher.json'
            config.write_text(json.dumps({'game_dir': str(old), 'source_dir': str(old)}), encoding='utf-8')
            with mock.patch.object(vw_launcher, 'check_game', return_value=(True, '')):
                settings = vw_launcher.load_settings(data, game_info, persist_migration=True)
            self.assertEqual(Path(settings['game_dir']), local)
            self.assertEqual(json.loads(config.read_text(encoding='utf-8')),
                             {'game_dir': suffix.as_posix(), 'source_dir': suffix.as_posix()})

    def test_round_trip_does_not_change_the_file(self):
        vw_launcher.save_settings(self.data, {'game_dir': str(self.data / 'game')})
        before = self.written()
        vw_launcher.save_settings(self.data, vw_launcher.load_settings(self.data))
        self.assertEqual(self.written(), before)

    def test_existing_absolute_config_migrates_to_valid_local_install(self):
        old = self.program.with_name('Old') / 'data' / 'game'
        local = self.data / 'game'
        dump = str(self.program.with_name('External') / 'CUSA36381')
        old.mkdir(parents=True)
        (old.parent / 'launcher.json').write_text('{}', encoding='utf-8')
        self.data.mkdir(parents=True)
        (self.data / 'launcher.json').write_text(
            json.dumps({'game_dir': str(old), 'source_dir': dump, 'english_mode': 1}), encoding='utf-8')
        with mock.patch.dict(os.environ, {'VW_INSTALL': '1'}), \
                mock.patch.object(vw_launcher, 'check_game', return_value=(True, '')) as validate:
            settings = vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn'], persist_migration=True)
            validate.assert_called_once_with(vw_launcher.GAMES['unicorn'], local)
            self.assertEqual(vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn']), settings)
            self.assertEqual(validate.call_count, 1)
        self.assertEqual(settings['game_dir'], str(local))
        self.assertEqual(self.written(), {'game_dir': 'data/game', 'source_dir': dump, 'english_mode': 1})

    def test_diagnostics_uses_relocated_path_without_writing_settings(self):
        from types import SimpleNamespace

        old = self.program.with_name('Old') / 'data' / 'game'
        local = self.data / 'game'
        self.data.mkdir(parents=True)
        config = {'game_dir': str(old)}
        (self.data / 'launcher.json').write_text(json.dumps(config), encoding='utf-8')
        root = SimpleNamespace(destroy=mock.Mock())
        env = {'VW_DATA_DIR': str(self.data), 'APPIMAGE': '', 'VW_INSTALL': '1'}
        with mock.patch.dict(os.environ, env), mock.patch.object(sys, 'argv', ['launcher', '--export-diagnostics']), \
                mock.patch.object(vw_launcher, 'tk_root', return_value=root), \
                mock.patch.object(vw_launcher, 'export_diagnostics') as export, \
                mock.patch.object(vw_launcher, 'check_game', return_value=(True, '')):
            with self.assertRaises(SystemExit) as stop:
                vw_launcher.main('unicorn')
        self.assertEqual(stop.exception.code, 0)
        export.assert_called_once_with(root, self.data, str(local))
        self.assertEqual(self.written(), config)

    def test_migration_write_failure_keeps_the_relocated_path(self):
        old = self.program.with_name('Old') / 'data' / 'game'
        local = self.data / 'game'
        self.data.mkdir(parents=True)
        config = {'game_dir': str(old)}
        (self.data / 'launcher.json').write_text(json.dumps(config), encoding='utf-8')
        with mock.patch.dict(os.environ, {'VW_INSTALL': '1'}), \
                mock.patch.object(vw_launcher, 'check_game', return_value=(True, '')), \
                mock.patch.object(vw_launcher, 'save_settings', side_effect=PermissionError('read-only config')) as save:
            settings = vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn'], persist_migration=True)
        save.assert_called_once()
        self.assertEqual(settings['game_dir'], str(local))
        self.assertEqual(self.written(), config)

    def test_invalid_local_install_leaves_legacy_config_untouched(self):
        old = self.program.with_name('Old') / 'data' / 'game'
        self.data.mkdir(parents=True)
        config = {'game_dir': str(old)}
        (self.data / 'launcher.json').write_text(json.dumps(config), encoding='utf-8')
        with mock.patch.dict(os.environ, {'VW_INSTALL': '1'}), \
                mock.patch.object(vw_launcher, 'check_game', return_value=(False, '')):
            self.assertEqual(vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn']), config)
        self.assertEqual(self.written(), config)

    def test_external_game_path_is_not_redirected_to_portable_copy(self):
        external = str(self.program.with_name('External') / 'CUSA36381')
        vw_launcher.save_settings(self.data, {'game_dir': external})
        with mock.patch.object(vw_launcher, 'check_game') as validate:
            settings = vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn'])
        validate.assert_not_called()
        self.assertEqual(settings['game_dir'], external)

    def test_external_dump_with_managed_suffix_stays_external(self):
        external = self.program.with_name('External') / 'data' / 'game'
        external.mkdir(parents=True)
        self.data.mkdir(parents=True)
        config = {'game_dir': str(external), 'source_dir': str(external)}
        (self.data / 'launcher.json').write_text(json.dumps(config), encoding='utf-8')
        with mock.patch.dict(os.environ, {'VW_INSTALL': '1'}), \
                mock.patch.object(vw_launcher, 'check_game', return_value=(True, '')):
            settings = vw_launcher.load_settings(self.data, vw_launcher.GAMES['unicorn'])
        self.assertEqual(settings, config)
        self.assertEqual(self.written(), config)

    def test_appimage_anchors_relative_paths_outside_the_mount(self):
        class BeforeChdir(Exception):
            pass

        mounted_exe = self.program.with_name('Mount') / 'usr' / 'bin' / 'Unicorn Overlord'
        env = {'APPIMAGE': str(self.program / 'Unicorn.AppImage'),
               'VW_GAME_DIR': 'data/game', 'VW_DATA_DIR': 'data'}
        with mock.patch.dict(os.environ, env), mock.patch.object(sys, 'argv', ['run.py']), \
                mock.patch.object(sys, 'frozen', True, create=True), \
                mock.patch.object(sys, 'executable', str(mounted_exe)), \
                mock.patch.object(run.os, 'chdir', side_effect=BeforeChdir):
            self.assertEqual(vw_launcher.data_dir(vw_launcher.GAMES['unicorn']), self.data)
            with self.assertRaises(BeforeChdir):
                run.main()
            self.assertEqual(Path(os.environ['VW_DATA_DIR']), self.data)
            self.assertEqual(Path(os.environ['VW_GAME_DIR']), self.data / 'game')

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

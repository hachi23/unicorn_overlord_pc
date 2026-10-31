from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('bbmods', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA36381'
        self.assets = self.game / 'game_data' / 'data'
        self.assets.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'original executable')
        (self.assets / 'a.wad').write_bytes(b'original')
        (self.assets / 'b.wad').write_bytes(b'untouched')
        self.moddir = self.root / 'mods'

    def mod(self, name, contents=b'mod', file='a.wad'):
        root = self.moddir / name
        folder = root / 'game_data' / 'data'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / file).write_bytes(contents)
        return root

    def test_merge_replacement_new_file_and_directory_listing(self):
        a = self.mod('A')
        self.mod('A', b'new', 'new.wad')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'game_data' / 'data'
        self.assertEqual((folder / 'a.wad').read_bytes(), b'mod')
        self.assertEqual((folder / 'b.wad').read_bytes(), b'untouched')
        self.assertEqual((folder / 'new.wad').read_bytes(), b'new')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.wad', 'b.wad', 'new.wad'})
        self.assertEqual((self.assets / 'a.wad').read_bytes(), b'original')
        self.assertFalse((self.assets / 'new.wad').exists())
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')

    def test_last_mod_wins_and_reordering_changes_winner(self):
        a, b = self.mod('A', b'A'), self.mod('B', b'B')
        for layers, winner in [([('A', a), ('B', b)], b'B'), ([('B', b), ('A', a)], b'A')]:
            result = mods.build_overlay(self.game, self.root / 'out', layers)
            self.assertEqual((result / 'game_data/data/a.wad').read_bytes(), winner)

    def test_selection_disable_and_new_mod(self):
        for name in ['C', 'B', 'A']:
            self.mod(name)
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['B', 'A', 'deleted'], 'disabled': ['A']}))
        self.assertEqual(mods.selected(self.moddir, config, mods.game_names(self.game)), ['B', 'C'])

    def test_no_mod_returns_original_game(self):
        self.assertEqual(mods.build_overlay(self.game, self.root / 'out', []), self.game)
        self.assertFalse((self.root / 'out').exists())

    def test_directory_conflict_does_not_modify_base(self):
        a = self.mod('A')
        (a / 'game_data/data/a.wad').unlink()
        nested = a / 'game_data/data/b.wad'
        nested.mkdir()
        (nested / 'child').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((self.assets / 'b.wad').read_bytes(), b'untouched')
        self.assertEqual(list((self.root / 'out').iterdir()), [])

    def test_mod_symlinks_rejected(self):
        a = self.mod('A')
        try:
            (a / 'game_data/escape').symlink_to(self.assets, target_is_directory=True)
        except OSError:  # Windows without Developer Mode or admin
            self.skipTest('symlinks unavailable')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def archive(self, path, files):
        """A CPK of `files` (name -> bytes), stored uncompressed."""
        source = mods.cpk.Memory()
        for name, data in files.items():
            source.add(name, data)
        mods.cpk.write(path, {name: (source, name) for name in files})

    def test_switch_text_archive_merged_into_language_archives(self):
        merge = {'archives': ('Unicorn_US.CPK',), 'into': ('Unicorn_TW.CPK', 'Unicorn_CN.CPK'), 'skip': ('.ftx',)}
        for name in ('Unicorn_TW.CPK', 'Unicorn_CN.cpk'):
            self.archive(self.game / name, {'GUI/Interface.mbs': b'ps4 ui', 'MsgSheet/UcSysMsg.fms': b'old text'})
        for name, text in (('A', b'switch text'), ('B', b'later text')):
            (self.moddir / name).mkdir(parents=True)
            self.archive(self.moddir / name / 'Unicorn_US.CPK',
                         {'MsgSheet/UcSysMsg.fms': text, 'MsgSheet/UcNew.fms': b'new', 'Help/Help.ftx': b'nvt'})
        with mock.patch.object(mods, 'archive_merge', return_value=merge):
            names = mods.game_names(self.game)
            self.assertEqual(mods.discover(self.moddir, names), ['A', 'B'])
            result = mods.build_overlay(self.game, self.root / 'out', [('A', self.moddir / 'A'), ('B', self.moddir / 'B')])
        for name in ('Unicorn_TW.CPK', 'Unicorn_CN.cpk'):
            archive = mods.cpk.Archive(result / name)
            self.assertEqual(set(archive.files), {'GUI/Interface.mbs', 'MsgSheet/UcSysMsg.fms', 'MsgSheet/UcNew.fms'})
            self.assertEqual(archive.read('MsgSheet/UcSysMsg.fms'), b'later text')
            self.assertEqual(archive.read('GUI/Interface.mbs'), b'ps4 ui')
            self.assertEqual(mods.cpk.Archive(self.game / name).read('MsgSheet/UcSysMsg.fms'), b'old text')
        self.assertFalse((result / 'Unicorn_US.CPK').exists())

    def test_translation_sheets_merged_into_language_archives(self):
        import fms
        merge = {'archives': (), 'into': ('Unicorn_TW.CPK', 'Unicorn_CN.CPK'), 'folders': ('MsgSheet',)}
        template = fms.build(['', '', ''], b'FMSB' + bytes(4) + (0x20).to_bytes(4, 'little') + bytes(8)
                             + (3).to_bytes(4, 'little') + (3).to_bytes(4, 'little') + bytes(4))
        sheet = fms.build(['Options', 'Music Volume', 'Off'], template)
        for name in ('Unicorn_TW.CPK', 'Unicorn_CN.cpk'):
            self.archive(self.game / name, {'MsgSheet/UcOption.fms': sheet, 'GUI/Interface.mbs': b'ui'})
        a, b = self.moddir / 'A' / 'MsgSheet', self.moddir / 'B' / 'MsgSheet'
        a.mkdir(parents=True)
        b.mkdir(parents=True)
        (a / 'UcOption.json').write_text('{"0": "Opciones", "2": "No"}', encoding='utf-8')
        (a / 'README.txt').write_text('ignored')
        (b / 'ucoption.json').write_text('{"1": "Volumen de la música"}', encoding='utf-8')
        with mock.patch.object(mods, 'archive_merge', return_value=merge):
            names = mods.game_names(self.game)
            self.assertEqual(mods.discover(self.moddir, names), ['A', 'B'])
            result = mods.build_overlay(self.game, self.root / 'out', [('A', self.moddir / 'A'), ('B', self.moddir / 'B')])
        for name in ('Unicorn_TW.CPK', 'Unicorn_CN.cpk'):
            archive = mods.cpk.Archive(result / name)
            self.assertEqual(set(archive.files), {'MsgSheet/UcOption.fms', 'GUI/Interface.mbs'})
            self.assertEqual(fms.parse(archive.read('MsgSheet/UcOption.fms')), ['Opciones', 'Volumen de la música', 'No'])
            self.assertEqual(fms.parse(mods.cpk.Archive(self.game / name).read('MsgSheet/UcOption.fms')),
                             ['Options', 'Music Volume', 'Off'])

    def test_translation_entry_out_of_range_rejected(self):
        import fms
        merge = {'archives': (), 'into': ('Unicorn_TW.CPK',), 'folders': ('MsgSheet',)}
        template = b'FMSB' + bytes(4) + (0x20).to_bytes(4, 'little') + bytes(8) + (1).to_bytes(4, 'little') + bytes(8)
        self.archive(self.game / 'Unicorn_TW.CPK', {'MsgSheet/UcOption.fms': fms.build(['Options'], template)})
        folder = self.moddir / 'A' / 'MsgSheet'
        folder.mkdir(parents=True)
        (folder / 'UcOption.json').write_text('{"5": "x"}', encoding='utf-8')
        with mock.patch.object(mods, 'archive_merge', return_value=merge), self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', self.moddir / 'A')])

    def test_executable_replacement_rejected(self):
        a = self.mod('A')
        (a / 'eboot.bin').write_bytes(b'unsupported')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_shadps4_sibling_folder_and_global_disable(self):
        legacy = Path(str(self.game) + '-mods')
        folder = legacy / 'game_data/data'
        folder.mkdir(parents=True)
        (folder / 'a.wad').write_bytes(b'legacy')
        for enabled, expected in [('1', b'legacy'), ('0', b'original')]:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir), '--enabled', enabled],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'game_data/data/a.wad').read_bytes(), expected)

    def test_app0_wrapped_mod_discovered(self):
        self.moddir.mkdir()
        (self.moddir / 'Wrapped/app0/game_data').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir, mods.game_names(self.game)), ['Wrapped'])

    def test_other_case_replaces_the_game_file(self):
        root = self.moddir / 'Upper'
        (root / 'GAME_DATA/Data').mkdir(parents=True)
        (root / 'GAME_DATA/Data/A.WAD').write_bytes(b'upper')
        result = mods.build_overlay(self.game, self.root / 'out', [('Upper', root)])
        folder = result / 'game_data/data'
        self.assertEqual((folder / 'a.wad').read_bytes(), b'upper')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.wad', 'b.wad'})
        self.assertEqual({p.name for p in result.iterdir()}, {'game_data', 'eboot.bin'})

    def test_wrapped_layouts(self):
        layouts = {'Archive': 'Archive v1.2/game_data/data', 'Nested': 'Nested/app0/game_data/data'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.wad').write_bytes(name.encode())
        (self.moddir / 'Archive/readme.txt').write_text('notes')
        (self.moddir / 'Junk/stuff').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir, mods.game_names(self.game)), ['Archive', 'Nested'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'game_data/data/a.wad').read_bytes(), name.encode())

    def test_top_level_archive_replaced_and_system_files_refused(self):
        (self.game / 'Data.CPK').write_bytes(b'original archive')
        root = self.moddir / 'Archive'
        root.mkdir(parents=True)
        (root / 'data.cpk').write_bytes(b'mod archive')
        (root / 'readme.txt').write_text('notes')
        result = mods.build_overlay(self.game, self.root / 'out', [('Archive', root)])
        self.assertEqual((result / 'Data.CPK').read_bytes(), b'mod archive')
        self.assertFalse((result / 'readme.txt').exists())
        (root / 'eboot.bin').write_bytes(b'patched')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('Archive', root)])

    def test_invalid_profile_fails(self):
        self.mod('A')
        config = self.root / 'mods.json'
        config.write_text('{"disabled":"A"}')
        with self.assertRaises(ValueError):
            mods.selected(self.moddir, config, mods.game_names(self.game))

    def test_run_uses_overlay_propagates_exit_and_cleans_view(self):
        self.mod('A')
        probe = self.root / 'probe'
        probe.write_text('import json,sys,os\nfrom pathlib import Path\n'
            'game=Path(sys.argv[sys.argv.index("--app0")+1])\n'
            'Path(os.environ["VW_DATA_DIR"],"mounted.json").write_text(json.dumps({\n'
            '"path":str(game),"content":(game/"game_data/data/a.wad").read_text()}))\n'
            'sys.exit(7)\n')
        env = dict(VW_PROBE=str(probe), VW_DATA_DIR=str(self.root), VW_GAME_DIR=str(self.game),
            VW_MODS_DIR=str(self.moddir), VW_MODS_ENABLED='1', VW_MODS_CONFIG=str(self.root/'mods.json'))
        spec = importlib.util.spec_from_file_location('vwrun', ROOT / 'run.py')
        run = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(run)
        real_script = run.run_script
        # The fake game has no eboot to prepare: only the mods step runs.
        run.run_script = lambda name, *args, **kw: real_script(name, *args, **kw) if name == 'mods.py' else None
        cwd = os.getcwd()
        self.addCleanup(os.chdir, cwd)
        real_run = run.LOG.run
        def launch(command, **kwargs):
            # The fake probe is Python, not a native executable. Windows cannot
            # execute a shebang script directly; retain a real child process on both OSes.
            if command[0] == str(probe):
                command = [sys.executable, *command]
            return real_run(command, **kwargs)
        with mock.patch.dict(os.environ, env), mock.patch.object(sys, 'argv', ['run.py']), \
             mock.patch.object(run.LOG, 'run', side_effect=launch):
            self.assertEqual(run.entrypoint(), 7)
        mounted = json.loads((self.root / 'mounted.json').read_text())
        self.assertEqual(mounted['content'], 'mod')
        self.assertFalse(Path(mounted['path']).exists())
        self.assertEqual((self.assets/'a.wad').read_bytes(), b'original')

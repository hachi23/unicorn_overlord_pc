import json
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import graphics_options as g


class GraphicsOptionsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ini = Path(self.temp.name) / 'vw.ini'
        self.state = Path(self.temp.name) / 'state.json'

    def tearDown(self):
        self.temp.cleanup()

    def test_defaults_and_invalid_values(self):
        self.ini.write_text('render_resolution=8K\naspect_ratio=5:4\nrenderer=shadps4\n', encoding='utf-8')
        options, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(options, {'render_resolution': 'native', 'aspect_ratio': '16:9'})

    def test_write_keys_keeps_other_lines(self):
        self.ini.write_text('# comment\nshow_fps=1\nrcas=1\n', encoding='utf-8')
        g.write_keys(self.ini, {'rcas': '0', 'vsync': '0'})
        self.assertEqual(self.ini.read_text(encoding='utf-8'), '# comment\nshow_fps=1\nrcas=0\nvsync=0\n')

    def test_unconfirmed_restart_is_tried_once_then_reverted(self):
        self.ini.write_text('render_resolution=1800p\naspect_ratio=16:9\n', encoding='utf-8')
        good, _ = g.prepare(self.ini, self.state)  # a normal, confirmed launch
        g.write_keys(self.ini, {'render_resolution': '4K', 'aspect_ratio': '21:9', 'restart_unconfirmed': '1'})
        tried, reverted = g.prepare(self.ini, self.state)  # the launch of "Apply and restart"
        self.assertFalse(reverted)
        self.assertEqual((tried['render_resolution'], tried['aspect_ratio']), ('4K', '21:9'))
        back, reverted = g.prepare(self.ini, self.state)  # it never confirmed: back to the good ones
        self.assertTrue(reverted)
        self.assertEqual(back, good)
        ini = g.read_ini(self.ini)
        self.assertEqual((ini['render_resolution'], ini['restart_unconfirmed']), ('1800p', '0'))

    def test_restart_from_an_attempt_confirms_it(self):
        self.ini.write_text('render_resolution=native\n', encoding='utf-8')
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'render_resolution': '720p', 'restart_unconfirmed': '1'})
        g.prepare(self.ini, self.state, restarted=True)  # 720p, unconfirmed
        # Its menu asks for another restart before (or after) its own confirmation.
        g.write_keys(self.ini, {'render_resolution': '900p', 'restart_unconfirmed': '1'})
        tried, reverted = g.prepare(self.ini, self.state, restarted=True)
        self.assertFalse(reverted)
        self.assertEqual(tried['render_resolution'], '900p')
        self.assertEqual(json.loads(self.state.read_text())['last_good']['render_resolution'], '720p')
        # 900p never reaches the game: a manual start goes back to 720p, not native.
        back, reverted = g.prepare(self.ini, self.state)
        self.assertTrue(reverted)
        self.assertEqual(back['render_resolution'], '720p')

    def test_confirmed_restart_becomes_the_last_good(self):
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'render_resolution': '1800p', 'restart_unconfirmed': '1'})
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'restart_unconfirmed': '0'})  # the game confirmed
        g.prepare(self.ini, self.state)
        self.assertEqual(json.loads(self.state.read_text())['last_good']['render_resolution'], '1800p')


if __name__ == '__main__':
    unittest.main()

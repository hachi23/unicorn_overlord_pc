import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sys

import paths
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402


class RendererChoiceTest(unittest.TestCase):
    def test_asks_only_when_the_build_has_the_shadps4_renderer(self):
        name = 'vwport.exe' if vw_launcher.WINDOWS else 'vwport'
        with tempfile.TemporaryDirectory() as temp:
            runtime = Path(temp)
            with mock.patch.object(vw_launcher, 'RUNTIME', runtime):
                self.assertFalse(vw_launcher.has_shadps4())
                (runtime / 'bin' / 'shadps4').mkdir(parents=True)
                (runtime / 'bin' / 'shadps4' / name).write_bytes(b'')
                self.assertTrue(vw_launcher.has_shadps4())


if __name__ == '__main__':
    unittest.main()

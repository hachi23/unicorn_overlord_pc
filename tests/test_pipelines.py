"""tools/native/pipelines.py: the pipelines.txt lines the renderer writes, merged into the
header it compiles at start, and read back from that header unchanged."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools' / 'native'))
import pipelines  # noqa: E402


class PipelinesTests(unittest.TestCase):
    def test_recorded_lines_round_trip_through_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            recorded = Path(tmp) / 'pipelines.txt'
            # As src/native_vk.c writes them: hex hashes, blend, mask; decimal topology.
            recorded.write_text('11023290 658258bb 61010104 f 3\n'
                                'not a pipeline\n'
                                '0000beef 0000cafe 00000000 7 4\n'
                                '11023290 658258bb 61010104 f 3\n')
            keys = pipelines.read_recorded(recorded)
            self.assertEqual(keys, {(0x11023290, 0x658258bb, 0x61010104, 0xf, 3),
                                    (0x0000beef, 0x0000cafe, 0, 7, 4)})
            header = Path(tmp) / 'native_pipelines.h'
            header.write_text(pipelines.render(keys))
            self.assertEqual(pipelines.read_header(header), keys)
            self.assertIn('{0x0000beef, 0x0000cafe, 0x00000000, 0x7, 4},', header.read_text())

    def test_an_empty_list_still_compiles(self):
        self.assertIn('{0, 0, 0, 0, 0}', pipelines.render(set()))


if __name__ == '__main__':
    unittest.main()

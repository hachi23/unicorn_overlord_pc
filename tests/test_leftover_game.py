import sys
import unittest

import paths
sys.path.insert(0, str(paths.ROOT / 'launcher'))
import vw_launcher  # noqa: E402


class LeftoverGame(unittest.TestCase):
    """Issue #35: a game closed with Alt+F4 stayed running, and every later start failed for memory."""

    def test_tasklist_lists_the_running_copies(self):
        output = ('\r\n"vwport.exe","5240","Console","1","6,312,448 K"\r\n'
                  '"VWPORT.EXE","812","Console","1","20,112 K"\r\n')
        self.assertEqual(vw_launcher.parse_tasklist(output), [5240, 812])

    def test_tasklist_without_a_copy(self):
        # Printed (in the system's language) when nothing matches the filter.
        self.assertEqual(vw_launcher.parse_tasklist('INFO: No tasks are running which match the specified criteria.\r\n'), [])
        self.assertEqual(vw_launcher.parse_tasklist('"python.exe","100","Console","1","9,000 K"\n'), [])

    def test_memory_failure_is_explained(self):
        log = ('[    1.620] STOP: cannot create 6080 MiB of guest memory (error 1450): '
               'too little free memory and page file\n[    1.811] Exit code 21\n')
        self.assertEqual(vw_launcher.failure_hint(log), vw_launcher.MEMORY_HINT)
        self.assertIn('vwport.exe', vw_launcher.MEMORY_HINT)

    def test_other_failures_get_no_hint(self):
        self.assertEqual(vw_launcher.failure_hint('[    2.000] Fault: read access to 0000000000000010\n'), '')

    def test_gpu_feature_hint(self):
        log = ('[    0.846] GPU [Debug] <Critical> vk_instance.cpp:271 CreateDevice: Assertion Failed!\n'
               'Required Vulkan feature unavailable: sparseBinding and sparseResidencyBuffer (sparse buffers)\n'
               '[    0.846] STOP: GPU library assertion failed (see GPU log above)\n')
        self.assertEqual(vw_launcher.failure_hint(log), vw_launcher.GPU_HINT)


if __name__ == '__main__':
    unittest.main()

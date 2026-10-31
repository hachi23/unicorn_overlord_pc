import struct
import sys
import unittest

import paths
sys.path.insert(0, str(paths.ROOT / 'scripts'))
import cpk  # noqa: E402


def crilayla(head, codes):
    """A CRILAYLA stream: `codes` in the order the decoder reads them (a byte for a literal, or
    (distance, length) with length 3..5), the output filled from its end."""
    bits = []

    def put(value, width):
        bits.extend((value >> i) & 1 for i in reversed(range(width)))

    size = 0
    for code in codes:
        if isinstance(code, int):
            put(0, 1)
            put(code, 8)
            size += 1
        else:
            back, length = code
            put(1, 1)
            put(back - 3, 13)
            put(length - 3, 2)
            size += length
    bits.extend([0] * (-len(bits) % 8))
    stream = bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))[::-1]
    return b'CRILAYLA' + struct.pack('<II', size, len(stream)) + stream + head, size


class CrilaylaTest(unittest.TestCase):
    def test_literals_and_back_reference(self):
        head = bytes(range(256))
        # Read backwards: 'f', 'e', 'd', then three bytes copied from three ahead.
        data, size = crilayla(head, [ord('f'), ord('e'), ord('d'), (3, 3), ord('!')])
        self.assertEqual(size, 7)
        self.assertEqual(cpk.crilayla(data), head + b'!defdef')

    def test_plain_data_unchanged(self):
        self.assertEqual(cpk.crilayla(b'FMSB text'), b'FMSB text')

    def test_bad_reference_rejected(self):
        data, _ = crilayla(bytes(256), [ord('a'), (5, 3)])
        with self.assertRaises(ValueError):
            cpk.crilayla(data)

    def test_cut_short_rejected(self):
        data, _ = crilayla(bytes(256), [ord('a')])
        with self.assertRaises(ValueError):
            cpk.crilayla(data[:-10])


if __name__ == '__main__':
    unittest.main()

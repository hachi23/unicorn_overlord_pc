from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import struct
import unittest

import ftx


def ps4_template(width, height, data_format=38):
    """A PS4 FTEX: FTEX/FTX0 headers, a GNF header with a tile mode 13 T#, zero blocks."""
    size = width * height // 2 if data_format == 38 else width * height
    header = bytearray(0xC0)
    header[:4] = b'FTEX'
    struct.pack_into('<I', header, 0x04, size + 0x140)
    struct.pack_into('<I', header, 0x44, size + 0x100)
    header[0x80:0x84] = b'FTX0'
    struct.pack_into('<I', header, 0x84, size + 0x100)
    gnf = bytearray(0x100)
    gnf[:4] = b'GNF '
    struct.pack_into('<I', gnf, 0x0C, size + 0x100)
    struct.pack_into('<8I', gnf, 0x10, 0, data_format << 20, (width - 1) | ((height - 1) << 14),
                     13 << 20, (width - 1) << 13, 0, 0, size)
    return bytes(header + gnf) + bytes(size) + b'TRAILER.........'


def switch_ftex(width, height, block):
    """A Switch FTEX (.texI) whose block (x, y) is block(x, y), in Tegra block-linear order."""
    bw, bh = width // 4, height // 4
    size = bw * bh * 8
    data = bytearray(size)
    gobs = (bw * 8 + 63) // 64
    for y in range(bh):
        for x in range(bw):
            at = ftx._tegra_offset(x, y, 8, gobs, ftx._block_height(bh))
            data[at:at + 8] = block(x, y)
    tex = bytearray(0x100)
    tex[:5] = b'.texI'
    struct.pack_into('<II', tex, 0x0C, width, height)
    struct.pack_into('<II', tex, 0x1C, 0x100, size)
    header = bytearray(0xC0)
    header[:4] = b'FTEX'
    return bytes(header + tex) + bytes(data)


class FtxTests(unittest.TestCase):
    def test_switch_blocks_land_at_their_ps4_tiled_places(self):
        block = lambda x, y: bytes([x, y]) + b'BC4...'
        out = ftx.convert(switch_ftex(64, 128, block), ps4_template(64, 128))
        gnf = out.find(b'GNF ')
        self.assertEqual(gnf, 0xC0)
        words = struct.unpack_from('<8I', out, gnf + 0x10)
        self.assertEqual(((words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1), (64, 128))
        self.assertEqual(((words[4] >> 13) & 0x3FFF) + 1, 64)  # pitch
        size = 16 * 32 * 8
        self.assertEqual(words[7], size)
        self.assertEqual(struct.unpack_from('<I', out, 0x04)[0], size + 0x140)
        self.assertEqual(out[-16:], b'TRAILER.........')
        data = out[gnf + 0x100:gnf + 0x100 + size]
        # Tile mode 13: 8x8-block micro tiles in rows, Morton order inside.
        for (x, y), index in {(0, 0): 0, (1, 0): 1, (0, 1): 2, (2, 0): 4, (7, 7): 63, (8, 0): 64,
                              (0, 8): 128, (9, 9): 64 * 3 + 3}.items():
            self.assertEqual(data[index * 8:index * 8 + 8], block(x, y), (x, y))

    def test_half_size_font_is_doubled_to_the_ps4_size(self):
        # One BC4 block: endpoints 200, 10; pixel (row r, column c) has index (r * 4 + c) % 8.
        bits = sum(((r * 4 + c) % 8) << (3 * (r * 4 + c)) for r in range(4) for c in range(4))
        block = bytes([200, 10]) + bits.to_bytes(6, 'little')
        out = ftx.convert(switch_ftex(32, 32, lambda x, y: block), ps4_template(64, 64))
        gnf = out.find(b'GNF ')
        words = struct.unpack_from('<8I', out, gnf + 0x10)
        self.assertEqual(((words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1), (64, 64))
        self.assertEqual(words[7], 16 * 16 * 8)
        # Output block (1, 0) (Morton index 1) is the source's top right quarter doubled.
        top_right = out[gnf + 0x100 + 8:gnf + 0x100 + 16]
        self.assertEqual(top_right[:2], bytes([200, 10]))
        indices = int.from_bytes(top_right[2:], 'little')
        got = [[(indices >> (3 * (r * 4 + c))) & 7 for c in range(4)] for r in range(4)]
        self.assertEqual(got, [[2, 2, 3, 3], [2, 2, 3, 3], [6, 6, 7, 7], [6, 6, 7, 7]])

    def test_ps4_files_and_unknown_layouts_are_refused(self):
        with self.assertRaises(ValueError):
            ftx.convert(ps4_template(64, 64), ps4_template(64, 64))
        with self.assertRaises(ValueError):
            ftx.convert(switch_ftex(32, 32, lambda x, y: bytes(8)), ps4_template(64, 64, data_format=10))


if __name__ == '__main__':
    unittest.main()

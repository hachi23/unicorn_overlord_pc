from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import struct
import unittest

import ftx
import glyphs
from test_ftx import ps4_template

CELL_W, CELL_H, COLUMNS = 130, 154, 31
HEADER = bytes.fromhex('0010000801008200 9a00'.replace(' ', ''))  # 4096x2048, 130x154 cells
PAGE_AT = 0x1000  # the page 0 records of the two tables, 0x800 bytes each


def elf_with_tables():
    """An ELF (one loadable segment at vaddr 0) with two 31-column glyph tables. Page 0: glyph
    code - 0x20 for ASCII, advance = right ink edge - 20 (see atlas), byte 3 25; 0xC0 'À' as
    glyph 105 with advance 50, byte 3 27."""
    data = bytearray(0x3000)
    data[:4] = b'\x7fELF'
    struct.pack_into('<Q', data, 0x20, 0x40)
    struct.pack_into('<HH', data, 0x36, 0x38, 1)
    struct.pack_into('<IIQQQQQQ', data, 0x40, 1, 5, 0, 0, 0, len(data), len(data), 0x1000)
    for table in range(2):
        at, page = 0x200 + table * 0x820, PAGE_AT + table * 0x800
        data[at:at + 4] = b'\1\0\0\0'
        data[at + 4:at + 4 + len(HEADER)] = HEADER
        struct.pack_into('<Q', data, at + 0x20, page)
        for code in range(0x21, 0x7F):
            struct.pack_into('<HBB', data, page + code * 8, code - 0x20, right_edge(code - 0x20) - 20, 25)
        struct.pack_into('<HBB', data, page + 0x20 * 8, 0, 18, 25)
        struct.pack_into('<HBB', data, page + 0xC0 * 8, 105, 50, 27)
    return bytes(data)


def left_edge(index):  # the ink's left edge in the cell (it varies, so the fit can tell it apart)
    return 8 + (index % 5) * 4 - (index % COLUMNS * CELL_W) % 4


def right_edge(index):
    return left_edge(index) + 24 + (index % 7) * 8 + 3


def atlas(indices):
    """A 4096x2048 BC4 font whose glyphs `indices` are white boxes from left_edge to right_edge."""
    out = bytearray(ps4_template(4096, 2048))
    data = out.find(b'GNF ') + 0x100
    micro_row = (4096 // 4) >> 3
    for index in indices:
        x, y = index % COLUMNS * CELL_W, index // COLUMNS * CELL_H
        for by in range(y // 4 + 8, y // 4 + 20):
            for bx in range((x + left_edge(index)) // 4, (x + right_edge(index)) // 4 + 1):
                at = data + (((by >> 3) * micro_row + (bx >> 3)) * 64 + ftx._morton(bx & 7, by & 7)) * 8
                out[at:at + 8] = b'\xff\xff' + bytes(6)
    return bytes(out)


def record(writes, table, code):
    page = PAGE_AT + table * 0x800
    for vaddr, data in writes:
        if vaddr <= page + code * 8 < vaddr + len(data):
            at = page + code * 8 - vaddr
            return struct.unpack_from('<HBB', data, at) + (data[at + 4:at + 8],)
    return None


class GlyphTests(unittest.TestCase):
    def test_latin1_font_records_point_to_its_cells(self):
        ascii_cells = range(1, 0x5F)
        latin1 = atlas(list(ascii_cells) + list(range(0xA1 - 0x20, 0x100 - 0x20)))
        stock = atlas(list(ascii_cells) + list(range(95, 200)))  # Â (glyph 95) drawn
        writes, notes = glyphs.writes(elf_with_tables(), {'AlbertusNovaFont': latin1, 'CongenialFont': stock})
        self.assertEqual(notes, ['AlbertusNovaFont (Latin-1 order)'])
        # À (Windows-1251 А) -> glyph 160, its advance from the ASCII fit, byte 3 kept.
        self.assertEqual(record(writes, 0, 0xC0), (160, right_edge(160) - 20, 27, bytes(4)))
        self.assertEqual(record(writes, 0, 0xFF)[:2], (223, right_edge(223) - 20))
        self.assertEqual(record(writes, 0, 0xA0)[:2], (0, 18))  # no-break space: a space
        self.assertEqual(record(writes, 0, 0x97)[:2], (ord('-') - 0x20, right_edge(ord('-') - 0x20) - 20))
        self.assertIsNone(record(writes, 1, 0xC0))  # the stock-order font keeps its table

    def test_no_writes_without_the_tables(self):
        latin1 = atlas(list(range(1, 0x5F)) + list(range(0x81, 0xE0)))
        self.assertEqual(glyphs.writes(b'\x7fELF' + bytes(0x100), {'AlbertusNovaFont': latin1}), ([], []))


if __name__ == '__main__':
    unittest.main()

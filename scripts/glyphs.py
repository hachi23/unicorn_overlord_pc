#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Glyph tables for Switch fonts drawn in another order (the Russian Switch translation, #84).

The game finds a character's glyph through a table in the eboot, one per font: a 0x20-byte
header (u32 1; u16 atlas width, height; u16 ?, cell width, cell height, ...) and 256 page
pointers (character >> 8), each page 256 eight-byte records (character & 0xFF): u16 glyph
index, u8 advance, u8 (follows the case), 4 bytes set for ASCII only. Glyph i is cell
(i % columns, i // columns) of the atlas, columns = atlas width // cell width. Three tables
serve the Western text languages (1-5): AlbertusNovaFont, CongenialFont (4096x2048, 130x154
cells, 31 columns) and AlbertusNovaFont_L (4096x4096, 265x306, 15 columns).

The stock atlases hold ASCII and then the French, German, Spanish and Italian letters in an
order of their own, then symbols. The Russian translation's fonts hold ASCII where the stock
ones do and then Latin-1 order (its text is Windows-1251 bytes read as Latin-1, so its
Cyrillic is U+00C0-U+00FF): glyph = character - 0x20 in the 31-column fonts and
character - 0x40 in the 15-column one, where the cells between are blank. Its patched Switch
executable changes the tables to match; this does the same for the PS4 one: for each such
font (Latin1Layout), the records of U+00A0-U+00FF point to its cells, with advances measured
from the glyphs (fitted to the font's own ASCII advances against where their ink is).
A few Windows-1251 punctuation bytes the text uses take ASCII glyphs (the fonts have no
cells for them).
"""
import struct

# The Western text languages' tables: header bytes 0x04-0x0D (atlas width, height, ?, cell
# width, height), and the font of each match in eboot order (v01.04, Hong Kong 1.05).
TABLES = {
    bytes.fromhex('0010000801008200 9a00'.replace(' ', '')): ('AlbertusNovaFont', 'CongenialFont'),
    bytes.fromhex('0010001001000901 3201'.replace(' ', '')): ('AlbertusNovaFont_L',),
}
# Windows-1251 punctuation without cells in these fonts -> the ASCII character drawn instead.
FALLBACKS = {0x84: '"', 0x91: "'", 0x92: "'", 0x93: '"', 0x94: '"', 0x96: '-', 0x97: '-'}
INK = 96  # a pixel at least this bright is ink


def segments(elf):
    """(vaddr, file offset, file size) of the ELF's loadable segments."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    size, count = struct.unpack_from('<HH', elf, 0x36)
    out = []
    for i in range(count):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * size)
        if kind == 1:
            out.append((vaddr, offset, filesz))
    return out


def file_offset(elf_segments, vaddr, length):
    for start, offset, size in elf_segments:
        if start <= vaddr and vaddr + length <= start + size:
            return offset + vaddr - start
    return None


def tables(elf):
    """{font name: (geometry (atlas width, height, cell width, cell height), page 0 vaddr)} of
    the TABLES found in `elf`."""
    found, elf_segments = {}, segments(elf)
    for header, names in TABLES.items():
        at, matches = elf.find(header), []
        while at >= 0:
            start = at - 4
            if start % 8 == 0 and elf[start:start + 4] == b'\1\0\0\0':
                page = struct.unpack_from('<Q', elf, start + 0x20)[0]
                if file_offset(elf_segments, page, 0x800) is not None:
                    width, height, _, cell_w, cell_h = struct.unpack_from('<5H', header)
                    matches.append(((width, height, cell_w, cell_h), page))
            at = elf.find(header, at + 1)
        if len(matches) == len(names):
            found.update(zip(names, matches))
    return found


class Atlas:
    """The pixels of a PS4 BC4 FTEX (tile mode 13), read a block at a time."""

    def __init__(self, ftx):
        gnf = ftx.find(b'GNF ')
        words = struct.unpack_from('<8I', ftx, gnf + 0x10)
        if (words[1] >> 20) & 0x3F != 38 or (words[3] >> 20) & 0x1F != 13:
            raise ValueError('not a BC4 tile mode 13 texture')
        self.width, self.height = (words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1
        self.data = memoryview(ftx)[gnf + 0x100:gnf + 0x100 + words[7]]
        self.micro_row = (self.width // 4) >> 3

    def _block(self, x, y):
        from ftx import _morton
        at = (((y >> 3) * self.micro_row + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * 8
        return self.data[at:at + 8]

    def _column_ink(self, block):
        """The 4 columns of a BC4 block: whether each has a pixel of at least INK."""
        a, b = block[0], block[1]
        if max(a, b) < INK:
            return 0
        if a > b:
            palette = [a, b] + [((7 - i) * a + i * b) // 7 for i in range(1, 7)]
        else:
            palette = [a, b] + [((5 - i) * a + i * b) // 5 for i in range(1, 5)] + [0, 255]
        bits, mask = int.from_bytes(block[2:8], 'little'), 0
        for pixel in range(16):
            if palette[(bits >> (3 * pixel)) & 7] >= INK:
                mask |= 1 << (pixel & 3)
        return mask

    def span(self, x, y, width, height):
        """The first and last inked pixel columns of the rectangle (relative to x), or None when it
        is blank. x and width are multiples of 4."""
        rows = range(y // 4, (y + height + 3) // 4)

        def column(bx):  # the block column's ink, one bit per pixel column
            mask = 0
            for by in rows:
                mask |= self._column_ink(self._block(bx, by))
            return mask

        blocks = range(x // 4, (x + width) // 4)
        for bx in blocks:
            mask = column(bx)
            if mask:
                left = bx * 4 + (mask & -mask).bit_length() - 1 - x
                break
        else:
            return None
        for bx in reversed(blocks):
            mask = column(bx)
            if mask:
                return left, bx * 4 + mask.bit_length() - 1 - x


def _cell(atlas, geometry, index):
    """The ink span (left, right) of glyph `index` in pixels from its cell's left edge, or None."""
    width, _, cell_w, cell_h = geometry
    columns = width // cell_w
    x, y = (index % columns) * cell_w, (index // columns) * cell_h
    left = x // 4 * 4
    span = atlas.span(left, y, (x + cell_w - left) // 4 * 4, cell_h)
    return span and (span[0] + left - x, span[1] + left - x)


def _fit(points):
    """Least squares advance = a * left + b * right + c over (left, right, advance) points."""
    sums = [[0.0] * 4 for _ in range(3)]
    for left, right, advance in points:
        row = (left, right, 1.0)
        for i in range(3):
            for j in range(3):
                sums[i][j] += row[i] * row[j]
            sums[i][3] += row[i] * advance
    for i in range(3):  # Gauss-Jordan on the 3x4 normal equations
        pivot = max(range(i, 3), key=lambda r: abs(sums[r][i]))
        sums[i], sums[pivot] = sums[pivot], sums[i]
        for r in range(3):
            if r != i:
                f = sums[r][i] / sums[i][i]
                sums[r] = [v - f * w for v, w in zip(sums[r], sums[i])]
    return [sums[i][3] / sums[i][i] for i in range(3)]


def latin1_first(atlas, geometry):
    """The glyph of U+0020 when the atlas is in Latin-1 order (0x20 or 0x40, see the module
    notes), else None (the stock order: its cell of Â, glyph 95, is drawn)."""
    if _cell(atlas, geometry, 95):
        return None
    for first in (0x20, 0x40):
        if all(_cell(atlas, geometry, code - first) for code in (0xC0, 0xD0, 0xE0, 0xFF)):
            return first
    return None


def writes(elf, fonts):
    """The record writes (vaddr, bytes) for the fonts in `fonts` ({name: PS4 FTEX}) in Latin-1
    order, and a note per font."""
    out, notes, elf_segments = [], [], segments(elf)
    for name, (geometry, page) in tables(elf).items():
        if name not in fonts:
            continue
        try:
            atlas = Atlas(fonts[name])
        except ValueError:
            continue
        if (atlas.width, atlas.height) != geometry[:2]:
            continue
        first = latin1_first(atlas, geometry)
        if first is None:
            continue
        base = file_offset(elf_segments, page, 0x800)
        records = [bytearray(elf[base + c * 8:base + c * 8 + 8]) for c in range(256)]
        # Advance = a * left + b * right + c from the glyph's ink in its cell, fitted to the
        # font's ASCII (its glyphs are the stock ones, placed in their cells the same way).
        points = []
        for c in range(0x21, 0x7F):
            span = _cell(atlas, geometry, records[c][0] | records[c][1] << 8)
            if span and records[c][2]:
                points.append((*span, records[c][2]))
        a, b, c = _fit(points)
        for code in range(0xA0, 0x100):
            glyph = code - first
            span = None if code == 0xA0 else _cell(atlas, geometry, glyph)
            if span:
                advance = max(1, min(255, round(a * span[0] + b * span[1] + c)))
            else:  # no-break space, or a blank cell: a space
                glyph, advance = records[0x20][0] | records[0x20][1] << 8, records[0x20][2]
            # Byte 3 stays: it follows the case, which Windows-1251 keeps where Latin-1 has it.
            records[code][:3] = struct.pack('<HB', glyph, advance)
            records[code][4:] = bytes(4)
        for code, character in FALLBACKS.items():
            records[code][:] = records[ord(character)]
        out.append((page + 0x80 * 8, b''.join(bytes(r) for r in records[0x80:])))
        notes.append(f'{name} (Latin-1 order)')
    return out, notes

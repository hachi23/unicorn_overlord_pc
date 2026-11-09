#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Switch FTEX textures (.ftx with an NVT '.texI' image) as PS4 ones (a GNF image), so a Switch
mod's fonts or pictures can be used (#84).

An FTEX file: a 0xC0-byte header (FTEX, FTX0) and one image. The PS4 image is a 0x100-byte GNF
header with a T# (texture descriptor) and the blocks in tile mode 13 (8x8 blocks Morton-ordered
per micro tile). The Switch image is a '.texI' header (width, height at +0x0C, data at +0x100,
size at +0x20; '.texM' for the pictures) and the blocks in Tegra block-linear order. Both use the same block formats (BC4
for fonts, BC7 for the tutorial pictures), so converting moves blocks without decoding them.
The Switch textures are half the PS4 size. The fonts must have the PS4 size (the game places the
glyphs in pixels of it): a BC4 font is doubled block by block (each pixel's index repeated 2x2,
the same endpoints; the game draws these 4K fonts smaller anyway). A picture keeps its size.

    ftx.py <switch.ftx> <ps4 template.ftx> <out.ftx>
"""
from pathlib import Path
import struct
import sys

FTEX_HEADER = 0xC0
GNF_HEADER = 0x100
# GNF data formats: bytes per 4x4 block.
BLOCK_BYTES = {35: 8, 36: 16, 37: 16, 38: 8, 39: 16, 40: 16, 41: 16}


def is_switch(data):
    return data[:4] == b'FTEX' and data[FTEX_HEADER:FTEX_HEADER + 4] == b'.tex'  # .texI, .texM


def _morton(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def _tegra_offset(x, y, bpe, gobs_per_row, block_height):
    """Byte offset of block (x, y) in Tegra block-linear order (GOB 64 bytes x 8 rows)."""
    xb = x * bpe
    rows = 8 * block_height
    return ((y // rows) * gobs_per_row * block_height * 512 + (xb // 64) * block_height * 512
            + ((y % rows) // 8) * 512 + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16))


# BC4 doubling: a 12-bit row of four 3-bit indices -> the 12-bit row of its left (0) or right (1)
# two pixels, each repeated.
_DOUBLE = [[sum(((row >> (3 * (half * 2 + i // 2))) & 7) << (3 * i) for i in range(4))
            for row in range(4096)] for half in (0, 1)]


def _double_bc4(block):
    """The four BC4 blocks (top left, top right, bottom left, bottom right) of `block` doubled."""
    bits = int.from_bytes(block[2:8], 'little')
    rows = [(bits >> (12 * r)) & 0xFFF for r in range(4)]
    out = []
    for top in (0, 2):
        for half in (0, 1):
            a, b = _DOUBLE[half][rows[top]], _DOUBLE[half][rows[top + 1]]
            out.append(bytes(block[:2]) + (a | a << 12 | b << 24 | b << 36).to_bytes(6, 'little'))
    return out


def _block_height(rows_of_blocks):
    """The Tegra block height (in GOBs) the Switch tools pick for this many block rows."""
    height = 1
    while height < 16 and height * 8 < rows_of_blocks:
        height *= 2
    return height


def convert(switch, template):
    """The PS4 FTEX for the Switch FTEX `switch`, with the headers of the game's PS4 file
    `template` of the same name (same block format)."""
    if not is_switch(switch):
        raise ValueError('not a Switch FTEX')
    gnf = template.find(b'GNF ')
    if template[:4] != b'FTEX' or gnf != FTEX_HEADER:
        raise ValueError('template is not a PS4 FTEX')
    tex = FTEX_HEADER
    width, height = struct.unpack_from('<II', switch, tex + 0x0C)
    offset, size = struct.unpack_from('<II', switch, tex + 0x1C)
    words = list(struct.unpack_from('<8I', template, gnf + 0x10))
    data_format = (words[1] >> 20) & 0x3F
    bpe = BLOCK_BYTES.get(data_format)
    if not bpe:
        raise ValueError(f'GNF data format {data_format} is not a block format')
    if (words[3] >> 20) & 0x1F != 13:
        raise ValueError('template is not in tile mode 13')
    bw, bh = (width + 3) // 4, (height + 3) // 4
    if bw % 8 or bh % 8 or size != bw * bh * bpe or tex + offset + size > len(switch):
        raise ValueError(f'unexpected Switch image {width}x{height}, {size} bytes')
    source = memoryview(switch)[tex + offset:tex + offset + size]
    gobs_per_row = (bw * bpe + 63) // 64
    block_height = _block_height(bh)
    ps4_width, ps4_height = (words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1
    double = data_format == 38 and (ps4_width, ps4_height) == (2 * width, 2 * height)
    if double:
        width, height, size = ps4_width, ps4_height, size * 4
    out = bytearray(size)
    micro_row = (width // 4) >> 3
    for y in range(bh):
        for x in range(bw):
            s = _tegra_offset(x, y, bpe, gobs_per_row, block_height)
            if double:
                for i, block in enumerate(_double_bc4(source[s:s + 8])):
                    X, Y = 2 * x + (i & 1), 2 * y + (i >> 1)
                    d = (((Y >> 3) * micro_row + (X >> 3)) * 64 + _morton(X & 7, Y & 7)) * 8
                    out[d:d + 8] = block
            else:
                d = (((y >> 3) * micro_row + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * bpe
                out[d:d + bpe] = source[s:s + bpe]
    old = words[7]
    words[2] = (words[2] & ~0xFFFFFFF) | (width - 1) | ((height - 1) << 14)
    words[4] = (words[4] & ~(0x3FFF << 13)) | ((width - 1) << 13)  # pitch
    words[7] = size
    header = bytearray(template[:gnf + GNF_HEADER])
    struct.pack_into('<8I', header, gnf + 0x10, *words)
    # The sizes the FTEX/FTX0/GNF headers hold, each the image size plus a constant.
    for at in (0x04, 0x44, 0x84, gnf + 0x0C):
        value = struct.unpack_from('<I', header, at)[0]
        if value >= old:
            struct.pack_into('<I', header, at, value - old + size)
    return bytes(header) + bytes(out) + template[gnf + GNF_HEADER + old:]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    Path(sys.argv[3]).write_bytes(convert(Path(sys.argv[1]).read_bytes(), Path(sys.argv[2]).read_bytes()))


if __name__ == '__main__':
    main()

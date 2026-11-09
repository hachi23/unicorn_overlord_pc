#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""nso_extract.py <main NSO> <out folder>: the text, ro and data segments of a Switch NSO
(LZ4 blocks decompressed) as text.bin, ro.bin, data.bin, with their load addresses."""
import struct, sys
def lz4(src, size):
    out = bytearray(); i = 0
    while i < len(src):
        tok = src[i]; i += 1
        n = tok >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255: break
        out += src[i:i+n]; i += n
        if i >= len(src): break
        off = src[i] | src[i+1] << 8; i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        m += 4
        for _ in range(m): out.append(out[-off])
    assert len(out) == size, (len(out), size)
    return bytes(out)
d = open(sys.argv[1], 'rb').read()
flags = struct.unpack_from('<I', d, 0xC)[0]
csz = struct.unpack_from('<3I', d, 0x60)
for k, (name, at) in enumerate((('text', 0x10), ('ro', 0x20), ('data', 0x30))):
    fo, mo, sz = struct.unpack_from('<3I', d, at)
    seg = d[fo:fo+csz[k]]
    seg = lz4(seg, sz) if flags & (1 << k) else seg
    open(f'{sys.argv[2]}/{name}.bin', 'wb').write(seg)
    print(name, hex(mo), hex(sz))

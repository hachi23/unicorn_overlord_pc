# SPDX-License-Identifier: GPL-2.0-or-later
"""uo_raw2png.py <output_WxH.raw> [out.png]: a frame dump (BGRA) as PNG, without extra modules."""
import sys, re, zlib, struct
src = sys.argv[1]; dst = sys.argv[2] if len(sys.argv) > 2 else src[:-4] + '.png'
w, h = map(int, re.search(r'_(\d+)x(\d+)\.raw$', src).groups())
d = open(src, 'rb').read()
rows = b''.join(b'\0' + bytes(x for i in range(w) for x in (d[(y*w+i)*4+2], d[(y*w+i)*4+1], d[(y*w+i)*4])) for y in range(h))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 1)) + chunk(b'IEND', b''))
print(dst)

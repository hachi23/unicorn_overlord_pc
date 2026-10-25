"""Minimal CRI CPK reader: list / extract (with CRILAYLA decompression).
usage: cpk.py list FILE.cpk [filter]
       cpk.py extract FILE.cpk OUTDIR [filter]
"""
import os, struct, sys


def unxor(b):
    out = bytearray(b); m = 0x655f
    for i in range(len(out)):
        out[i] ^= m & 0xff; m = (m * 0x4115) & 0xffff
    return bytes(out)


def utf(buf):
    if buf[:4] != b'@UTF':
        buf = unxor(buf)
    assert buf[:4] == b'@UTF', buf[:4]
    size, = struct.unpack('>I', buf[4:8])
    t = buf[8:8 + size]
    rows_off, str_off, data_off, name_off, ncol, rowlen, nrow = struct.unpack('>IIIIHHI', t[:24])
    rows_off &= 0xffff

    def s(o):
        e = t.index(b'\0', str_off + o); return t[str_off + o:e].decode('utf-8', 'replace')
    fmt = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f', 0xa: '>I', 0xb: '>II'}

    def val(typ, p):
        f = fmt[typ]; n = struct.calcsize(f); v = struct.unpack(f, t[p:p + n])
        if typ == 0xa: v = s(v[0])
        elif typ == 0xb: v = t[data_off + v[0]:data_off + v[0] + v[1]]
        else: v = v[0]
        return v, n
    cols = []; p = 24
    for _ in range(ncol):
        fl = t[p]; nm = s(struct.unpack('>I', t[p + 1:p + 5])[0]); p += 5
        const = None
        if fl & 0xf0 in (0x30, 0x70):
            const, n = val(fl & 0xf, p); p += n
        cols.append((fl, nm, const))
    rows = []
    for r in range(nrow):
        p = rows_off + r * rowlen; row = {}
        for fl, nm, const in cols:
            st = fl & 0xf0
            if st == 0x50:
                row[nm], n = val(fl & 0xf, p); p += n
            elif st in (0x30, 0x70): row[nm] = const
            else: row[nm] = None
        rows.append(row)
    return rows


def crilayla(d):
    usize, hoff = struct.unpack('<II', d[8:16])
    prefix = d[16 + hoff:16 + hoff + 0x100]
    src = d[16:16 + hoff]
    out = bytearray(usize)
    pos = len(src) - 1; bits_left = 0; cur = 0; w = usize - 1

    def get(n):
        nonlocal pos, bits_left, cur
        v = 0
        while n:
            if bits_left == 0:
                cur = src[pos]; pos -= 1; bits_left = 8
            take = min(bits_left, n)
            v = (v << take) | ((cur >> (bits_left - take)) & ((1 << take) - 1))
            bits_left -= take; n -= take
        return v
    end = 0
    while w >= end:
        if get(1):
            off = w + get(13) + 3
            ln = 3
            for lb in (2, 3, 5):
                x = get(lb); ln += x
                if x != (1 << lb) - 1: break
            else:
                while True:
                    x = get(8); ln += x
                    if x != 0xff: break
            for _ in range(ln):
                out[w] = out[off]; w -= 1; off -= 1
        else:
            out[w] = get(8); w -= 1
    return bytes(prefix) + bytes(out)


def entries(path):
    f = open(path, 'rb')
    h = f.read(16); assert h[:4] == b'CPK ', h
    sz, = struct.unpack('<Q', h[8:16])
    hdr = utf(f.read(sz))[0]
    toc = hdr['TocOffset']; content = hdr.get('ContentOffset') or toc
    f.seek(toc); th = f.read(16); tsz, = struct.unpack('<Q', th[8:16])
    rows = utf(f.read(tsz))
    base = min(toc, content)
    for r in rows:
        name = ((r.get('DirName') or '') + '/' + r['FileName']).lstrip('/')
        yield f, name, base + r['FileOffset'], r['FileSize'], r['ExtractSize']


def read(f, off, size):
    f.seek(off); d = f.read(size)
    if d[:8] == b'CRILAYLA': d = crilayla(d)
    return d


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'list':
        flt = sys.argv[3] if len(sys.argv) > 3 else ''
        for f, n, o, s, e in entries(path):
            if flt in n: print(f'{o:#012x} {s:10d} {e:10d} {n}')
    else:
        out = sys.argv[3]; flt = sys.argv[4] if len(sys.argv) > 4 else ''
        for f, n, o, s, e in entries(path):
            if flt in n:
                d = read(f, o, s); p = os.path.join(out, n)
                os.makedirs(os.path.dirname(p) or out, exist_ok=True)
                open(p, 'wb').write(d)

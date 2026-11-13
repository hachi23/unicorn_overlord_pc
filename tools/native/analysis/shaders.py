import re, struct, sys, collections, json, os
d = open(sys.argv[1], 'rb').read()
segs = [(0x4000, 0x0, 0x1274ea8), (0x127c000, 0x1400000, 0x1c7fb8), (0x1444000, 0x1600000, 0x3208)]
def va(off):
    for fo, v, sz in segs:
        if fo <= off < fo + sz: return v + off - fo
def off(v):
    for fo, a, sz in segs:
        if a <= v < a + sz: return fo + v - a
hdrs = [m.start() for m in re.finditer(b'Shdr', d) if d[m.start()-3:m.start()] != b'Orb']
infos = [m.start() for m in re.finditer(b'OrbShdr', d)]
print('headers', len(hdrs), 'infos', len(infos))
T = {0: 'PS', 1: 'VS', 2: 'ES', 3: 'LS', 4: 'CS', 5: 'GS', 7: 'HS', 8: 'DS'}
sh = []
for i, o in enumerate(infos):
    ver = d[o + 7]
    bits, = struct.unpack_from('<I', d, o + 8)
    typ = (bits >> 2) & 0xf; length = bits >> 8
    h, crc = struct.unpack_from('<QI', d, o + 16)
    # nearest header before
    hp = max(x for x in hdrs if x < o)
    sh.append(dict(info=va(o), hdr=va(hp), type=T.get(typ, typ), len=length, hash=h, crc=crc, hdrbytes=d[hp:hp+0x20].hex()))
c = collections.Counter(s['type'] for s in sh)
print('by type', dict(c))
print('unique hash', len({s['hash'] for s in sh}), 'unique crc', len({s['crc'] for s in sh}), 'unique (type,len)', len({(s['type'], s['len']) for s in sh}))
for t in c:
    ls = sorted(s['len'] for s in sh if s['type'] == t)
    print(t, 'code bytes min/med/max/total', ls[0], ls[len(ls)//2], ls[-1], sum(ls))
print('first', sh[0]); print('last', sh[-1])
# pointer refs to headers
hv = {s['hdr']: s for s in sh}
refs = collections.defaultdict(list)
for fo, a, sz in segs[1:]:
    for p in range(fo, fo + sz - 7, 8):
        v, = struct.unpack_from('<Q', d, p)
        if v in hv: refs[v].append(a + p - fo)
print('headers with a data pointer', len(refs), 'of', len(sh))
rl = sorted(x for v in refs.values() for x in v)
print('pointer slots range', hex(rl[0]), hex(rl[-1]), 'count', len(rl)) if rl else None
json.dump(dict(sh=sh, refs={str(k): v for k, v in refs.items()}), open('out/shaders.json', 'w'))
# cache cross-check
cd = sys.argv[2]
used = collections.Counter()
for f in os.listdir(cd):
    m = re.match(r'0x([0-9a-f]+)_(\d+)\.spv', f)
    if m: used[int(m.group(1), 16)] += 1
hs = {s['hash'] & 0xffffffff: s for s in sh}; hs.update({s['hash']: s for s in sh}); cr = {s['crc']: s for s in sh}
print('cache spv', sum(used.values()), 'distinct', len(used), 'match hash', sum(1 for u in used if u in hs), 'match crc', sum(1 for u in used if u in cr))
print('cache types', collections.Counter((hs.get(u) or cr.get(u) or {'type': '?'})['type'] for u in used))

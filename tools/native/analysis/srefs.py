import struct, json, collections, bisect
d = open(__import__('os').environ['UO_ELF'],'rb').read()
j = json.load(open('out/shaders.json')); sh = j['sh']
hd = sorted(s['hdr'] for s in sh); lo, hi = hd[0] - 0x400, max(s['info'] for s in sh) + 0x40
print('region', hex(lo), hex(hi))
# gaps: bytes before each header after the previous info
for s in sh[:3]:
    o = s['hdr'] + 0x4000
    print(hex(s['hdr']), 'pre', d[o-0x30:o].hex(), 'info-hdr', s['info'] - s['hdr'], 'len', s['len'])
# rip-relative lea / mov targets from text (va 0..0xa40000 approx)
t = collections.Counter(); srcs = collections.defaultdict(list)
text_end = 0xa40000
i = 0x4000
while i < 0x4000 + text_end:
    b = d[i]
    if b in (0x48, 0x4c) and d[i+1] == 0x8d and (d[i+2] & 0xc7) == 0x05:
        disp, = struct.unpack_from('<i', d, i + 3)
        tgt = (i - 0x4000) + 7 + disp
        if lo <= tgt < hi: t[tgt] += 1; srcs[tgt].append(i - 0x4000)
    i += 1
print('lea targets in region', len(t), 'sites', sum(t.values()))
# classify target relative to shader headers
rel = collections.Counter()
for tg in t:
    k = bisect.bisect_right(hd, tg) - 1
    rel[tg - hd[k] if k >= 0 else None] += 1
print('target - nearest header below (top):', rel.most_common(8))
# also 8-byte pointers anywhere in data to region
ptrs = []
for fo, a, sz in [(0x127c000, 0x1400000, 0x1c7fb8), (0x1444000, 0x1600000, 0x3208)]:
    for p in range(fo, fo + sz - 7, 8):
        v, = struct.unpack_from('<Q', d, p)
        if lo <= v < hi: ptrs.append((a + p - fo, v))
print('data pointers into region', len(ptrs), [(hex(a), hex(v)) for a, v in ptrs[:6]])
rp = collections.Counter()
for a, v in ptrs:
    k = bisect.bisect_right(hd, v) - 1; rp[v - hd[k] if k >= 0 else None] += 1
print('ptr - header:', rp.most_common(6))
json.dump({'lea': {str(k): v for k, v in srcs.items()}, 'ptrs': ptrs}, open('out/srefs.json', 'w'))

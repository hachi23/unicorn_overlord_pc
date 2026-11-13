import struct, json, collections
d = open(__import__('os').environ['UO_ELF'],'rb').read()
def off(v):
    for fo, a, sz in [(0x4000, 0x0, 0x1274ea8), (0x127c000, 0x1400000, 0x1c7fb8), (0x1444000, 0x1600000, 0x3208)]:
        if a <= v < a + sz: return fo + v - a
j = json.load(open('out/shaders.json')); sh = j['sh']
cont = {s['hdr'] - 0x24: s for s in sh}
r = json.load(open('out/srefs.json'))
slots = sorted(a for a, v in r['ptrs'] if v in cont)
print('pointer slots to shader containers', len(slots), 'distinct shaders', len({v for a, v in r['ptrs'] if v in cont}), hex(slots[0]), hex(slots[-1]))
lea = [int(k) for k in r['lea'] if int(k) in cont]
print('lea to containers', len(lea))
gaps = collections.Counter(b - a for a, b in zip(slots, slots[1:]))
print('slot gaps', gaps.most_common(8))
# dump table start
o = off(0x1403060)
for k in range(0, 0x120, 8):
    v, = struct.unpack_from('<Q', d, o + k)
    tag = ''
    if v in cont: tag = 'SHADER %s len %d' % (cont[v]['type'], cont[v]['len'])
    elif off(v) and v > 0x1000:
        s = d[off(v):off(v)+24].split(b'\0')[0]
        tag = repr(s) if len(s) > 2 and all(32 <= c < 127 for c in s) else ''
    print(hex(0x1403060 + k), hex(v), tag)
# strings just before first few shader containers
for s in sh[:1] + sh[400:402] + sh[1500:1501]:
    c = off(s['hdr'] - 0x24)
    print(s['type'], hex(s['hdr']), [x.decode('latin1') for x in d[c-0x180:c].split(b'\0') if len(x) > 2 and all(32 <= ch < 127 for ch in x)])

import struct, json, collections, re, os
d = open(__import__('os').environ['UO_ELF'],'rb').read()
def off(v):
    for fo, a, sz in [(0x4000, 0x0, 0x1274ea8), (0x127c000, 0x1400000, 0x1c7fb8), (0x1444000, 0x1600000, 0x3208)]:
        if a <= v < a + sz: return fo + v - a
j = json.load(open('out/shaders.json')); sh = j['sh']
cont = {s['hdr'] - 0x24: s for s in sh}
r = json.load(open('out/srefs.json'))
size = {}
for a, v in r['ptrs']:
    if v in cont: size[v] = struct.unpack_from('<Q', d, off(a) + 8)[0]
used = set()
cd = __import__('os').environ.get('UO_SHADER_CACHE', 'run_uo/user/cache/CUSA36381')
for f in os.listdir(cd):
    m = re.match(r'0x([0-9a-f]+)_(\d+)\.spv', f)
    if m: used.add(int(m.group(1), 16))
fam = collections.defaultdict(list)
allnames = collections.Counter()
for v, s in cont.items():
    if v not in size: continue
    blob = d[off(v):off(v) + size[v]]
    tail = blob[s['info'] - v + 0x1c:]
    names = [x.decode() for x in re.findall(rb'[A-Za-z_(][A-Za-z0-9_.()\[\]]{1,}', tail)]
    s['names'] = names
    key = (s['type'], tuple(sorted(set(n for n in names if not n.startswith('S_')))))
    fam[key].append(s)
    for n in set(names): allnames[n] += 1
print('containers sized', len(size))
for t in ('VS', 'PS', 'CS'):
    fs = [(k, v) for k, v in fam.items() if k[0] == t]
    u = [k for k, v in fs if any((x['hash'] & 0xffffffff) in used or x['hash'] in used for x in v)]
    print(t, 'shaders', sum(len(v) for k, v in fs), 'name-signature families', len(fs), 'families touched by cached play', len(u))
big = sorted(fam.items(), key=lambda kv: -len(kv[1]))[:12]
for k, v in big:
    print(len(v), k[0], 'len %d..%d' % (min(x['len'] for x in v), max(x['len'] for x in v)), ' '.join(k[1])[:230])
cb = collections.Counter(n for n in allnames if n.startswith('Fn') or 'ConstantBuffer' in n)
print('constant buffer-like names', sorted(n for n in allnames if n.startswith('Fn') or 'Buffer' in n)[:60])
print('distinct uniform names g_*', len([n for n in allnames if n.startswith('g_')]), 'samplers/textures', sorted(n for n in allnames if re.match(r'(s_|t_|g_Tex|g_Samp|Tex|Samp)', n))[:40])
top = sorted(sh, key=lambda s: -s['len'])[:8]
for s in top: print('largest', s['type'], s['len'], hex(s['hdr']), 'used' if s['hash'] in used else '', ' '.join(s.get('names', []))[:160])
json.dump(sh, open('out/shaders_named.json', 'w'))

"""slots.py FRAME_DUMP HASH...: for draws using a shader, the bound constant slots (stage/slot:
bytes and the first floats) and texture slots, once per distinct binding set."""
import sys
want = set(h.lower() for h in sys.argv[2:])
vs = ps = None; consts = {}; tex = {}; seen = set()
for line in open(sys.argv[1]):
    w = line.split()
    if not w: continue
    if w[0] == 'vs': vs = w[2]
    elif w[0] == 'ps': ps = w[2]
    elif w[0] == 'constants': consts[(w[2], w[4])] = ' '.join(w[6:7] + w[8:14])
    elif w[0] == 'texture' and w[1] == 'stage': tex[(w[2], w[4])] = ' '.join(w[5:9]) if 'none' not in line else 'none'
    elif w[0] == 'draw' and (vs in want or ps in want):
        key = (vs, ps, tuple(sorted(consts.items())), tuple(sorted(tex.items())))
        if key in seen: continue
        seen.add(key)
        print(f'vs {vs} ps {ps}')
        for (st, sl), v in sorted(consts.items()): print(f'  const stage {st} slot {sl}: {v}')
        for (st, sl), v in sorted(tex.items()): print(f'  tex stage {st} slot {sl}: {v}')

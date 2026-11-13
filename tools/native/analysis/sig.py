"""sig.py HASH...: a pixel shader's feature signature from the old recompiler's output (ref.py):
textures and the input feeding each uv, overlay blends, x2 multiplies, gradient lookups (uv.y
from a constant), constants read, and what the output rgb/alpha start from."""
import re, subprocess, sys
for h in sys.argv[1:]:
    t = subprocess.run([sys.executable, '-I', 'tools/native/analysis/ref.py', h], capture_output=True, text=True).stdout
    tex = re.findall(r'texture\(sampler2D\(fs_img(\d)[^)]*\), vec2\(([^,]+), ([^)]+)\)\)', t)
    texs = ['T%s(%s)' % (i, 'grad ' + y.strip() if re.match(r'c\d\[', y.strip()) else re.sub(r'fs_in_attr(\d)\.x', r'in\1', u)) for i, u, y in tex]
    consts = sorted(set(re.findall(r'c\d\[\d+\]', t)), key=lambda s: (s[1], int(s[3:-1])))
    ins = sorted(set(re.findall(r'fs_in_attr\d', t)))
    out = re.findall(r'frag_color0\.([xw]) = ([^;]+);', t)
    print(h, '|', ' '.join(texs), '| overlay' if '0.25 >' in t else '', '| x2' if re.search(r'\b2\.0 \*', t) else '',
          '| consts', ' '.join(consts), '| inputs', ' '.join(i[-1] for i in ins), '| out', ' '.join(f'{k}={v}' for k, v in out),
          '| loops' if 'for (' in t else '')

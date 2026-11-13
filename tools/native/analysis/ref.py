"""ref.py HASH [variant]: the old recompiler's version of a game shader, condensed for reading.
Constant buffer reads become cN[i] (N: the shader's Nth buffer, i: the float index), textures
tK, inputs/outputs keep their names; A development aid only: needs the old path's shader cache (run_uo/user/cache/CUSA36381)."""
import re, subprocess, sys
h, var = sys.argv[1].lower().zfill(8), (sys.argv[2] if len(sys.argv) > 2 else '0')
src = subprocess.run(['spirv-cross', '--version', '450', '--vulkan-semantics',
                      f'run_uo/user/cache/CUSA36381/0x00000000{h}_{var}.spv'], capture_output=True, text=True).stdout
body = src[src.index('void main()'):]
decl = [l for l in src[:src.index('void main()')].splitlines() if re.match(r'layout\(.*\) (in|out|uniform texture|uniform sampler)', l)]
body = re.sub(r'uintBitsToFloat\(ssbo_(\d+)_1\.data\[(\d+)u\]\)', lambda m: f'c{int(m.group(1))-1}[{m.group(2)}]', body)
body = re.sub(r'ssbo_(\d+)_1\.data\[', lambda m: f'c{int(m.group(1))-1}u[', body)
body = body.replace('precise ', '')
lines = [l.strip() for l in body.splitlines()[2:-1] if l.strip()]
text = '\n'.join(lines)
print('\n'.join(decl)); print(text)

"""names.py HASH...: the reflection names stored after a game shader (uniforms, buffers, inputs).
Finds the compiler footer ("OrbShdr", hash at +0x10) in the eboot and prints the strings that
follow the shader blob. Usage: python3 -I tools/native/analysis/names.py 66480ee3 0d2194b7"""
import re, struct, sys
elf = open(__import__('os').environ['UO_ELF'], 'rb').read()
for h in sys.argv[1:]:
    want = struct.pack('<I', int(h, 16))
    hits = [m.start() for m in re.finditer(b'OrbShdr', elf) if elf[m.start() + 0x10:m.start() + 0x14] == want]
    if not hits:
        print(h, 'not found'); continue
    at = hits[0]
    # The reflection block starts after the footer; take strings until the next shader header.
    end = elf.find(b'Shdr', at + 0x20)
    block = elf[at + 0x20:min(end if end > 0 else at + 0x4000, at + 0x4000)]
    names = [s.decode() for s in re.findall(rb'[A-Za-z_][A-Za-z0-9_\[\]\.]{2,}', block)]
    print(h, ' '.join(dict.fromkeys(names)))

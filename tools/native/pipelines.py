#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""pipelines.py [pipelines.txt...]: merge the pipelines players' runs recorded into the port's
list, src/native_pipelines.h, which the renderer compiles at start so first draws do not stutter.

Each pipelines.txt (in <data>/user/native_shaders/, written by src/native_vk.c) has one line per
pipeline: vertex shader hash, pixel shader hash, blend word, write mask (hex), Vulkan topology.
Only hashes and state numbers: nothing of the game is copied. The list is kept sorted and
without duplicates, so running it again with the same files changes nothing. With no file
arguments it rewrites the header from the list it already has."""
import re
import sys
from pathlib import Path

HEADER = Path(__file__).resolve().parents[2] / 'src' / 'native_pipelines.h'
ENTRY = re.compile(r'\{0x([0-9a-f]+), 0x([0-9a-f]+), 0x([0-9a-f]+), 0x([0-9a-f]+), (\d+)\}')
PREAMBLE = """/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pipelines the game uses, collected from playthroughs with tools/native/pipelines.py: vertex
 * and pixel shader hash, the game's blend word, write mask, Vulkan topology. src/native_vk.c
 * compiles them at start so that first draws do not stutter. Hashes and state words only: no
 * game data. Generated: edit with the tool, not by hand. */
"""


def read_header(path):
    if not path.exists():
        return set()
    return {(int(a, 16), int(b, 16), int(c, 16), int(d, 16), int(e)) for a, b, c, d, e in ENTRY.findall(path.read_text())}


def read_recorded(path):
    keys = set()
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) != 5:
            continue
        try:
            keys.add((int(parts[0], 16), int(parts[1], 16), int(parts[2], 16), int(parts[3], 16), int(parts[4])))
        except ValueError:
            continue
    return keys


def render(keys):
    lines = [f'    {{0x{vs:08x}, 0x{ps:08x}, 0x{blend:08x}, 0x{mask:x}, {topology}}},'
             for vs, ps, blend, mask, topology in sorted(keys)]
    body = '\n'.join(lines) if lines else '    {0, 0, 0, 0, 0},   /* none yet: the renderer skips shaders it cannot find */'
    return f'{PREAMBLE}static const uint32_t native_known_pipelines[][5] = {{\n{body}\n}};\n'


def main(argv):
    keys = read_header(HEADER)
    before = len(keys)
    for path in argv[1:]:
        keys |= read_recorded(path)
    HEADER.write_text(render(keys))
    print(f'{HEADER.name}: {len(keys)} pipelines ({len(keys) - before} new)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

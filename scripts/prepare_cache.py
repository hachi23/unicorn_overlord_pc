#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Skips the game image preparation (prepare.py, link_libc.py, link_modules.py,
content_profile.py) when nothing it reads has changed since the last start.

The key covers the preparation scripts' source, what preparation reads from the game folder
(eboot.bin, sce_module/, sce_sys/, dvdroot_ps4/: size and modification time, through links) and
the content SKU. Not the folder's path or its other files: a mod or language view (mods.py) is a
new temporary folder with rewritten archives at every start, and the same game image. A changed
key, a missing output or VW_PREPARE_ALWAYS=1 prepares again.
"""
import hashlib
import json
import os
from pathlib import Path

STAMP = 'prepare_key.json'
OUTPUTS = ('boot-linked.bin', 'content.bin', 'analysis.json', 'eboot.elf')


def _stat(path):
    try:
        st = path.stat()
    except OSError:
        return None
    return [path.name, st.st_size, st.st_mtime_ns]


def key(game, scripts, sku):
    game, scripts = Path(game).resolve(), Path(scripts)
    digest = hashlib.sha256()
    for script in sorted(scripts.glob('*.py')):
        digest.update(script.name.encode())
        digest.update(script.read_bytes())
    files = [_stat(game / 'eboot.bin'), _stat(game / 'dvdroot_ps4')]
    for sub in ('sce_module', 'sce_sys'):
        folder = game / sub
        if folder.is_dir():
            files += [[sub, *_stat(entry)] for entry in sorted(folder.iterdir()) if entry.is_file()]
    return {'scripts': digest.hexdigest(), 'sku': sku, 'files': files}


def valid(out, current):
    if os.environ.get('VW_PREPARE_ALWAYS') == '1':
        return False
    out = Path(out)
    if not all((out / name).is_file() for name in OUTPUTS):
        return False
    try:
        return json.loads((out / STAMP).read_text(encoding='utf-8')) == current
    except (OSError, ValueError):
        return False


def store(out, current):
    (Path(out) / STAMP).write_text(json.dumps(current), encoding='utf-8')


def forget(out):
    (Path(out) / STAMP).unlink(missing_ok=True)


def cached_device_name(out, probe, compute):
    """The GPU name from the last start while vwport and the Vulkan driver environment are the
    same (on Linux also the PCI display devices); otherwise compute() and remember it."""
    probe = Path(probe)
    env = {k: v for k, v in sorted(os.environ.items())
           if k.startswith(('VK_', 'DRI_PRIME', 'MESA_VK', 'VW_GPU', '__NV', 'DXVK_FILTER'))}
    devices = []
    pci = Path('/sys/bus/pci/devices')
    if pci.is_dir():
        for dev in sorted(pci.iterdir()):
            try:
                if (dev / 'class').read_text().startswith('0x03'):
                    devices.append([dev.name, (dev / 'vendor').read_text().strip(),
                                    (dev / 'device').read_text().strip()])
            except OSError:
                pass
    current = {'probe': _stat(probe), 'env': env, 'devices': devices}
    path = Path(out) / 'gpu_device.json'
    try:
        saved = json.loads(path.read_text(encoding='utf-8'))
        if saved.get('key') == current and saved.get('name'):
            return saved['name']
    except (OSError, ValueError):
        pass
    name = compute()
    if name:
        path.write_text(json.dumps({'key': current, 'name': name}), encoding='utf-8')
    return name

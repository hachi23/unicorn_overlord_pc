# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit, local-only support export. Never walk the user's data/game directories."""
import ctypes
import json
import os
from pathlib import Path
import platform
import re
import zipfile

LAUNCH_KEYS = ('VW_PATCHES', 'VW_PATCHES_ONLY', 'VW_RENDER_SCALE', 'VW_NEO_MODE',
               'VW_VBLANK_HZ', 'VW_DMEM_MB', 'VW_DEFERRED_READBACK', 'VW_STALE_READBACK',
               'VW_ASYNC_SHADERS', 'VW_GPU_LOG', 'VW_HANG_DUMP', 'VW_MODS_ENABLED',
               'VW_FSR1', 'VW_RCAS', 'VW_FRAMES_AHEAD', 'VW_FPS_LIMIT', 'VW_TEXT_LANGUAGE')
SETTING_KEYS = ('render_resolution', 'deferred_readback', 'stale_readback', 'async_shaders',
                'display_mode', 'vsync', 'fsr1', 'rcas', 'rcas_strength',
                'frames_queued', 'show_fps', 'overlay_fps', 'overlay_frametime', 'overlay_scale',
                'text_language', 'button_icons')


def system_info():
    cpu = platform.processor() or platform.machine()
    ram = None
    if os.name == 'nt':
        class Memory(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong),
                        *[(name, ctypes.c_ulonglong) for name in
                          ('total', 'available', 'page_total', 'page_available', 'virtual',
                           'virtual_available', 'extended')]]
        state = Memory()
        state.length = ctypes.sizeof(state)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(state)):
            ram = state.total
    else:
        try:
            for line in Path('/proc/cpuinfo').read_text().splitlines():
                if line.startswith('model name'):
                    cpu = line.split(':', 1)[1].strip()
                    break
            ram = os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
        except (OSError, ValueError, AttributeError):
            pass
    return {'os': platform.platform(), 'cpu': cpu, 'cpu_threads': os.cpu_count(),
            'ram_gib': round(ram / 2**30, 1) if ram else None}


def write_summary(log, summary):
    path = Path(log).with_suffix('.json')
    temp = path.with_suffix('.json.tmp')
    temp.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    temp.replace(path)


def redact(text, paths=()):
    # Known roots first (longest wins); then home-directory patterns from either platform.
    roots = {str(p) for p in (*paths, Path.home()) if p and len(str(p)) > 3}
    for root in sorted(roots, key=len, reverse=True):
        for form in {root, root.replace('\\', '/'), root.replace('/', '\\')}:
            text = re.sub(re.escape(form), '<local-path>', text, flags=re.IGNORECASE)
    text = re.sub(r'(?i)[a-z]:[\\/]Users[\\/][^\\/\s"\']+', '<home>', text)
    return re.sub(r'/(?:home|Users)/[^/\s"\']+', '<home>', text)


def support_files(log, data, paths=()):
    """Only a selected run, its one rotation and its summary; reject symlink escapes."""
    folder = (Path(data) / 'logs').resolve()
    log = Path(log)
    if log.is_symlink() or log.resolve().parent != folder or not re.fullmatch(r'run-[\w-]+\.log', log.name):
        raise ValueError('Select a timestamped run log in the logs folder.')
    files = {}
    for source, name in ((log.with_suffix('.log.1'), 'previous.log'),
                         (log, 'run.log'), (log.with_suffix('.json'), 'system.json')):
        if source.is_symlink():
            raise ValueError('Symbolic links cannot be exported.')
        if source.is_file():
            with source.open('r', encoding='utf-8', errors='replace') as stream:
                # Generated logs are capped; also bound imported/legacy logs.
                content = stream.read(16 * 1024 * 1024)
                if stream.read(1):
                    content += '\n[Export truncated at 16 MiB]\n'
            files[name] = redact(content, (data, *paths))
    if 'run.log' not in files:
        raise FileNotFoundError(log)
    return files


def export_archive(destination, files):
    # files is exactly the redacted preview the user has just inspected.
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, content in files.items():
            if name not in ('run.log', 'previous.log', 'system.json'):
                raise ValueError('Unexpected support file')
            archive.writestr(name, content)

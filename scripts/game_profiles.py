#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-game launch settings for run.py.

`defaults` apply unless the variable is already set; `forced` always apply (they are what the
port was tested with). `native_modules` are the game's own sce_module libraries linked into the
image after libc and Fios2 (link_modules.py). Render resolution is a code patch in the probe for
each game (src/unicorn_render_scale.h).
"""

PROFILES = {
    'CUSA36381': {  # Unicorn Overlord
        # The bundled PSN libraries run natively: their C++ toolkit then reaches the runtime only
        # through the PSN C APIs, which answer as signed out.
        'native_modules': ('libSceNpToolkit2.prx', 'libSceNpCppWebApi.prx', 'libSceJobManager.prx'),
        # Switch text mods: the Switch version's language archives hold the same text files
        # (MsgSheet/*.fms). A mod's archive is merged into the ones this version binds (English
        # with the fan patch, else Chinese) instead of being added; its textures (.ftx, NVT on the
        # Switch, GNF here) are left out. The text language japanese binds neither.
        'mod_archive_merge': {
            'archives': ('Unicorn_US.CPK', 'Unicorn_FR.CPK', 'Unicorn_DE.CPK', 'Unicorn_IT.CPK', 'Unicorn_ES.CPK'),
            'into': ('Unicorn_TW.CPK', 'Unicorn_CN.CPK'),
            'skip': ('.ftx',),
        },
    },
}


def profile(title_id):
    return PROFILES.get(title_id or '', {})


def apply(title_id, env):
    """Sets the profile's variables in `env` (a dict, e.g. os.environ)."""
    p = profile(title_id)
    for key, value in p.get('defaults', {}).items():
        env.setdefault(key, value)
    env.update(p.get('forced', {}))


# GCN 3-5 under RADV (Polaris, Vega and their APUs): HiZ depth compression shows black halos
# and missing geometry; RADV_DEBUG=nohiz avoids it at some cost (found by bbport, its #54, #99).
NOHIZ_CHIPS = ('TONGA', 'ICELAND', 'CARRIZO', 'FIJI', 'STONEY', 'POLARIS', 'VEGA', 'RAVEN', 'RENOIR')


def radv_nohiz(env, device_name):
    """RADV_DEBUG=nohiz for the game's GPU when it needs it, unless RADV_DEBUG is the user's or
    VW_AUTO_NOHIZ=0. Returns whether it was set."""
    if env.get('VW_AUTO_NOHIZ', '1') == '0' or env.get('RADV_DEBUG'):
        return False
    if any(f'(RADV {chip}' in (device_name or '') for chip in NOHIZ_CHIPS):
        env['RADV_DEBUG'] = 'nohiz'
        return True
    return False


def device_name(probe):
    """The game's GPU name from vwport --gpu-device, or ''."""
    import subprocess
    try:
        out = subprocess.run([str(probe), '--gpu-device'], capture_output=True, text=True,
                             timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
    except (OSError, subprocess.SubprocessError):
        return ''
    return out.strip().partition('\t')[2]

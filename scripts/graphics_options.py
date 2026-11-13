#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup graphics options of vw.ini (gpu/shim/vw_graphics.h) for run.py.

The in-game menu and the launcher write them; run.py turns them into environment variables
(the probe reads render_resolution, aspect_ratio and renderer itself) at every start, so "Apply and restart" uses the new values. After a
restart the game confirms them (restart_unconfirmed=0) once it shows frames; when a launch
never did, the next start goes back to the last confirmed values. A launch started by "Apply and
restart" confirms the one before it: that one reached the game, its menu asked for the restart.
"""
import json
from pathlib import Path

RESOLUTIONS = ('native', '720p', '900p', '1800p', '4K')
ASPECT_RATIOS = ('16:9', '21:9', '32:9')
RENDERERS = ('shadps4', 'native')
DEFAULTS = {'render_resolution': 'native', 'aspect_ratio': '16:9', 'deferred_readback': '1',
            'stale_readback': '1', 'async_shaders': '1', 'renderer': 'shadps4'}


def read_ini(path):
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def write_keys(path, updates):
    """Rewrites `updates` in place and appends missing keys, keeping every other line."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# vw settings (in-game menu: Insert / R3+L2)']
    done, out = set(), []
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            out.append(f'{key}={updates[key]}')
            done.add(key)
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if k not in done]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def startup_options(ini):
    """The startup options, normalized; unknown values fall back to the defaults."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    if options['render_resolution'] not in RESOLUTIONS:
        options['render_resolution'] = 'native'
    if options['aspect_ratio'] not in ASPECT_RATIOS:
        options['aspect_ratio'] = '16:9'
    if options['renderer'] not in RENDERERS:
        options['renderer'] = 'shadps4'
    for key in ('deferred_readback', 'stale_readback', 'async_shaders'):
        options[key] = '0' if options[key] == '0' else '1'
    return options


def prepare(config, state_path, restarted=False):
    """(options, reverted): the startup options of this launch, restoring the last confirmed
    ones when the previous launch after a restart never confirmed its own. restarted: this
    launch comes from the in-game "Apply and restart" (run.py --after)."""
    ini = read_ini(config)
    state = {}
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    options = startup_options(ini)
    reverted = False
    if ini.get('restart_unconfirmed') == '1':
        if restarted and state.get('attempt') and state.get('attempt_options'):
            # The previous attempt asked for this restart from its menu: it reached the game,
            # even when that was sooner than its own confirmation (or Apply overwrote it).
            state['last_good'] = state['attempt_options']
        if not restarted and state.get('attempt') and state.get('last_good'):
            options = startup_options(state['last_good'])
            write_keys(config, {**options, 'restart_unconfirmed': '0'})
            state = {'last_good': options, 'attempt': False}
            reverted = True
        else:
            state['attempt'] = True
            state['attempt_options'] = options
    else:
        state = {'last_good': options, 'attempt': False}
    Path(state_path).write_text(json.dumps(state, indent=2), encoding='utf-8')
    return options, reverted


def environment(options):
    return {'VW_DEFERRED_READBACK': options['deferred_readback'],
            'VW_STALE_READBACK': options['stale_readback'],
            'VW_ASYNC_SHADERS': options['async_shaders']}

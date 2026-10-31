#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts vwport (Windows and Linux; run.sh builds first on Linux).

Environment: VW_GAME_DIR (the game folder), VW_DATA_DIR (out/, user/, vw.ini; default this
directory), VW_PATCHES, VW_MODS_*, VW_USER_DIR, ... Extra arguments go to vwport.
The in-game "Apply and restart" runs this script again through VW_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
from datetime import datetime
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import threading
import traceback
import uuid

PORT = Path(__file__).resolve().parent
WINDOWS = os.name == 'nt'
LOG_KEEP = 10  # run logs kept in <data>/logs
LOG_BYTES = 8 * 1024 * 1024  # two segments per run, plus a bounded live last_run.log


class RunLog:
    """Everything a run prints (this script, the preparation scripts, vwport) goes to the console
    and to <data>/logs/run-<date>-<time>.log with the seconds since the start; the newest
    LOG_KEEP logs are kept and last_run.log mirrors the live current segment."""

    def __init__(self):
        self.file = None
        self.start = time.monotonic()
        self.lock = threading.RLock()
        self.size = 0
        self.support = None

    def open(self, data):
        logs = data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        self.path = logs / f'run-{datetime.now():%Y%m%d-%H%M%S-%f}-{uuid.uuid4().hex[:8]}.log'
        self.file = open(self.path, 'x', encoding='utf-8', errors='replace', buffering=1)
        for old in sorted(logs.glob('run-*.log'))[:-LOG_KEEP]:
            old.unlink(missing_ok=True)
            old.with_suffix('.log.1').unlink(missing_ok=True)
            old.with_suffix('.json').unlink(missing_ok=True)
        self.last = data / 'last_run.log'
        self.latest = open(self.last, 'w', encoding='utf-8', errors='replace', buffering=1)

    def write(self, line, console=True):
        if console:
            print(line, flush=True)
        with self.lock:
            if self.file:
                # Bound even a single pathological line. The previous segment preserves the
                # recent history; the support header is also in a separate JSON file.
                line = line[:LOG_BYTES // 8]
                entry = f'[{time.monotonic() - self.start:9.3f}] {line}\n'
                if self.size + len(entry.encode('utf-8')) > LOG_BYTES:
                    self.file.close()
                    self.path.replace(self.path.with_suffix('.log.1'))
                    self.file = open(self.path, 'w', encoding='utf-8', buffering=1)
                    self.latest.close()
                    self.latest = open(self.last, 'w', encoding='utf-8', buffering=1)
                    self.size = 0
                    entry = '[Log rotated; previous segment is .log.1]\n' + entry
                self.file.write(entry)
                self.latest.write(entry)
                self.size += len(entry.encode('utf-8'))
                if self.support is not None:
                    import diagnostics
                    updated = False
                    if line.startswith('GPU selected:'):
                        self.support['gpu_driver_api'] = line
                        updated = True
                    elif line.startswith('Mods: ') and line.endswith(' files'):
                        name, _, count = line[6:].rpartition(': ')
                        if name and count.split()[0].isdigit():
                            self.support.setdefault('mods', []).append(name[:256])
                            updated = True
                    elif line.startswith('Patches: ') and line.endswith(' writes)'):
                        self.support.setdefault('patches', []).append(line[9:][:512])
                        updated = True
                    if updated:
                        diagnostics.write_summary(self.path, self.support)

    def run(self, command, capture=False, **kwargs):
        """Runs command with its output (stdout and stderr) through the log; returns its exit code."""
        with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE if capture else subprocess.STDOUT,
                              text=True, errors='replace', bufsize=1, **kwargs) as process:
            output = []
            def read_errors():
                for line in process.stderr:
                    self.write(line.rstrip('\n'))
            reader = threading.Thread(target=read_errors) if capture else None
            if reader:
                reader.start()
            for line in process.stdout:
                if capture:
                    output.append(line)
                else:
                    self.write(line.rstrip('\n'))
            code = process.wait()
            if reader:
                reader.join()
            return (code, ''.join(output)) if capture else code

    def close(self, code):
        if not self.file:
            return
        self.write(f'Exit code {code}', console=False)
        self.file.close()
        self.file = None
        self.latest.close()


LOG = RunLog()


def port_version():
    """VERSION (written by packaging/package.py), else git describe, else 'unknown'."""
    try:
        return (PORT / 'VERSION').read_text(encoding='utf-8').strip()
    except OSError:
        pass
    try:
        return subprocess.run(['git', 'describe', '--always', '--dirty', '--tags'], cwd=PORT, capture_output=True,
                              text=True, timeout=5, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)
                              ).stdout.strip() or 'unknown'
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def fail(message):
    LOG.write(message, console=False)
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        import time
        # The restart exec'd this script in the game's own process: directly, or (the packaged
        # launcher) as the one-file bootloader whose child this is.
        if pid in (os.getpid(), os.getppid()):
            return
        for _ in range(300):  # 30 s, as on Windows
            try:
                os.kill(pid, 0)
            except OSError:
                return
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def run_script(name, *args, capture=False):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if capture:
        code, output = LOG.run(command, capture=True, creationflags=no_console())
        if code:
            LOG.write(f'{name} failed (exit code {code})')
            sys.exit(code)
        return output
    code = LOG.run(command, creationflags=no_console())
    if code:
        LOG.write(f'{name} failed (exit code {code})')
        sys.exit(code)
    return None


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'  # the in-game "Apply and restart"
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    # Relative paths (VW_GAME_DIR, VW_DATA_DIR) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('VW_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    LOG.open(data)
    LOG.write(f'vwport {port_version()} · {time.strftime("%Y-%m-%d %H:%M:%S %z")} · {platform.platform()} · '
              f'Python {platform.python_version()}')
    env.setdefault('VW_CONFIG', str(data / 'vw.ini'))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('VW_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    import diagnostics
    graphics, reverted = graphics_options.prepare(env['VW_CONFIG'], out / 'graphics_state.json', restarted)
    if reverted:
        LOG.write('Graphics: the last restart never reached the game; previous options restored')
    env.update(graphics_options.environment(graphics))
    support = {**diagnostics.system_info(), 'port_version': port_version(), 'graphics': graphics}
    LOG.support = support
    diagnostics.write_summary(LOG.path, support)
    LOG.write(f'System: {support}')

    probe = Path(env['VW_PROBE']) if env.get('VW_PROBE') else find_executable('vwport.exe' if WINDOWS else 'vwport')
    if not probe or not probe.is_file():
        fail('vwport not found: build it with build.sh or use a packaged build.')
    if not WINDOWS and not os.access(probe, os.X_OK):  # bundled as data by the packaged launcher
        probe.chmod(0o755)
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    import game_profiles
    import prepare_cache
    gpu = prepare_cache.cached_device_name(out, probe, lambda: game_profiles.device_name(probe))
    LOG.write(f'GPU: {gpu or "unknown"}')
    support['gpu'] = gpu or 'unknown'
    diagnostics.write_summary(LOG.path, support)
    if game_profiles.radv_nohiz(env, gpu):
        LOG.write('RADV: RADV_DEBUG=nohiz for this GPU (VW_AUTO_NOHIZ=0: off)')

    if not env.get('VW_GAME_DIR'):
        fail('Set VW_GAME_DIR to the game folder (eboot.bin, sce_module, ...).')
    game = Path(env['VW_GAME_DIR'])
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set VW_GAME_DIR).')
    from patches import eboot_supported
    if eboot_supported(game) is False and not env.get('VW_ALLOW_OTHER_EBOOT'):
        fail('eboot.bin is another build than the supported one (another region or update package), '
             'so the port cannot run it. It needs CUSA36381 v01.04 or the Hong Kong 1.05 '
             'package (issue #14). VW_ALLOW_OTHER_EBOOT=1 starts it anyway, for testing.')
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('VW_MODS_DIR', data / 'mods'),
                           '--config', env.get('VW_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('VW_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        sku = env.get('VW_CONTENT_SKU', 'full')
        prepare_key = prepare_cache.key(game, PORT / 'scripts', sku)
        if prepare_cache.valid(out, prepare_key):
            LOG.write('Game image: unchanged since the last start, preparation skipped')
        else:
            prepare_cache.forget(out)
            run_script('prepare.py', game, '--out', out)
            run_script('link_libc.py', game, '--out', out)
            run_script('link_modules.py', game, '--out', out)
            run_script('content_profile.py', game, '--out', out, '--sku', sku)
            prepare_cache.store(out, prepare_key)

        from patches import (eboot_matches, game_app_version, game_profile, game_title_id,
                             patch_requirements, selected_patches)
        profile = game_profile(game_title_id(game))
        LOG.write(f'Game: {game_title_id(game)} v{game_app_version(game)} · {original_game}')
        support.update(game=game_title_id(game), game_version=game_app_version(game))
        diagnostics.write_summary(LOG.path, support)
        patched = bool(profile) and ((game_app_version(game) == profile[2] and eboot_matches(game, profile))
                                     or bool(env.get('VW_FORCE_PATCHES')))
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries).
        try:
            from PIL import Image
            with Image.open(game / 'sce_sys' / 'icon0.png') as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('VW_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # Per-game settings (scripts/game_profiles.py).
        game_profiles.apply(game_title_id(game), env)
        # DLC the game is told it owns: packages or extracted folders in dlc/ (scripts/dlc.py).
        import dlc
        owned = dlc.labels(env.get('VW_DLC_DIR', data / 'dlc'), game_title_id(game))
        if owned:
            env['VW_ADDCONT'] = ','.join(owned)
            LOG.write('DLC: ' + ', '.join(f'{label} ({source})' for label, source in owned.items()))
        if patched:
            # The patch notes give the direct memory and VBlank rate the selected patches need.
            names = selected_patches(profile[1], profile[2], env.get('VW_PATCHES', ''), env.get('VW_PATCHES_ONLY') == '1')
            env['VW_PATCHES'] = ';'.join(names)
            env['VW_PATCHES_ONLY'] = '1'
            dmem, vblank = patch_requirements(profile[1], names, profile[2])
            for key, value in (('VW_DMEM_MB', dmem), ('VW_VBLANK_HZ', vblank)):
                if value and key not in env:
                    env[key] = str(value)
                    derived.append(key)
        run_script('patches.py', '--out', out, '--extra', env.get('VW_PATCHES', ''), '--game-dir', game,
                   '--patches-dir', env.get('VW_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('VW_PATCHES_CONFIG', data / 'patches.json'))
        if 'VW_VBLANK_HZ' not in env:
            env['VW_VBLANK_HZ'] = '60'
            derived.append('VW_VBLANK_HZ')
        env['VW_DERIVED'] = ','.join(derived)
        support['launch_options'] = {key: env[key] for key in diagnostics.LAUNCH_KEYS if key in env}
        support['settings'] = {key: value for key, value in graphics_options.read_ini(env['VW_CONFIG']).items()
                               if key in diagnostics.SETTING_KEYS}
        diagnostics.write_summary(LOG.path, support)
        LOG.write('Launch options: ' + str(support['launch_options']))

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('VW_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('VW_GPU_USER_DIR', str(user_dir))
        from save_backup import backup
        try:
            if made := backup(user_dir):
                LOG.write(f'Saves backed up to {made}')
        except OSError as error:
            LOG.write(f'Save backup failed, starting anyway: {error}')
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        if WINDOWS:
            env['VW_RESTART_COMMAND'] = subprocess.list2cmdline(restart)
        else:
            import shlex
            env['VW_RESTART_COMMAND'] = shlex.join(restart)
        # The in-game "Change game folder": the start program (launcher/vw_launcher.py) again.
        if env.get('VW_START_COMMAND'):
            env['VW_CHOOSE_GAME_COMMAND'] = f"{env['VW_START_COMMAND']} --choose-game --after {os.getpid()}"
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('VW_TIMEOUT', '0'), *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return LOG.run(command, creationflags=no_console())
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


def entrypoint():
    code = 1
    try:
        code = main()
    except KeyboardInterrupt:
        code = 130
    except SystemExit as stop:
        code = stop.code
    except Exception:
        LOG.write(traceback.format_exc())
    finally:
        LOG.close(code)
    return code


if __name__ == '__main__':
    sys.exit(entrypoint())

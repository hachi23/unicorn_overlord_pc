#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The ports' start program (Windows and Linux): a one-time setup, then straight into the game.

There is no launcher window. The first start asks which dump the user has (a game's `dumps`: for
Unicorn Overlord the Asia version with or without the fan English patch, each with its caveats and
text languages; the chosen language goes to vw.ini), then for its folder (a folder dialog),
checks that it is the supported game and version, installs it into data/game (hard
links when the dump is on the same drive, otherwise a copy with a progress window; resumable)
and remembers it in data/launcher.json; every start then prepares the game image (run.py,
skipped while nothing changed) and runs it. All settings
are in the in-game menu (Insert / R3+L2), which can also choose another game folder. Each game
has a two-line entry script (unicorn_launcher.py) that calls main(<key>).

Packaged (PyInstaller one-folder build, packaging/package.py) the program carries run.py and
scripts/ and also runs them (`--run`, `--script <file>`), so a packaged port needs no Python
installation:

    <Game>(.exe)        this program
    _internal/          its Python runtime, runtime/run.py, runtime/scripts/, runtime/bin/vwport
    LICENSE, licenses/
    data/               vw.ini, launcher.json, game/ (the installed dump), user/ (saves, caches),
                        out/ (generated), logs/

Arguments: --choose-game asks for the game folder again; --after PID first waits for that
process to end (the in-game "Change game folder" restart); --export-diagnostics only saves a
diagnostics ZIP of the newest run (works while the game is still running); --finish-update
FOLDER (with --after) is the second half of a Windows update (scripts/updates.py).

Packaged, each start first looks for a newer release (unless check_updates=0 in vw.ini) and
offers it; see scripts/updates.py.
"""
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys

FROZEN = getattr(sys, 'frozen', False)
HERE = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent
# Packaged: run.py, scripts/ and bin/vwport are in _internal/runtime (PyInstaller's bundle
# folder). From the source tree: the repository.
RUNTIME = Path(sys._MEIPASS) / 'runtime' if FROZEN else HERE.parent  # noqa: SLF001
WINDOWS = os.name == 'nt'

# Per game: the dump it accepts and its texts; the colors are the packaged program's icon.
GAMES = {
    'unicorn': {
        'title_id': 'CUSA36381', 'app_version': '01.04',
        'version_hint': 'the base game with the 1.04 update applied',
        'name': 'Unicorn Overlord', 'title': 'Unicorn Overlord', 'run_dir': 'run_uo',
        'colors': {'bg': '#0a1222', 'gold': '#c8a254', 'gold_hi': '#f0d48a'},
        # Both have the same eboot.bin; the fan English patch replaces the Chinese text files
        # with one English file in both slots (docs/KNOWN_ISSUES.md). text_language: vw.ini.
        'dumps': {
            'english': {
                'label': 'Asia version with the fan English patch',
                'caveats': (
                    'English text (a fan translation). Voices: Japanese or English, in the game\'s '
                    'Voice Language option.',
                    'Leave Text Language in the game\'s options on Game Default. Japanese there shows '
                    'the Japanese text; both Chinese choices also show English.',
                    'A few tutorial pictures were not translated by the patch and may show Japanese '
                    'text.',
                ),
                'languages': (('auto', 'English'),),
            },
            'asia': {
                'label': 'Asia version (official, no English patch)',
                'caveats': (
                    'No English text. Pick the text language below; it can be changed later in '
                    'System > Options > Text Language (applied at the next start).',
                    'The Asia version itself never shows its Japanese text; the port switches it on. '
                    'Tested in the menus so far.',
                    'Korean is not included in this version. Voices: Japanese or English, in the '
                    'game\'s Voice Language option.',
                ),
                'languages': (('traditional_chinese', 'Traditional Chinese (the game\'s default)'),
                              ('simplified_chinese', 'Simplified Chinese'), ('japanese', 'Japanese')),
            },
        },
    },
}


def data_dir(game):
    """data/ beside a packaged program (or its .AppImage: the AppImage itself is read-only); in the
    source tree the game's run directory."""
    beside = Path(os.environ['APPIMAGE']).parent if os.environ.get('APPIMAGE') else HERE
    default = beside / 'data' if FROZEN else RUNTIME / game['run_dir']
    return Path(os.environ.get('VW_DATA_DIR', default))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script."""
    # As `python script.py`: the script's folder comes first on the import path.
    script = argv[1] if argv[0] == '--script' else str(RUNTIME / 'run.py')
    sys.argv = [script, *argv[2 if argv[0] == '--script' else 1:]]
    sys.path.insert(0, str(Path(script).resolve().parent))
    runpy.run_path(script, run_name='__main__')
    return 0


sys.path.insert(0, str(RUNTIME / 'scripts'))
from patches import game_app_version, game_title_id  # noqa: E402


def load_settings(data):
    try:
        return json.loads((data / 'launcher.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(data, settings):
    data.mkdir(parents=True, exist_ok=True)
    temp = data / 'launcher.json.tmp'  # renamed over it: an interrupted write keeps the old file
    temp.write_text(json.dumps(settings, indent=2), encoding='utf-8')
    os.replace(temp, data / 'launcher.json')


def check_game(game_info, folder):
    """(ok, message) for a game folder."""
    name, title_id, app_version = game_info['name'], game_info['title_id'], game_info['app_version']
    if not folder:
        return False, f'Choose the folder of your {name} dump.'
    game = Path(folder)
    if not (game / 'eboot.bin').is_file():
        return False, ('There is no eboot.bin in this folder. Choose the folder that contains eboot.bin, '
                       'sce_module and sce_sys.')
    title, version = game_title_id(game), game_app_version(game)
    if title != title_id:
        return False, f'This is {title or "an unknown game"}, not {name} ({title_id}).'
    if version != app_version:
        return False, f'This dump is version {version}. The port needs {app_version}: {game_info["version_hint"]}.'
    return True, f'{name}  ·  {title_id}  ·  v{version}'


def detect_dump(folder):
    """'english' when Unicorn_CN.CPK and Unicorn_TW.CPK are the same file (the English patch puts
    its text in both), else 'asia'. Names are matched without case, as the game does."""
    import hashlib
    files = {}
    try:
        for entry in Path(folder).iterdir():
            files[entry.name.lower()] = entry
    except OSError:
        return 'asia'
    cn, tw = files.get('unicorn_cn.cpk'), files.get('unicorn_tw.cpk')
    try:
        if not cn or not tw or cn.stat().st_size != tw.stat().st_size:
            return 'asia'
        digests = [hashlib.sha256(path.read_bytes()).digest() for path in (cn, tw)]
    except OSError:
        return 'asia'
    return 'english' if digests[0] == digests[1] else 'asia'


def choose_dump(game_info, root, kind=None, language=None):
    """The setup window: which dump the user has, its caveats and (Asia) the text language.
    (kind, text_language) or None when cancelled."""
    import tkinter as tk
    from tkinter import ttk
    dumps = game_info['dumps']
    kind_var = tk.StringVar(value=kind if kind in dumps else next(iter(dumps)))
    language_var = tk.StringVar()
    result = {}
    window = tk.Toplevel(root)
    window.title(f"{game_info['name']}: setup")
    window.resizable(False, False)
    window.attributes('-topmost', True)
    body = ttk.Frame(window, padding=16)
    body.pack(fill='both')
    ttk.Label(body, wraplength=520,
              text=f"Which {game_info['name']} dump do you have? Both are {game_info['title_id']} "
                   f"version {game_info['app_version']} ({game_info['version_hint']}), from your "
                   'own console.').pack(anchor='w')
    for key, dump in dumps.items():
        ttk.Radiobutton(body, text=dump['label'], value=key, variable=kind_var,
                        command=lambda: refresh()).pack(anchor='w', pady=(8, 0))
    caveats = ttk.Label(body, wraplength=520, justify='left')
    caveats.pack(anchor='w', pady=(10, 4))
    row = ttk.Frame(body)
    row.pack(anchor='w', pady=(4, 0))
    ttk.Label(row, text='Text language').pack(side='left')
    languages = ttk.Combobox(row, textvariable=language_var, state='readonly', width=36)
    languages.pack(side='left', padx=8)

    def refresh():
        dump = dumps[kind_var.get()]
        caveats.configure(text='\n'.join(f'•  {line}' for line in dump['caveats']))
        names = [name for _, name in dump['languages']]
        languages.configure(values=names, state='readonly' if len(names) > 1 else 'disabled')
        keys = [key for key, _ in dump['languages']]
        language_var.set(names[keys.index(language)] if language in keys else names[0])

    def done(ok):
        if ok:
            dump = dumps[kind_var.get()]
            result['value'] = (kind_var.get(), next(key for key, name in dump['languages']
                                                   if name == language_var.get()))
        window.destroy()
    buttons = ttk.Frame(body)
    buttons.pack(fill='x', pady=(14, 0))
    ttk.Button(buttons, text='Choose the game folder…', command=lambda: done(True)).pack(side='left')
    ttk.Button(buttons, text='Cancel', command=lambda: done(False)).pack(side='right')
    ttk.Label(body, wraplength=520, foreground='#666',
              text='Later starts go straight into the game; the settings are in the in-game menu '
                   '(Insert, or R3+L2 on a controller).').pack(anchor='w', pady=(12, 0))
    window.protocol('WM_DELETE_WINDOW', lambda: done(False))
    refresh()
    window.wait_window()
    return result.get('value')


def tk_root():
    """A hidden Tk root for the dialogs (no launcher window)."""
    import tkinter as tk
    root = tk.Tk()
    root.withdraw()
    root.attributes('-topmost', True)
    return root


def choose_game(game_info, start, dump=None, language=None):
    """Asks for the dump folder until a supported one is chosen: (folder, dump kind, text
    language), the kind and language None for a game without `dumps`; None when cancelled."""
    from tkinter import filedialog, messagebox
    root = tk_root()
    try:
        if game_info.get('dumps'):
            chosen = choose_dump(game_info, root, dump, language)
            if not chosen:
                return None
            dump, language = chosen
        else:
            messagebox.showinfo(f"{game_info['name']}: setup",
                                f"Choose the folder of your own {game_info['name']} dump:\n\n"
                                f"{game_info['title_id']} version {game_info['app_version']} "
                                f"({game_info['version_hint']}).\n\n"
                                'It is the folder that contains eboot.bin, sce_module and sce_sys. It is '
                                'checked and prepared once, then the game starts. Later starts go straight '
                                'into the game; the settings are in the in-game menu (Insert, or R3+L2 on a '
                                'controller).', parent=root)
        folder = start
        while True:
            folder = filedialog.askdirectory(parent=root, title=f"The {game_info['title_id']} folder "
                                             '(eboot.bin, sce_module, sce_sys)',
                                             initialdir=folder or str(Path.home()), mustexist=True)
            if not folder:
                return None
            ok, message = check_game(game_info, folder)
            if ok and dump:
                found = detect_dump(folder)
                if found != dump:
                    dumps = game_info['dumps']
                    if messagebox.askyesno(f"{game_info['name']}: setup",
                                           f"This folder is the {dumps[found]['label']}, not the "
                                           f"{dumps[dump]['label']}.\n\nUse it as the "
                                           f"{dumps[found]['label']}?", parent=root):
                        chosen = choose_dump(game_info, root, found)
                        if not chosen:
                            return None
                        dump, language = chosen
                        if chosen[0] == found:
                            return folder, dump, language
                    continue
            if ok:
                return folder, dump, language
            if not messagebox.askretrycancel(f"{game_info['name']}: setup", message, parent=root):
                return None
    finally:
        root.destroy()


def install_plan(source):
    """(relative path, size) of every file of the dump, largest last."""
    source = Path(source)
    files = [(path.relative_to(source), path.stat().st_size) for path in source.rglob('*') if path.is_file()]
    return sorted(files, key=lambda item: item[1])


def install_files(source, target, progress=None, cancelled=lambda: False):
    """Installs the dump at `source` into `target`: hard links where the drive allows them, else
    copies. Files already there with the same size are kept, so a cancelled install resumes.
    Writes into <target>.partial and renames it at the end. Returns False when cancelled.
    progress(done_bytes, total_bytes, linked) is called as it goes."""
    source, target = Path(source), Path(target)
    partial = target.with_name(target.name + '.partial')
    partial.mkdir(parents=True, exist_ok=True)
    plan = install_plan(source)
    total = sum(size for _, size in plan) or 1
    done, linked = 0, True
    for relative, size in plan:
        if cancelled():
            return False
        to = partial / relative
        to.parent.mkdir(parents=True, exist_ok=True)
        if not (to.is_file() and to.stat().st_size == size):
            to.unlink(missing_ok=True)
            try:
                os.link(source / relative, to)
            except OSError:
                linked = False
                copied = 0
                with open(source / relative, 'rb') as reader, open(to, 'wb') as writer:
                    while chunk := reader.read(8 << 20):
                        if cancelled():
                            break
                        writer.write(chunk)
                        copied += len(chunk)
                        if progress:
                            progress(done + copied, total, linked)
                if copied != size:  # cancelled in the middle of the file
                    to.unlink(missing_ok=True)
                    return False
        done += size
        if progress:
            progress(done, total, linked)
    if target.exists():
        import shutil
        shutil.rmtree(target)
    partial.rename(target)
    return True


def install_game(game_info, source, data):
    """The install with a progress window; the installed folder, or None when cancelled."""
    import shutil
    import threading
    import tkinter as tk
    from tkinter import messagebox, ttk
    target = data / 'game'
    plan = install_plan(source)
    need = sum(size for _, size in plan)
    data.mkdir(parents=True, exist_ok=True)
    same_drive = os.stat(source).st_dev == os.stat(data).st_dev
    root = tk_root()
    try:
        free = shutil.disk_usage(data).free
        if not same_drive and free < need + (256 << 20):
            messagebox.showerror(f"{game_info['name']}: setup",
                                 f'The game needs {need / 2**30:.1f} GB in {data}, but only '
                                 f'{free / 2**30:.1f} GB are free. Free some space, or move the port '
                                 'to the drive that holds the dump (then nothing is copied).', parent=root)
            return None
        window = tk.Toplevel(root)
        window.title(f"{game_info['name']}: installing")
        window.resizable(False, False)
        window.attributes('-topmost', True)
        text = tk.StringVar(value=f'Installing the game into {target} …')
        ttk.Label(window, textvariable=text, padding=(16, 12, 16, 4), wraplength=420).pack(fill='x')
        bar = ttk.Progressbar(window, length=420, maximum=1000)
        bar.pack(padx=16, pady=8)
        state = {'done': 0, 'total': need or 1, 'linked': same_drive, 'result': None, 'error': None}
        stop = threading.Event()

        def work():
            def progress(done, total, linked):
                state.update(done=done, total=total, linked=linked)
            try:
                state['result'] = install_files(source, target, progress, stop.is_set)
            except OSError as error:
                state['error'] = error
        ttk.Button(window, text='Cancel', command=stop.set).pack(pady=(0, 12))
        window.protocol('WM_DELETE_WINDOW', stop.set)
        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            bar['value'] = 1000 * state['done'] / max(state['total'], 1)
            how = 'linking (same drive, no extra space)' if state['linked'] else 'copying'
            text.set(f"Installing the game into {target}: {how}, "
                     f"{state['done'] / 2**30:.1f} of {state['total'] / 2**30:.1f} GB")
            if worker.is_alive():
                window.after(200, poll)
            else:
                window.destroy()
        poll()
        window.wait_window()
        worker.join()
        if state['error']:
            messagebox.showerror(f"{game_info['name']}: setup", f"Installing failed: {state['error']}\n\n"
                                 'Start the program again to continue where it stopped.', parent=root)
            return None
        return str(target) if state['result'] else None
    finally:
        root.destroy()


def export_diagnostics(root, data, game_dir):
    """A support ZIP of the newest run: the log (paths redacted) and the system summary, shown
    in an editable preview first; nothing else from data/ or the game folder."""
    import diagnostics
    from tkinter import filedialog, messagebox
    import tkinter as tk
    logs = sorted((data / 'logs').glob('run-*.log'))
    if not logs:
        messagebox.showerror('Diagnostics', f'No run log in {data / "logs"}.', parent=root)
        return
    try:
        files = diagnostics.support_files(logs[-1], data, (game_dir, RUNTIME, HERE))
    except (OSError, ValueError) as error:
        messagebox.showerror('Diagnostics', str(error), parent=root)
        return
    preview = tk.Toplevel(root)
    preview.title('Diagnostics: review before sharing')
    preview.geometry('900x600')
    preview.attributes('-topmost', True)
    tk.Label(preview, text='Known folders are replaced by <local-path>. Remove anything else you do not '
             'want to share; the ZIP holds exactly this text.').pack(anchor='w', padx=8, pady=4)
    text = tk.Text(preview, wrap='none')
    text.pack(fill='both', expand=True)
    text.insert('end', '\n\n'.join(f'--- {name} ---\n{content}' for name, content in files.items()
                                    if name != 'system.json'))
    summary = files.get('system.json')

    def save():
        destination = filedialog.asksaveasfilename(parent=preview, title='Save the diagnostics ZIP',
                                                   defaultextension='.zip', initialfile='vwport-diagnostics.zip',
                                                   filetypes=[('ZIP archive', '*.zip')])
        if not destination:
            return
        chosen = {'run.log': text.get('1.0', 'end-1c')}
        if summary:
            chosen['system.json'] = summary
        try:
            diagnostics.export_archive(destination, chosen)
        except (OSError, ValueError) as error:
            messagebox.showerror('Diagnostics', str(error), parent=preview)
            return
        preview.destroy()
        messagebox.showinfo('Diagnostics', 'Saved. Attach the ZIP to your GitHub issue.', parent=root)
    tk.Button(preview, text='Save diagnostics ZIP…', command=save).pack(pady=6)
    preview.wait_window()


def report_failure(game_info, data, code, game_dir=''):
    """The game stopped with an error: offer a diagnostics ZIP for an issue, or the log itself."""
    log = data / 'last_run.log'
    try:
        from tkinter import messagebox
        root = tk_root()
        try:
            answer = messagebox.askyesnocancel(
                game_info['name'], f'The game stopped with an error (code {code}).\n\n'
                'Yes: save a diagnostics ZIP to attach to a GitHub issue (you review it first).\n'
                f'No: open the log of this run ({log}).\n\n'
                'docs/TROUBLESHOOTING.md in the repository explains what to look for.',
                icon='error', parent=root)
            if answer:
                export_diagnostics(root, data, game_dir)
            elif answer is False:
                if WINDOWS:
                    os.startfile(log)  # noqa: S606
                else:
                    subprocess.Popen(['xdg-open', str(log)])
        finally:
            root.destroy()
    except Exception as error:  # noqa: BLE001  (no display: the log is still written)
        print(f'The game stopped (code {code}); log: {log} ({error})', file=sys.stderr)


def wait_for(pid):
    """Waits (at most 30 s) for the previous game process to end."""
    import time
    # Linux: the in-game restart exec'd this program in the game's own process (directly, or
    # through a shell or the bootloader whose child this is); waiting for it would take 30 s.
    if not WINDOWS and pid in (os.getpid(), os.getppid()):
        return
    for _ in range(300):
        if WINDOWS:
            import ctypes
            handle = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
            if not handle:
                return
            done = ctypes.windll.kernel32.WaitForSingleObject(handle, 100) == 0
            ctypes.windll.kernel32.CloseHandle(handle)
            if done:
                return
        else:
            try:
                os.kill(pid, 0)
            except OSError:
                return
            time.sleep(0.1)


def current_version():
    try:
        return (RUNTIME / 'VERSION').read_text(encoding='utf-8').strip()
    except OSError:
        return ''


def ask_update(game_info, release, version):
    """'update', 'later' or 'skip'."""
    import tkinter as tk
    from tkinter import ttk
    root = tk_root()
    choice = {'value': 'later'}
    try:
        window = tk.Toplevel(root)
        window.title(f"{game_info['name']}: update")
        window.resizable(False, False)
        window.attributes('-topmost', True)
        notes = release['notes'].strip()
        if len(notes) > 900:
            notes = notes[:900].rsplit('\n', 1)[0] + '\n…'
        ttk.Label(window, padding=(16, 12, 16, 4), wraplength=460,
                  text=f"Version {release['tag']} is available (this is {version or 'a test build'}).\n\n"
                       f"{notes}\n\nYour settings, saves and the installed game are kept.").pack(fill='x')
        buttons = ttk.Frame(window, padding=(16, 4, 16, 12))
        buttons.pack(fill='x')

        def pick(value):
            choice['value'] = value
            window.destroy()
        ttk.Button(buttons, text='Update now', command=lambda: pick('update')).pack(side='left')
        ttk.Button(buttons, text='Later', command=lambda: pick('later')).pack(side='left', padx=8)
        ttk.Button(buttons, text='Skip this version', command=lambda: pick('skip')).pack(side='right')
        window.protocol('WM_DELETE_WINDOW', lambda: pick('later'))
        window.wait_window()
    finally:
        root.destroy()
    return choice['value']


def download_update(game_info, release, destination):
    """The download with a progress window; True when it finished and its SHA-256 matched."""
    import threading
    import tkinter as tk
    from tkinter import messagebox, ttk
    import updates
    root = tk_root()
    try:
        window = tk.Toplevel(root)
        window.title(f"{game_info['name']}: downloading {release['tag']}")
        window.resizable(False, False)
        window.attributes('-topmost', True)
        text = tk.StringVar(value='Downloading …')
        ttk.Label(window, textvariable=text, padding=(16, 12, 16, 4)).pack(fill='x')
        bar = ttk.Progressbar(window, length=420, maximum=1000)
        bar.pack(padx=16, pady=8)
        state = {'done': 0, 'total': release['size'], 'result': False, 'error': None}
        stop = threading.Event()

        def work():
            try:
                name = release['url'].rsplit('/', 1)[1]
                sha = updates.expected_sha256(updates.fetch_text(release['sums_url']), name)
                if not sha:
                    raise ValueError(f'{name} is not in {updates.SUMS}')
                state['result'] = updates.download(release['url'], destination, sha,
                                                   lambda d, t: state.update(done=d, total=t or state['total']),
                                                   stop.is_set)
            except (OSError, ValueError) as error:
                state['error'] = error
        ttk.Button(window, text='Cancel', command=stop.set).pack(pady=(0, 12))
        window.protocol('WM_DELETE_WINDOW', stop.set)
        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            total = max(state['total'], 1)
            bar['value'] = 1000 * state['done'] / total
            text.set(f"Downloading {release['tag']}: {state['done'] / 2**20:.0f} of {total / 2**20:.0f} MB")
            if worker.is_alive():
                window.after(200, poll)
            else:
                window.destroy()
        poll()
        window.wait_window()
        worker.join()
        if state['error']:
            messagebox.showerror(f"{game_info['name']}: update", f"The update could not be downloaded: "
                                 f"{state['error']}\n\nThe game starts with the current version.", parent=root)
        return state['result'] and not state['error']
    finally:
        root.destroy()


def check_for_update(game_info, data, settings):
    """Packaged builds: offers a newer release and applies it. Returns only when the game should
    start as it is (no update, declined, offline, failed)."""
    import updates
    if not FROZEN or not updates.setting_enabled(data / 'vw.ini'):
        return
    appimage = os.environ.get('APPIMAGE')
    if not WINDOWS and not appimage:
        return
    shutil_rmtree_quiet(data / 'update')
    version = current_version()
    try:
        release = updates.pick_release(updates.fetch_releases(), version, settings.get('skipped_update', ''))
    except (OSError, ValueError) as error:  # offline, rate limited: try next time
        print(f'Update check: {error}', file=sys.stderr)
        return
    if not release:
        return
    choice = ask_update(game_info, release, version)
    if choice == 'skip':
        settings['skipped_update'] = release['tag']
        save_settings(data, settings)
    if choice != 'update':
        return
    folder = data / 'update'
    folder.mkdir(parents=True, exist_ok=True)
    download = folder / release['url'].rsplit('/', 1)[1]
    if not download_update(game_info, release, download):
        return
    try:
        if WINDOWS:
            program = updates.unpack(download, folder / release['tag'])
            exe = program / Path(sys.executable).name
            subprocess.Popen([str(exe), '--finish-update', str(HERE), '--after', str(os.getpid())],
                             cwd=str(program))
            sys.exit(0)
        updates.replace_appimage(download, appimage)
        os.execv(appimage, [appimage, *sys.argv[1:]])
    except (OSError, ValueError) as error:
        from tkinter import messagebox
        root = tk_root()
        try:
            messagebox.showerror(f"{game_info['name']}: update", f'The update could not be installed: {error}\n\n'
                                 'The game starts with the current version.', parent=root)
        finally:
            root.destroy()


def finish_update(game_info, target):
    """Windows, run from the unpacked update: copies it over the program in `target`, then
    starts it there."""
    import updates
    target = Path(target)
    try:
        updates.install(HERE, target)
    except OSError as error:
        from tkinter import messagebox
        root = tk_root()
        try:
            messagebox.showerror(f"{game_info['name']}: update", f'The update could not be installed: {error}\n\n'
                                 'The previous version is still in place.', parent=root)
        finally:
            root.destroy()
    subprocess.Popen([str(target / Path(sys.executable).name)], cwd=str(target))
    return 0


def shutil_rmtree_quiet(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)


def start_command():
    """This program's own command line (the in-game "Change game folder" runs it again)."""
    if FROZEN:
        return [sys.executable]
    return [sys.executable, str(Path(sys.argv[0]).resolve())]


def restore_library_path():
    """Linux packaged: the library path the program was started with. PyInstaller puts its own
    libraries first and every child inherited them: vwport took the bundled Vulkan loader, whose
    vkCreateWaylandSurfaceKHR is null (a crash on Wayland: Steam Deck game mode, Plasma). This
    process keeps the libraries it loaded at start; children and an updated AppImage get the
    original path."""
    if not FROZEN or WINDOWS:
        return
    original = os.environ.pop('LD_LIBRARY_PATH_ORIG', None)
    if original is None:
        os.environ.pop('LD_LIBRARY_PATH', None)
    else:
        os.environ['LD_LIBRARY_PATH'] = original


def main(key):
    """Runs the setup and the game for GAMES[key], or one of the packaged roles (--run, --script)."""
    restore_library_path()
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if '--after' in args:
        wait_for(int(args[args.index('--after') + 1]))
    game_info = GAMES[key]
    if '--finish-update' in args:
        sys.exit(finish_update(game_info, args[args.index('--finish-update') + 1]))
    data = data_dir(game_info)
    if '--export-diagnostics' in args:  # also while the game hangs: run a second copy with it
        root = tk_root()
        try:
            export_diagnostics(root, data, load_settings(data).get('game_dir', ''))
        finally:
            root.destroy()
        sys.exit(0)
    settings = load_settings(data)
    folder = settings.get('game_dir')
    if '--choose-game' in args or not check_game(game_info, folder)[0]:
        chosen = choose_game(game_info, settings.get('source_dir', folder), settings.get('dump'),
                             settings.get('text_language'))
        if not chosen:
            sys.exit(0)
        folder, dump, language = chosen
        settings['source_dir'] = folder
        if dump:
            import graphics_options
            settings.update(dump=dump, text_language=language)
            data.mkdir(parents=True, exist_ok=True)
            graphics_options.write_keys(data / 'vw.ini', {'text_language': language})
        if os.environ.get('VW_INSTALL', '1') != '0' and Path(folder).resolve() != (data / 'game').resolve():
            installed = install_game(game_info, folder, data)
            if not installed:
                sys.exit(0)
            folder = installed
        settings['game_dir'] = folder
        save_settings(data, settings)

    check_for_update(game_info, data, settings)
    os.environ.update(VW_GAME_DIR=folder, VW_DATA_DIR=str(data), VW_CONFIG=str(data / 'vw.ini'))
    if FROZEN:
        # run.py uses the vwport of this bundle (bin/); one placed beside the program replaces it
        # (testing a build).
        beside = HERE / ('vwport.exe' if WINDOWS else 'vwport')
        if beside.is_file():
            os.environ['VW_PROBE'] = str(beside)
    # run.py turns it into VW_CHOOSE_GAME_COMMAND for the in-game menu.
    os.environ['VW_START_COMMAND'] = (subprocess.list2cmdline if WINDOWS else shlex.join)(start_command())
    data.mkdir(parents=True, exist_ok=True)
    try:
        code = run_role(['--run'])
    except SystemExit as stop:
        code = stop.code
    if code not in (0, None):
        report_failure(game_info, data, code, folder)
    sys.exit(code)

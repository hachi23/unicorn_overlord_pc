#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Packages a port (Linux, or Windows from MSYS2 CLANG64): python3 packaging/package.py <game>

<game> is a key of launcher/vw_launcher.py GAMES (unicorn). On Linux this builds the static
executable first (build.sh --static); on Windows build first (bash build.sh --static). Result:
dist/<Title> PC/ and an archive of it (.tar.gz on Linux, .zip on Windows):

    <Title>(.exe)   the start program (PyInstaller, one folder: it starts without unpacking
                    itself, which a one-file build did on every start, ~0.5 s on Linux)
    _internal/      its Python runtime and runtime/: run.py, scripts/ and bin/ with
                    vwport(.exe), the game executable (FFmpeg, SDL3, the GPU library and the C++
                    runtime linked in; Windows: plus any DLL it still loads from MSYS2). No Python
                    installation is needed.
    LICENSE, licenses/, README.txt
    data/           created on first start: vw.ini, the installed game, saves, generated files

No game file is included: the start program asks for the folder of the user's own dump.
"""
import ast
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile
import notices

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = os.name == 'nt' or sys.platform == 'msys'
OUT = ROOT / 'out'
VENV = OUT / 'pyenv'


def run(*command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def venv_python():
    python = VENV / ('Scripts/python.exe' if WINDOWS else 'bin/python')
    if not python.is_file():
        run(sys.executable, '-m', 'venv', VENV)
        run(python, '-m', 'pip', 'install', '-q', '--disable-pip-version-check', 'pyinstaller', 'pillow')
    return python


def stdlib_imports():
    """Standard modules imported by run.py and the scripts (run inside the frozen launcher)."""
    stdlib = set(sys.stdlib_module_names)
    found = set()
    for source in [ROOT / 'run.py', *sorted((ROOT / 'scripts').glob('*.py'))]:
        for node in ast.walk(ast.parse(source.read_text(encoding='utf-8'))):
            names = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                names = [node.module]
            for name in names:
                if name.split('.')[0] in stdlib and not (name.startswith('ctypes.wintypes') and not WINDOWS):
                    found.add(name)
    return sorted(found)


ICON_CODE = """
import sys
from PIL import Image, ImageDraw, ImageColor
size = 256
bg, frame, star = (ImageColor.getrgb(c) for c in sys.argv[2:5])
img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)
draw.ellipse((8, 8, size - 8, size - 8), fill=bg, outline=frame, width=10)
c, r, w = size / 2, 92, 22
draw.polygon([(c, c - r), (c + w, c - w), (c + r, c), (c + w, c + w), (c, c + r), (c - w, c + w),
              (c - r, c), (c - w, c - w)], fill=star)
draw.ellipse((c - 14, c - 14, c + 14, c + 14), fill=bg)
img.save(sys.argv[1], sizes=[(256, 256), (64, 64), (32, 32), (16, 16)])
"""


def make_icon(python, game):
    """A four-pointed star in the game's colors (the game's own art is not shipped)."""
    path = OUT / ('icon.ico' if WINDOWS else 'icon.png')
    colors = game['colors']
    run(python, '-c', ICON_CODE, path, colors['bg'], colors['gold'], colors['gold_hi'])
    return path


def freeze(python, key, game, binaries):
    # PIL._tkinter_finder: loaded by ImageTk at run time, not seen by PyInstaller's analysis.
    hidden = [arg for module in [*stdlib_imports(), 'PIL._tkinter_finder'] for arg in ('--hidden-import', module)]
    # vwport and its DLLs as data: PyInstaller must not bundle what they load from the system
    # (the Vulkan loader, libc).
    # VERSION: run.py prints it at the top of every run log (CI sets VW_VERSION).
    version = os.environ.get('VW_VERSION') or subprocess.run(
        ['git', 'describe', '--always', '--dirty', '--tags'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    (OUT / 'VERSION').write_text((version or 'unknown') + '\n', encoding='utf-8')
    data = [arg for source, target in ((ROOT / 'run.py', 'runtime'), (OUT / 'VERSION', 'runtime'),
                                       (ROOT / 'scripts', 'runtime/scripts'), (binaries, 'runtime/bin'))
            for arg in ('--add-data', f'{source}{os.pathsep}{target}')]
    run(python, '-m', 'PyInstaller', '--noconfirm', '--clean', '--log-level', 'WARN', '--onedir',
        '--windowed', '--name', game['title'], '--icon', make_icon(python, game), '--distpath', OUT / 'pyi-dist',
        '--workpath', OUT / 'pyi-work', '--specpath', OUT / 'pyi-work', '--paths', ROOT / 'scripts',
        '--exclude-module', '__pycache__', *data, *hidden, ROOT / 'launcher' / f'{key}_launcher.py', cwd=ROOT)
    return OUT / 'pyi-dist' / game['title']  # the folder: the program and _internal/


def collect_binaries():
    """out/pyi-bin: vwport, stripped (Windows: with the CLANG64 DLLs it still loads, if any)."""
    folder = OUT / 'pyi-bin'
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    if WINDOWS:
        probe = OUT / 'vwport.exe'
        shutil.copy2(probe, folder)
        # ldd lists DLLs with their MSYS2 paths; system DLLs are not copied.
        listing = subprocess.run(['ldd', str(probe)], capture_output=True, text=True, check=True).stdout
        for line in listing.splitlines():
            parts = line.split('=>')
            if len(parts) == 2 and '/clang64/bin/' in parts[1]:
                dll = parts[1].split('(')[0].strip()
                shutil.copy2(subprocess.run(['cygpath', '-w', dll], capture_output=True, text=True,
                                            check=True).stdout.strip(), folder)
    else:
        shutil.copy2(OUT / 'static' / 'vwport', folder)
    binary = folder / ('vwport.exe' if WINDOWS else 'vwport')
    symbols = OUT / 'symbols'
    symbols.mkdir(exist_ok=True)
    objcopy = shutil.which('objcopy') or shutil.which('llvm-objcopy')
    strip = shutil.which('strip') or shutil.which('llvm-strip')
    if not objcopy or not strip:
        raise RuntimeError('objcopy and strip are required to retain matching release debug symbols')
    run(objcopy, '--only-keep-debug', binary, symbols / 'vwport.debug')
    run(strip, '--strip-unneeded', binary)
    run(objcopy, f'--add-gnu-debuglink={symbols / "vwport.debug"}', binary)
    return folder


def source_archive(destination):
    """Only tracked port/submodule code and build-downloaded sources, never game data."""
    with tarfile.open(destination, 'w:gz') as archive:
        repos = [ROOT]
        listing = subprocess.check_output(['git', 'submodule', 'status', '--recursive'], cwd=ROOT, text=True)
        repos += [ROOT / line.split()[1] for line in listing.splitlines()]
        for repo in repos:
            prefix = 'source/' if repo == ROOT else f'source/{repo.relative_to(ROOT)}/'
            with subprocess.Popen(['git', 'archive', '--format=tar', f'--prefix={prefix}', 'HEAD'],
                                  cwd=repo, stdout=subprocess.PIPE) as process:
                with tarfile.open(fileobj=process.stdout, mode='r|') as source:
                    for member in source:
                        archive.addfile(member, source.extractfile(member) if member.isfile() else None)
                if process.wait():
                    raise RuntimeError(f'Cannot archive {repo}')
        recipe = ROOT / 'packaging/deps.sh'
        manifest = recipe.read_text() if recipe.is_file() else ''
        for name, version, url, checksum in re.findall(
                r'^"([\w]+) (\S+) (https://\S+) ([a-f0-9]{64})"$', manifest, re.MULTILINE):
            extension = url.rsplit('.tar.', 1)[-1]
            path = OUT / 'deps/src' / f'{name}-{version}.tar.{extension}'
            if not path.is_file():
                if name == 'nasm':  # build tool may come from the system
                    continue
                raise FileNotFoundError(f'Required dependency source missing: {path.name}')
            digest = hashlib.sha256()
            with path.open('rb') as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b''):
                    digest.update(chunk)
            if digest.hexdigest() != checksum:
                raise ValueError(f'Dependency source checksum mismatch: {path.name}')
            archive.add(path, arcname=f'dependency-sources/{path.name}')
        # Only the four known CI clones, and only their committed files. Never sweep src-*.
        for name in ('Vulkan-Headers', 'glslang', 'VulkanMemoryAllocator', 'robin-map'):
            path = ROOT / f'src-{name}'
            if not (path / '.git').exists():
                continue
            with subprocess.Popen(['git', 'archive', '--format=tar', f'--prefix=header-sources/{name}/', 'HEAD'],
                                  cwd=path, stdout=subprocess.PIPE) as process:
                with tarfile.open(fileobj=process.stdout, mode='r|') as source:
                    for member in source:
                        archive.addfile(member, source.extractfile(member) if member.isfile() else None)
                if process.wait():
                    raise RuntimeError(f'Cannot archive header sources: {name}')


def main():
    key = sys.argv[1] if len(sys.argv) > 1 else 'unicorn'
    sys.path.insert(0, str(ROOT / 'launcher'))
    from vw_launcher import GAMES
    game = GAMES[key]
    if WINDOWS:
        if not (OUT / 'vwport.exe').is_file():
            sys.exit('Build first: bash build.sh --static')
    else:
        run('bash', ROOT / 'build.sh', '--static', cwd=ROOT)
    name = f"{game['title']} PC"
    dist = ROOT / 'dist' / name
    python = venv_python()
    launcher = freeze(python, key, game, collect_binaries())
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir(parents=True)
    shutil.copytree(launcher, dist, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'LICENSE', dist)
    notices.collect(dist / 'licenses', python)
    (dist / 'README.txt').write_text(
        f'{game["title"]} experimental PC port\nNo game files included.\n'
        'Keep this folder together (the program, _internal/, LICENSE and licenses/). Settings,\n'
        'the installed game, logs and saves are in data/ beside the program. After an error the\n'
        'program offers a reviewed diagnostics ZIP (also: start it with --export-diagnostics).\n'
        'Help: https://github.com/hachi23/unicorn_overlord_pc/blob/main/docs/TROUBLESHOOTING.md\n'
        'Other languages (experimental): '
        'https://github.com/hachi23/unicorn_overlord_pc/blob/main/docs/LANGUAGE_MODS.md\n', encoding='utf-8')
    revision = subprocess.check_output(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=ROOT, text=True).strip()
    target = 'windows' if WINDOWS else 'linux'
    with tarfile.open(dist.parent / f'{key}-{target}-symbols-{revision}.tar.gz', 'w:gz') as symbols:
        symbols.add(OUT / 'symbols', arcname=f'symbols-{revision}')
    if not WINDOWS:
        source_archive(dist.parent / f'{key}-source-{revision}.tar.gz')
    archive = dist.parent / (name.replace(' ', '-') + ('-windows.zip' if WINDOWS else '-linux.tar.gz'))
    if WINDOWS:
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for path in dist.rglob('*'):
                z.write(path, Path(name) / path.relative_to(dist))
    else:
        with tarfile.open(archive, 'w:gz') as t:
            t.add(dist, arcname=name)
    files = sorted(p.name for p in dist.rglob('*') if p.is_file())
    print(f'{dist}: {len(files)} files ({", ".join(files)}); archive {archive} '
          f'({archive.stat().st_size >> 20} MiB)')


if __name__ == '__main__':
    main()

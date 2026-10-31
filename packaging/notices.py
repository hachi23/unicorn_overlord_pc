# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect notices from the exact checked-out/built dependencies; fail on omissions."""
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig

ROOT = Path(__file__).resolve().parent.parent
VENDORED = {
    'shadPS4': 'gpu/shadps4/LICENSE', 'LibAtrac9': 'third_party/LibAtrac9/LICENSE',
    'imgui': 'gpu/third_party/imgui/LICENSE.txt', 'sirit': 'gpu/third_party/sirit/LICENSE.txt',
    'SPIRV-Headers': 'gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE',
    'magic_enum': 'gpu/third_party/magic_enum/LICENSE', 'miniz': 'gpu/third_party/miniz/LICENSE',
    'xbyak': 'gpu/third_party/xbyak/COPYRIGHT', 'half': 'gpu/third_party/half/LICENSE.txt',
    'DejaVu': 'gpu/third_party/fonts/LICENSE-DejaVu.txt',
    'Anime4K': 'gpu/third_party/anime4k/LICENSE', 'SMAA': 'gpu/third_party/smaa/LICENSE.txt',
}
STATIC = ('zlib', 'xxhash', 'fmt', 'zycore', 'zydis', 'sdl3', 'ffmpeg')


def copy_notices(source, destination):
    candidates = sorted(p for p in source.iterdir() if p.is_file() and
                        p.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')))
    if not candidates:
        raise FileNotFoundError(f'No license notices found in {source}')
    destination.mkdir(parents=True, exist_ok=True)
    for path in candidates:
        shutil.copy2(path, destination / path.name)


def collect_native(destination, root=ROOT):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name, source in VENDORED.items():
        shutil.copy2(root / source, destination / f'{name}.txt')
    for name in STATIC:
        copy_notices(root / 'out/deps/build' / name, destination / name)
    shutil.copytree(root / 'packaging/licenses', destination / 'headers', dirs_exist_ok=True)
    # Preserve the original inline notices in AMD's shader and register headers, including
    # the extra portability-header attribution. These files contain no game data.
    for source in ('gpu/shadps4/video_core/host_shaders/fsr/ffx_a.h',
                   'gpu/shadps4/video_core/host_shaders/fsr/ffx_fsr1.h',
                   'gpu/third_party/gcn/include/gcn/si_ci_vi_merged_offset.h',
                   'gpu/third_party/gcn/include/gcn/si_ci_vi_merged_pm4_it_opcodes.h'):
        shutil.copy2(root / source, destination / Path(source).name)


def tcl_notice(name, version, library, root=ROOT):
    series = '.'.join(version.split('.')[:2])
    candidates = list(Path(library).parent.glob(f'{name}{series}*/license.terms'))
    candidates += list(Path('/usr/share/doc').glob(f'{name}{series}*/copyright'))
    # Python's Windows installer omits the standalone license. The upstream terms
    # are retained for the supported series; an unknown series still fails closed.
    candidates.append(root / 'packaging/licenses' / f'{name.title()}-{series}.txt')
    source = next((p for p in candidates if p.is_file()), None)
    if source is None:
        raise FileNotFoundError(f'{name} {version} runtime license not found')
    return source


def collect_python(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    python_notices = [Path(sys.base_prefix) / 'LICENSE.txt',
                      Path(sysconfig.get_path('stdlib')) / 'LICENSE.txt',
                      Path(f'/usr/share/doc/python{sys.version_info.major}.{sys.version_info.minor}/copyright')]
    source = next((p for p in python_notices if p.is_file()), None)
    if source is None:
        raise FileNotFoundError('Python runtime license not found')
    shutil.copy2(source, destination / 'Python.txt')
    versions = {'Python': sys.version}
    for name in ('Pillow', 'PyInstaller', 'pyinstaller-hooks-contrib'):
        dist = importlib.metadata.distribution(name)
        licenses = [p for p in (dist.files or []) if '.dist-info/' in str(p) and
                    any(part.lower().startswith(('license', 'copying', 'notice')) for part in p.parts)]
        if not licenses:
            raise FileNotFoundError(f'{name} wheel has no license notices')
        for index, path in enumerate(licenses):
            source = Path(dist.locate_file(path))
            if source.is_file():
                shutil.copy2(source, destination / f'{name}-{index}-{path.name}')
        versions[name] = dist.version
    # Tcl/Tk are frozen by PyInstaller's tkinter hook. Resolve without opening a GUI.
    import tkinter
    interpreter = tkinter.Tcl()
    tcl = Path(interpreter.eval('info library'))
    versions['Tcl'] = interpreter.eval('info patchlevel')
    versions['Tk'] = str(tkinter.TkVersion)
    for name in ('tcl', 'tk'):
        source = tcl_notice(name, versions[name.title()], tcl)
        shutil.copy2(source, destination / f'{name}.txt')
    (destination / 'versions.json').write_text(json.dumps(versions, indent=2), encoding='utf-8')


def collect_toolchain(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        compiler = shutil.which('clang')
        if not compiler:
            raise FileNotFoundError('Cannot locate the MSYS2 CLANG64 compiler')
        prefix = Path(compiler).resolve().parent.parent / 'share/licenses'
        if not prefix.is_dir():
            raise FileNotFoundError(f'MSYS2 notices not found: {prefix}')
        # Includes exact installed Vulkan/header and statically linked compiler runtime notices.
        shutil.copytree(prefix, destination / 'msys2', dirs_exist_ok=True)
    else:
        # Debian/Ubuntu (the CI runner): /usr/share/doc/<package>/copyright. Arch-based systems:
        # /usr/share/licenses/<package>/ (license files of the package).
        for name, pattern, arch in (('libstdc++', 'libstdc++6', 'libstdc++'), ('libgcc', 'libgcc-s1', 'libgcc'),
                                    ('zlib', 'zlib1g', 'zlib zlib-ng-compat zlib-ng'), ('Tcl', 'tcl8.6', 'tcl'), ('Tk', 'tk8.6', 'tk'),
                                    ('libffi', 'libffi8', 'libffi'), ('OpenSSL', 'libssl3', 'openssl'),
                                    ('bzip2', 'libbz2-1.0', 'bzip2'), ('liblzma', 'liblzma5', 'xz'),
                                    ('Expat', 'libexpat1', 'expat')):
            source = Path('/usr/share/doc') / pattern / 'copyright'
            folders = [Path('/usr/share/licenses') / package for package in arch.split()]
            folder = next((f for f in folders if f.is_dir() and any(f.iterdir())), None)
            if source.is_file():
                shutil.copy2(source, destination / f'{name}.txt')
            elif folder:
                shutil.copytree(folder, destination / name, dirs_exist_ok=True)
            else:
                raise FileNotFoundError(f'Toolchain/runtime notice missing: {source} (or /usr/share/licenses/{arch})')
        # The full license texts those notices refer to.
        for texts in ('/usr/share/common-licenses', '/usr/share/licenses/spdx'):
            if Path(texts).is_dir():
                shutil.copytree(texts, destination / 'license-texts', dirs_exist_ok=True)
                break


def collect(destination, python):
    destination = Path(destination)
    collect_native(destination)
    subprocess.run([str(python), str(Path(__file__).resolve()), str(destination / 'python')], check=True)
    collect_toolchain(destination / 'toolchain')
    shutil.copy2(ROOT / 'packaging/deps.sh', destination / 'dependency-sources-and-build-options.txt')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    (destination / 'SOURCE.txt').write_text(
        f'Port source revision: {commit}\nhttps://github.com/hachi23/unicorn_overlord_pc/tree/{commit}\n'
        'The matching source archive is attached to the release. It contains the port,\n'
        'submodules and downloaded static dependency sources. Build recipes: build.sh,\n'
        'packaging/deps.sh and .github/workflows/build.yml. Header-library versions and\n'
        'source repositories are pinned in that workflow. Runtime wheel versions are in\n'
        'python/versions.json; upstream sources: python.org, python-pillow.org, pyinstaller.org.\n',
        encoding='utf-8')


if __name__ == '__main__':
    collect_python(sys.argv[1])

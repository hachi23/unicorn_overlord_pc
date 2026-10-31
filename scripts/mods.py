#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

import cpk


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names)."""
    folder = Path(folder)
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


# The game's own executable and system folders: mods may not replace them.
SYSTEM = ('eboot.bin', 'sce_module', 'sce_sys')


def archive_merge(game):
    """The game's mod_archive_merge (game_profiles.py), or None."""
    from game_profiles import profile
    from patches import game_title_id
    return profile(game_title_id(game)).get('mod_archive_merge')


def game_names(game):
    """Case-folded names of the game folder's top-level content (CPK archives, data folders,
    ...), the archives a mod may bring to be merged into the game's (archive_merge) and the
    folders of loose files merged the same way: the paths a mod may replace or add under."""
    merge = archive_merge(game) or {}
    return ({e.name.casefold() for e in Path(game).iterdir()} - set(SYSTEM) |
            {name.casefold() for name in (*merge.get('archives', ()), *merge.get('folders', ()))})


def content_root(folder, names):
    """Where a mod's files are: the folder (or its app0/) when it holds some of the game's
    top-level `names`, else the same inside its only subfolder (an archive extracted into a
    folder of its name); None when there is no such layout."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    for base in (folder, child(folder, 'app0')):
        if base.is_dir() and any(e.name.casefold() in names for e in base.iterdir()):
            return base
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0], names)
    return None


def discover(root, names):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and content_root(p, names)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config, names):
    available = discover(root, names)
    if not config or not Path(config).is_file():
        return available
    settings = json.loads(Path(config).read_text())
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    disabled = set(disabled_names)
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    # New folders are enabled automatically and appended in alphabetical order.
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder, names):
    """(game path, source) of every file a mod replaces or adds under the game's top-level
    `names`."""
    root = content_root(folder, names)
    if not root:
        raise ValueError(f'{folder}: expected some of the game folder\'s files or folders inside the mod')
    root = root.resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in [*folders, *files]:
            if (directory / name).is_symlink():
                raise ValueError(f'Mod symlinks are unsupported: {directory / name}')
        for name in sorted(files):
            source = directory / name
            relative = source.relative_to(root)
            # Readme/metadata stay outside the mounted game. Executable patches use the patch
            # compiler (patches.py), with address validation.
            top = relative.parts[0].casefold()
            if top in SYSTEM:
                raise ValueError(f'Executable/system replacement is unsupported: {source}')
            if top not in names:
                continue
            if not source.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative, source


# Windows without the symlink privilege (Developer Mode off): directories become junctions,
# files hard links (copies across volumes). Those are not symlinks, so the overlay remembers
# what it linked and where to.
LINKED = {}


def make_link(link, target):
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
        return
    except OSError:
        if os.name != 'nt':
            raise
    if target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        try:
            os.link(target, link)
        except OSError:
            shutil.copy2(target, link)
    LINKED[str(link)] = target


def symlinks_work(directory):
    probe = Path(tempfile.mkdtemp(prefix='link-test-', dir=directory))
    try:
        (probe / 'link').symlink_to(probe, target_is_directory=True)
        return True
    except OSError:
        return False
    finally:
        shutil.rmtree(probe, ignore_errors=True)


def is_link(path):
    return path.is_symlink() or str(path) in LINKED or (hasattr(path, 'is_junction') and path.is_junction())


def link_target(path):
    return LINKED.get(str(path)) or path.resolve(strict=True)


def remove_link(path):
    if hasattr(path, 'is_junction') and path.is_junction():
        os.rmdir(path)
    else:
        path.unlink()
    LINKED.pop(str(path), None)


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if is_link(directory):
        target = link_target(directory)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        remove_link(directory)
        directory.mkdir()
        for entry in target.iterdir():
            make_link(directory / entry.name, entry)
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def merge_archives(overlay, merge, archives, loose=()):
    """Writes each of the game's merge['into'] archives in `overlay` again with the files of
    `archives` ((mod, path) in mod order: a later mod's file wins) in place of or beside its own,
    leaving out names ending in merge['skip']. Then the `loose` files ((mod, path in the
    archive, source) in mod order): a .fms sheet replaces the archive's, a .json one replaces
    the entries it lists (fms.apply)."""
    import fms
    sources = [(mod, cpk.Archive(path)) for mod, path in archives]
    mods = list(dict.fromkeys([m for m, _ in archives] + [m for m, _, _ in loose]))
    for target in merge['into']:
        destination = child(overlay, target)
        if not is_link(destination):
            continue
        base = cpk.Archive(link_target(destination))
        files = {name.casefold(): (name, base, name) for name in base.files}
        taken, skipped = set(), 0
        for mod, archive in sources:
            for name in archive.files:
                if name.casefold().endswith(tuple(merge.get('skip', ()))):
                    skipped += 1
                    continue
                files[name.casefold()] = (files.get(name.casefold(), (name,))[0], archive, name)
                taken.add(name.casefold())
        memory = cpk.Memory()
        for mod, name, source in loose:
            sheet = name.rsplit('.', 1)[0] + '.fms'
            if sheet.casefold() not in files:
                raise ValueError(f'{mod}: {name}: {destination.name} has no {sheet}')
            spelling, archive, original = files[sheet.casefold()]
            if name.casefold().endswith('.json'):
                try:
                    overrides = json.loads(source.read_text(encoding='utf-8-sig'))
                except json.JSONDecodeError as error:
                    raise ValueError(f'{mod}: {name}: {error}') from None
                current = memory.read(original) if archive is memory else cpk.crilayla(archive.read(original))
                data = fms.apply(current, overrides, f'{mod}: {name}')
            else:
                data = source.read_bytes()
                fms.parse(data)
            memory.add(spelling, data)
            files[sheet.casefold()] = (spelling, memory, spelling)
            taken.add(sheet.casefold())
        remove_link(destination)
        cpk.write(destination, {name: (archive, original) for name, archive, original in files.values()})
        print(f'Mods: {destination.name}: {len(taken)} files from {", ".join(mods)}'
              f'{f", {skipped} left out (another platform)" if skipped else ""}', file=sys.stderr)


def build_overlay(game, out, mods):
    game = Path(game).resolve(strict=True)
    names = game_names(game)
    merge = archive_merge(game) or {}
    merged = {name.casefold() for name in merge.get('archives', ())}
    folders = {name.casefold() for name in merge.get('folders', ())}
    replacements = []
    archives = []
    loose = []
    owners = {}
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root, names):
            if len(relative.parts) == 1 and relative.name.casefold() in merged:
                archives.append((name, source))
                count += 1
                continue
            if relative.parts[0].casefold() in folders:
                # A translation's sheets; anything else there (a readme) stays out.
                if len(relative.parts) == 2 and relative.suffix.casefold() in ('.json', '.fms'):
                    loose.append((name, relative.as_posix(), source))
                    count += 1
                continue
            key = relative.as_posix().casefold()
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    if not replacements and not archives and not loose:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt' and not symlinks_work(out) and out.drive.casefold() != game.drive.casefold():
        # Hard links only reach files on the same volume: the overlay goes beside the game.
        out = game.parent
    overlay = Path(tempfile.mkdtemp(prefix='mod-game-', dir=out))
    try:
        for entry in game.iterdir():
            make_link(overlay / entry.name, entry)
        replaced = added = 0
        for relative, source in replacements:
            # Each component takes the game's spelling when it exists in another case.
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if is_link(destination):
                replaced += link_target(destination).is_relative_to(game)
                remove_link(destination)
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            make_link(destination, source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        if archives or loose:
            merge_archives(overlay, merge, archives, loose)
        return overlay
    except BaseException:
        shutil.rmtree(overlay)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    args = parser.parse_args()
    layers = []
    if args.enabled == '1':
        # shadPS4's loose overlay convention, beside the original game.
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir():
            layers.append((legacy.name, legacy))
        # A directly selected loose overlay is also accepted.
        names = game_names(args.game)
        if args.mods_dir.is_dir() and any(e.name.casefold() in names for e in args.mods_dir.iterdir()):
            layers.append((args.mods_dir.name, args.mods_dir))
        else:
            for name in selected(args.mods_dir, args.config, names):
                layers.append((name, args.mods_dir / name))
    print(build_overlay(args.game, args.out, layers))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)

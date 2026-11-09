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


def child_name(archive, name):
    """`name` as `archive` spells it."""
    return next(n for n in archive.files if n.casefold() == name.casefold())


def merge_archives(overlay, merge, archives, loose=()):
    """Writes each of the game's merge['into'] archives in `overlay` again with the files of
    `archives` ((mod, path) in mod order: a later mod's file wins) in place of or beside its own,
    leaving out names ending in merge['skip']. Then the `loose` files ((mod, path in the
    archive, source) in mod order): a .fms sheet replaces the archive's, a .json one replaces
    the entries it lists (fms.apply). The merge['rewrap'] sheets an archive brings get line
    breaks where the PS4 boxes need them (fms.rewrap)."""
    import fms
    sources = [(mod, cpk.Archive(path)) for mod, path in archives]
    rewrapped = {}  # the same sheet for each target archive
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
        for sheet in merge.get('rewrap', ()):
            spelling, archive, original = files.get(sheet.casefold(), (None, base, None))
            if archive is base:
                continue
            key = (id(archive), original)
            if key not in rewrapped:
                ours = fms.parse(cpk.crilayla(base.read(child_name(base, sheet))))
                theirs = cpk.crilayla(archive.read(original))
                rewrapped[key] = fms.build(fms.rewrap(fms.parse(theirs), ours), theirs)
            memory.add(spelling, rewrapped[key])
            files[sheet.casefold()] = (spelling, memory, spelling)
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
        print(f'Mods (experimental): {destination.name}: {len(taken)} files from {", ".join(mods)}'
              f'{f", {skipped} left out (another platform)" if skipped else ""}', file=sys.stderr)


def language_files(folder, slots):
    """(label, path, slot) of the language archives (`slots`, matched without case) in
    `folder` or one folder level below it (a dump's romfs folder copied as it is), sorted by
    path; each fills the slot of its own name."""
    folder = Path(folder) if folder else None
    if not folder or not folder.is_dir():
        return []
    wanted = {name.casefold() for name in slots}
    found = [p for p in folder.iterdir() if p.is_file() and p.name.casefold() in wanted]
    found += [p for d in folder.iterdir() if d.is_dir() for p in d.iterdir()
              if p.is_file() and p.name.casefold() in wanted]
    return [(folder.name, p, p.name) for p in sorted(found)]


# The text language the game starts with (build_overlay: 'auto' becomes 'english' with the fan
# English patch); main() prints it after the folder for run.py.
TEXT_LANGUAGE = 'auto'

# vw.ini text_language keys (gpu/shim/vw_graphics.h) -> the archive the game opens for them.
SLOT_OF_LANGUAGE = {'english': 'Unicorn_US.CPK', 'french': 'Unicorn_FR.CPK', 'spanish': 'Unicorn_ES.CPK',
                    'german': 'Unicorn_DE.CPK', 'italian': 'Unicorn_IT.CPK'}


def english_text(path):
    """Whether a language archive's text is mostly Latin letters (the fan English patch's
    Unicorn_TW/CN.CPK) rather than CJK, judged from MsgSheet/UcSysMsg.fms."""
    try:
        text = cpk.Archive(path).extract('MsgSheet/UcSysMsg.fms')
    except (OSError, ValueError, KeyError):
        return False
    latin = sum(text.count(c) for c in b'etaoinsrhl')
    return latin > sum(1 for b in text if 0xe0 <= b <= 0xef)


def write_slots(overlay, merge, sources):
    """The game's language archives for Western text (merge['slots']: the game opens
    Unicorn_US/FR/ES/DE/IT.CPK for text languages 1-5, which the Asia dump lacks), each from
    the archives for it in `sources` ((label, path, slot) in order: a later file wins),
    over the game's own archive when it has one. Names ending in merge['skip'] (Switch
    pictures) are left out: the game then shows those of Unicorn.CPK."""
    by_slot = {}
    for label, path, slot in sources:
        by_slot.setdefault(slot.casefold(), []).append((label, path))
    for slot in merge.get('slots', ()):
        parts = by_slot.get(slot.casefold())
        if not parts:
            continue
        destination = child(overlay, slot)
        files, skipped = {}, 0
        if is_link(destination):
            base = cpk.Archive(link_target(destination))
            files = {name.casefold(): (name, base, name) for name in base.files}
            remove_link(destination)
        for _, path in parts:
            archive = cpk.Archive(path)
            for name in archive.files:
                if name.casefold().endswith(tuple(merge.get('skip', ()))):
                    skipped += 1
                    continue
                files[name.casefold()] = (files.get(name.casefold(), (name,))[0], archive, name)
            # The fan English patch keeps its story without the line breaks it added for the
            # Chinese boxes as <sheet>_original.fms. The game breaks Western lines itself, and
            # the added breaks made narration lines overlap there: the slot takes the original.
            for name in archive.files:
                if name.casefold().endswith('_original.fms'):
                    sheet = name[:-len('_original.fms')] + '.fms'
                    files[sheet.casefold()] = (files.get(sheet.casefold(), (sheet,))[0], archive, name)
        cpk.write(destination, {name: (archive, original) for name, archive, original in files.values()})
        print(f'Languages: {destination.name} from {", ".join(dict.fromkeys(l for l, _ in parts))}: '
              f'{len(files)} files{f", {skipped} pictures left out (Switch format)" if skipped else ""}',
              file=sys.stderr)


def build_overlay(game, out, mods, languages=None, text_language=None):
    """The game folder as the game sees it: `game` itself, or a view (links) with the mods'
    files and the language archives (`languages`: a folder of Switch Unicorn_XX.CPK files).
    Only the archive of `text_language` (a vw.ini key) is written, since a view makes the start
    prepare the game image again. For 'english' or 'auto' without a Unicorn_US.CPK, the fan
    English patch's archive (merge['english_from'], in a mod or the game) becomes it, and the
    language English (TEXT_LANGUAGE): the game's English mode shows the patch's text with the
    Western fonts (chapter title cards, #49)."""
    game = Path(game).resolve(strict=True)
    names = game_names(game)
    merge = archive_merge(game) or {}
    merged = {name.casefold() for name in merge.get('archives', ())}
    folders = {name.casefold() for name in merge.get('folders', ())}
    slots = {name.casefold() for name in merge.get('slots', ())}
    replacements = []
    archives = []
    loose = []
    owners = {}
    slot_sources = language_files(languages, merge.get('slots', ()))
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root, names):
            top = relative.name.casefold() if len(relative.parts) == 1 else None
            if top in merged:  # also into the Chinese archives, as before the slots
                archives.append((name, source))
            if top in slots:
                slot_sources.append((name, source, relative.name))
            if top in merged | slots:
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
    # Only the chosen language's slot: a view makes the start prepare the game image again.
    # 'auto' (the game's default) is English with the fan English patch: Chinese mode would show
    # its text in the Chinese fonts, and chapter title cards as one box (#49).
    global TEXT_LANGUAGE
    TEXT_LANGUAGE = text_language or 'auto'
    wanted = SLOT_OF_LANGUAGE.get(TEXT_LANGUAGE)
    slot_sources = [source for source in slot_sources if source[2].casefold() == (wanted or '').casefold()]
    # Translation mods (JSON sheets, a Switch archive merged into the Chinese ones) are read in
    # Chinese mode: with one, Game Default stays there.
    translation = bool(archives or loose)
    if (TEXT_LANGUAGE == 'english' or (TEXT_LANGUAGE == 'auto' and not translation)) and not slot_sources:
        english_from = {name.casefold() for name in merge.get('english_from', ())}
        # A mod's Unicorn_TW/CN.CPK (the patch installed as a mod) replaces the game's.
        sources = [source for relative, source in reversed(replacements)
                   if len(relative.parts) == 1 and relative.name.casefold() in english_from]
        sources += [child(game, name) for name in merge.get('english_from', ())]
        for path in sources:
            if path.is_file() and english_text(path):
                slot_sources.append(('the English patch', path, 'Unicorn_US.CPK'))
                TEXT_LANGUAGE = 'english'
                break
    if not replacements and not archives and not loose and not slot_sources:
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
        if slot_sources:
            write_slots(overlay, merge, slot_sources)
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
    parser.add_argument('--text-language', default='', help="vw.ini's text_language")
    parser.add_argument('--languages-dir', type=Path,
                        help='Switch language archives (Unicorn_US/FR/ES/DE/IT.CPK) for the text languages')
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
    print(build_overlay(args.game, args.out, layers, args.languages_dir, args.text_language))
    print(TEXT_LANGUAGE)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)

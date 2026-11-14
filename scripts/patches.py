"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.02"), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


PATCHES_DIR=Path(__file__).resolve().parent.parent/'patches'
# Built-in patch file per game, the app version its addresses are for and, when known, the
# sha256 of the only eboot.bin they were checked against (another build of the same version has
# its code elsewhere: byte writes there corrupt it).
GAMES=[]  # Unicorn Overlord ships no built-in patches; user patch files still apply (patches/)
DMEM_RETAIL_MB=5056  # runtime_memory.c POOL_SIZE default


def game_title_id(game):
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def game_profile(title_id):
    """(title IDs, built-in XML, app version, eboot sha256 or None) for a known game, else None."""
    return next((g for g in GAMES if title_id in g[0]),None)


def eboot_matches(game, profile):
    """Whether the game's eboot.bin is the build the profile's patches were checked against."""
    if not profile or not profile[3]:
        return True
    import hashlib
    try:
        return hashlib.sha256((Path(game)/'eboot.bin').read_bytes()).hexdigest()==profile[3]
    except OSError:
        return False


# sha256 of the eboot.bin builds the port's code hooks (render resolution, aspect ratio, button
# icons, the Options rows) know (src/unicorn_build.h): v01.04 (the 1.04 update, also under the fan
# English patch), the Hong Kong base game 01.00 master 1.05 (#33), and v01.04 as the retail update
# ships it (#95). The first v01.04 file is a backport: 196 PSN web API import stubs (NpCppWebApi,
# NpWebApi2, Http2), sceGnmGetGpuCoreClockFrequency and one sceSysmoduleLoadModule call return 0
# without calling. The game code is the same byte for byte, so both use the v01.04 addresses.
# Any other build is refused (issue #14).
SUPPORTED_EBOOTS={'CUSA36381':('de8e4970ae6ab39278bd61cd7818d7844adf934e1909e1356476a28d239414c4',
                               'fff894f5f889c19ba09b4af1cd7b8b403649a85540ff45137728218edc7c8eaf',
                               '67c1d56190a242720fa73d5061f5c9f49bfea1763091305da1049c95f207db5e')}


def eboot_supported(game):
    """True for the eboot.bin the PC features were made for, False for another build, None when
    the game has no known build or eboot.bin cannot be read."""
    expected=SUPPORTED_EBOOTS.get(game_title_id(game))
    if not expected:
        return None
    import hashlib
    try:
        return hashlib.sha256((Path(game)/'eboot.bin').read_bytes()).hexdigest() in expected
    except OSError:
        return None


def selected_patches(xml, app_version, extra, only=False):
    """The file's isEnabled patches for this version, then the names in extra (";"-separated).
    only: extra is the whole selection (the launcher's), without the file's defaults."""
    names=[] if only else [m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata')
                           if m.get('AppVer')==app_version and m.get('isEnabled','false').lower()=='true']
    names+=[n for n in (n.strip() for n in extra.split(';')) if n and n not in names]
    return names


def patch_requirements(xml, names, app_version):
    """(direct memory MiB, VBlank Hz) the selected patches' notes ask for, None when not needed.
    shadPS4 notes say "Requires setting DMEM to N MB": N is on top of the retail pool."""
    dmem=vblank=None
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        note=meta.get('Note','')
        if m:=re.search(r'DMEM to (\d+) MB',note): dmem=max(dmem or 0,DMEM_RETAIL_MB+int(m.group(1)))
        if m:=re.search(r'VBlank to (\d+) FPS',note): vblank=max(vblank or 0,int(m.group(1)))
    return dmem,vblank


def external_patches(directory, app_version, exclude, ids):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml (third-party
    shadPS4/GoldHEN files in the data directory's patches/ folder).
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        file_ids={e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids&ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_patches(out, writes):
    # G3PATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots.
    blob=struct.pack('<8sQQ',b'G3PATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def glyph_writes(a):
    """Glyph table writes for the Switch fonts in another order that mods.py converted
    (scripts/glyphs.py); they apply whatever the game's patch profile."""
    from mods import SWITCH_FONTS
    folder=Path(a.game_dir)/SWITCH_FONTS
    if not folder.is_dir(): return []
    import glyphs, hashlib
    elf=(a.out/'eboot.elf').read_bytes()
    fonts={p.stem:p.read_bytes() for p in sorted(folder.glob('*.ftx'))}
    key=hashlib.sha256(elf+b''.join(name.encode()+data for name,data in fonts.items())).hexdigest()
    cache=a.out/'glyph_tables.json'  # measuring the glyphs takes seconds
    try:
        saved=json.loads(cache.read_text())
        if saved['key']!=key: raise ValueError
        writes,notes=[(offset,bytes.fromhex(data)) for offset,data in saved['writes']],saved['notes']
    except (OSError,ValueError,KeyError,TypeError):
        writes,notes=glyphs.writes(elf,fonts)
        try:
            cache.write_text(json.dumps({'key':key,'notes':notes,'writes':[(o,d.hex()) for o,d in writes]}))
        except OSError:
            pass
    if notes: print(f'Patches: glyph tables for {", ".join(notes)}')
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('VW_GAME_DIR','../CUSA36381')))
    a=p.parse_args()
    title=game_title_id(a.game_dir)
    profile=game_profile(title)
    if not profile:
        write_patches(a.out,glyph_writes(a))
        print(f'Patches: no patch profile for {title}: none applied')
        return
    ids,xml,needed,_sha=profile
    # The patches are byte writes at the addresses of one game build: on another they would
    # corrupt code, so such a game runs unpatched.
    version=game_app_version(a.game_dir)
    if version!=needed and not os.environ.get('VW_FORCE_PATCHES'):
        write_patches(a.out,glyph_writes(a))
        print(f'Patches: game version {version}, {xml.name} is for {needed}: none applied')
        return
    if not eboot_matches(a.game_dir,profile) and not os.environ.get('VW_FORCE_PATCHES'):
        write_patches(a.out,glyph_writes(a))
        print(f'Patches: eboot.bin is not the build {xml.name} was checked against: none applied')
        return
    names=selected_patches(xml,needed,a.extra,os.environ.get('VW_PATCHES_ONLY')=='1')
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(xml,names,needed,segments)
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,needed,xml,ids),
                                                    a.patches_config),segments)
    write_patches(a.out,writes+glyph_writes(a))
    print(f'Patches: {title}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()

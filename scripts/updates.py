#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Updates of the packaged port from the repository's GitHub releases.

A release is a `vMAJOR.MINOR.PATCH` tag with the Windows ZIP, the Linux AppImage and SHA256SUMS
(.github/workflows/build.yml). The start program (launcher/vw_launcher.py) asks for the newest one
at start, at most every few seconds of network time; nothing is downloaded without the player's
yes, and every download is checked against SHA256SUMS before it replaces anything.

Applying an update:
  Windows: the ZIP is unpacked into data/update/<tag>/ and its program is started with
           --finish-update <install folder> --after <pid>. It waits for this program to end, moves
           the old program files aside, copies its own in (data/ is not touched), puts the old ones
           back if a copy fails, and starts the updated program.
  Linux:   the new AppImage is written beside the running one and renamed over it.
Builds that are not a release (the rolling "latest" build, a source tree) report their version as
`git describe` gives it; `vX.Y.Z-N-g<hash>` is newer than vX.Y.Z, anything else is older than
every release.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import urllib.request
import zipfile

REPOSITORY = 'hachi23/unicorn_overlord_pc'
RELEASES_URL = f'https://api.github.com/repos/{REPOSITORY}/releases?per_page=30'
# The shadPS4 edition (branch shadps4-edition) has its own downloads and tags (v1.6.0-shadps4), so it
# updates only to itself and the main build never to it.
ASSETS = {'windows': 'Unicorn-Overlord-shadPS4-PC-windows.zip', 'linux': 'Unicorn-Overlord-shadPS4-x86_64.AppImage'}
SUMS = 'SHA256SUMS'
PROGRAM_FILES_KEPT = ('data',)  # beside the program: never replaced by an update
VERSION = re.compile(r'^v(\d+)\.(\d+)\.(\d+)(?:-shadps4)?(?:-(\d+)-g[0-9a-f]+)?(?:-dirty)?$')


def parse_version(text):
    """(major, minor, patch, commits after the tag), or None for a build that is not a release."""
    match = VERSION.match((text or '').strip())
    return tuple(int(part or 0) for part in match.groups()) if match else None


def is_newer(tag, current):
    """Whether release `tag` is newer than the running build's version text."""
    release, running = parse_version(tag), parse_version(current)
    if not release or release[3]:
        return False
    return running is None or release[:3] > running[:3]


def platform_asset():
    return ASSETS['windows' if os.name == 'nt' else 'linux']


def pick_release(releases, current, skipped='', asset=None):
    """The newest release newer than `current` that has the asset and SHA256SUMS:
    {'tag', 'notes', 'url', 'sums_url', 'size'}; None when there is none or it is `skipped`."""
    asset = asset or platform_asset()
    best = None
    for release in releases:
        tag = release.get('tag_name', '')
        if release.get('draft') or not is_newer(tag, current):
            continue
        files = {a.get('name'): a for a in release.get('assets', [])}
        if asset not in files or SUMS not in files:
            continue
        if best is None or parse_version(tag) > parse_version(best['tag']):
            best = {'tag': tag, 'notes': release.get('body') or '', 'url': files[asset]['browser_download_url'],
                    'sums_url': files[SUMS]['browser_download_url'], 'size': files[asset].get('size', 0)}
    if best and best['tag'] == skipped:
        return None
    return best


# A frozen Python's OpenSSL looks for certificates where its build machine kept them; other
# Linux distributions keep them elsewhere.
CA_FILES = ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt', '/etc/ssl/cert.pem',
            '/etc/ssl/ca-bundle.pem')


def _context():
    context = ssl.create_default_context()
    if os.name != 'nt' and not context.get_ca_certs():
        for path in CA_FILES:
            if os.path.isfile(path):
                context.load_verify_locations(cafile=path)
                break
    return context


def _open(url, timeout):
    return urllib.request.urlopen(_request(url), timeout=timeout, context=_context())  # noqa: S310 (https only)


def _request(url):
    return urllib.request.Request(url, headers={'User-Agent': 'unicorn-overlord-pc-updater',
                                                'Accept': 'application/vnd.github+json'})


def fetch_releases(timeout=4.0):
    with _open(RELEASES_URL, timeout) as response:
        return json.loads(response.read().decode('utf-8'))


def fetch_text(url, timeout=10.0):
    with _open(url, timeout) as response:
        return response.read().decode('utf-8')


def expected_sha256(sums, name):
    """The hash of `name` in a SHA256SUMS text (`<hash>  <name>` lines), or None."""
    for line in sums.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip('*') == name and re.fullmatch(r'[0-9a-f]{64}', parts[0]):
            return parts[0]
    return None


def download(url, destination, sha256, progress=None, cancelled=lambda: False, timeout=30.0):
    """Downloads to `destination` (through destination.part); True when complete and the SHA-256
    matches. A mismatch or a cancel leaves nothing behind."""
    destination = Path(destination)
    partial = destination.with_name(destination.name + '.part')
    digest = hashlib.sha256()
    done = 0
    try:
        with _open(url, timeout) as response, open(partial, 'wb') as out:
            total = int(response.headers.get('Content-Length') or 0)
            while chunk := response.read(1 << 20):
                if cancelled():
                    raise InterruptedError
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        if digest.hexdigest() != sha256:
            raise ValueError(f'the download does not match {SUMS}')
        os.replace(partial, destination)
        return True
    except InterruptedError:
        return False
    finally:
        partial.unlink(missing_ok=True)


def unpack(archive, folder):
    """The release ZIP into `folder`; the program folder inside it (the one with _internal/)."""
    folder = Path(folder)
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    root = folder.resolve()
    with zipfile.ZipFile(archive) as z:
        for member in z.namelist():  # no entries outside the folder
            if not (folder / member).resolve().is_relative_to(root):
                raise ValueError(f'unsafe path in the update: {member}')
        z.extractall(folder)
    for candidate in [folder, *sorted(p for p in folder.iterdir() if p.is_dir())]:
        if (candidate / '_internal').is_dir():
            return candidate
    raise ValueError('the update has no program folder')


def install(new, target):
    """Replaces the program files in `target` with those of `new` (everything but data/). The
    old files are moved to <target>/.update-old first and moved back if a copy fails; they are
    removed once every file is in place."""
    new, target = Path(new), Path(target)
    old = target / '.update-old'
    if old.exists():
        shutil.rmtree(old)
    old.mkdir()
    names = [p.name for p in new.iterdir() if p.name not in PROGRAM_FILES_KEPT]
    moved = []
    try:
        for name in names:
            if (target / name).exists():
                shutil.move(str(target / name), str(old / name))
                moved.append(name)
        for name in names:
            source = new / name
            if source.is_dir():
                shutil.copytree(source, target / name)
            else:
                shutil.copy2(source, target / name)
    except OSError:
        for name in names:
            path = target / name
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            elif path.exists():
                path.unlink()
        for name in moved:
            shutil.move(str(old / name), str(target / name))
        shutil.rmtree(old, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)


def replace_appimage(new, appimage):
    """Puts a downloaded AppImage in place of the running one (a rename: the running copy keeps
    its open file)."""
    os.chmod(new, 0o755)
    os.replace(new, appimage)


def setting_enabled(vw_ini):
    """check_updates= in vw.ini (default on); VW_UPDATES=0 turns the check off."""
    if os.environ.get('VW_UPDATES', '1') == '0':
        return False
    try:
        for line in Path(vw_ini).read_text(encoding='utf-8').splitlines():
            if line.startswith('check_updates='):
                return line.split('=', 1)[1].strip() != '0'
    except OSError:
        pass
    return True

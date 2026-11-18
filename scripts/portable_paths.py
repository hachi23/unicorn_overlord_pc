# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path


def program_root(executable_dir: Path, appimage: str | None) -> Path:
    """The writable location beside the packaged program, outside any AppImage mount."""
    return Path(appimage).absolute().parent if appimage else executable_dir

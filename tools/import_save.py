"""Imports a player's UCSAVEFILE.DATA into a save slot so Load Game lists it.

The game builds its Load list from GLOBALDATA/UCGLOBALDATA.DAT, not from the save folders:
a table of 100 records of 0x48 bytes at 0x1818, one per slot. An empty record has the magic
"FNSD" at +4; a used one is the first 0x48 bytes of the slot's save file ("UCSD" header:
version, size, flags, timestamp, play time, ...). Slot n is the folder SAVEDATA<n>.

usage: import_save.py <savedata/1/CUSA36381> <slot> <UCSAVEFILE.DATA>
Close the game first; the global file is backed up next to itself once.
"""
import shutil
import sys
from pathlib import Path

TABLE, RECORD, SLOTS = 0x1818, 0x48, 100


def import_save(title_dir: Path, slot: int, save: Path) -> None:
    if not 0 <= slot < SLOTS:
        raise SystemExit(f"slot must be 0..{SLOTS - 1}")
    data = save.read_bytes()
    if data[4:8] != b"UCSD":
        raise SystemExit(f"{save}: not a Unicorn Overlord save (no UCSD header)")
    glob = title_dir / "GLOBALDATA" / "UCGLOBALDATA.DAT"
    g = bytearray(glob.read_bytes())
    if g[4:8] != b"UCSG":
        raise SystemExit(f"{glob}: no UCSG header")
    backup = glob.with_suffix(".DAT.bak")
    if not backup.exists():
        shutil.copy2(glob, backup)
    folder = title_dir / f"SAVEDATA{slot}"
    folder.mkdir(exist_ok=True)
    shutil.copy2(save, folder / "UCSAVEFILE.DATA")
    at = TABLE + slot * RECORD
    g[at:at + RECORD] = data[:RECORD]
    glob.write_bytes(g)
    print(f"slot {slot}: {save} -> {folder}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    import_save(Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3]))

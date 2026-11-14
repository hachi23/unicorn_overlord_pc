# Switch saves on the PS4 port (issue #71)

Status (2026-11-14): **works**. `scripts/switch_save.py` converts a Switch save, and the launcher
imports a folder of them (`--import-switch-save`, or the folder dropped on the program). Tested
with a real Switch 1.05 save of 70 hours: the four saves (Auto, 01-03, one of them Clear Data)
list with the right chapter, level and play time, load, and keep their units, gold, honors,
renown, world map, towns and stationed guards.

Limits:

- A save made during a battle (the autosave usually is) shows "Load failed because the game
  version has changed. Restarting stage." The battle snapshot is not converted. The game then
  starts that stage again from its beginning.
- The test save's final stage ("Unicorn Overlord") hangs when it starts, on the native renderer
  and on the v1.5.x shadPS4 build alike. Present waits for GPU work that never finishes. It looks
  like a port problem on that stage, not the conversion: everything else in the same save works.
  It is not settled until a PS4 save at that stage has been tried.
- `UCGLOBALDATA.DAT` (settings, Library, the Load list) is not converted. The importer writes
  only the Load list entries of the slots it fills.

## The files

Both are the game's save struct as it is in memory: no compression, no wrapper.

| | PS4 | Switch |
|---|---|---|
| File | `savedata/1/CUSA36381/SAVEDATA<n>/UCSAVEFILE.DATA` | `UCSAVEFILE00_AUTOSAVE.DAT`, `UCSAVEFILE01.DAT` ... |
| Size | 0x4DF720 | 0x4E2410 |
| Header | 0, `UCSD`, 0x1C50D5, size | the same, with the Switch size |
| Global | `GLOBALDATA/UCGLOBALDATA.DAT`, 1,671,256 bytes | `UCGLOBALDATA.DAT`, 200,888 bytes |

The version 0x1C50D5 is the same on both although the layout is not, so the game would read a
Switch file without complaint and get the wrong data: never load one unconverted.

The Load list comes from the global file: 100 records of 0x48 bytes at 0x1818, one per slot. A
used record is the first 0x48 bytes of that slot's save. Slot 0 is Auto.

## The layout

Found by comparing the save init functions of the two executables (PS4 v01.04 `eboot.elf` from
the preparation at 0x24cf59; Switch 1.05 update `main` at 0x2c8854, extracted with
`tools/re/nso_extract.py`). Each places its sub-structs and clears them, so the offsets of the
sub-inits line up block by block. The Switch build is AArch64 and pads some fields to 8 bytes.

| PS4 offset | Switch offset | Size (PS4) | Content |
|---|---|---|---|
| 0x0 | 0x0 | 0x20AEA1 | Header, money, items, characters, units, bonds... the same |
| 0x20AEA4 | 0x20AEA8 | 350 x 0xD8 | Town records: 0xF0 each on the Switch |
| 0x21D5F4 | 0x21F6C8 | 0x419 | The same |
| 0x21DA10 | 0x21FAF0 | 0xCFE00 | The same (shift 0x20E0) |
| 0x2ED810 | 0x2EF8F0 | 0x1280 | Zero in every save seen; not copied (a 0x10 shift inside is not located) |
| 0x2EEA90 | 0x2F0B80 | to 0x4D35F8 | The same (shift 0x20F0), including block B (0x425DA0, 0xAD858 bytes) |
| 0x4D35F8 | 0x4D56E8 | 0x40 | Flags of a 512-entry table |
| 0x4D3638 | 0x4D5728 | 512 x 4 | Table entries: u64 on the Switch, only the low 20 bits used |
| 0x4D3E38 | 0x4D6728 | 0x1700 | The same |
| 0x4D5538 | 0x4D7E28 | 0x20 | Flags of a 256-entry table |
| 0x4D5558 | 0x4D7E48 | 256 x 4 | Table entries: u64 on the Switch, only the low 8 bits used |
| 0x4D5958 | 0x4D8648 | to the end | The same (shift 0x2CF0) |

A town record (init Switch 0x2bcdd0) is a 0x40-byte name (`Town_C18_1`...) and two state
objects:

| | PS4 | Switch |
|---|---|---|
| Record | 0xD8; objects at +0x40 and +0x8C | 0xF0; objects at +0x40 and +0x98 |
| Object | 0x4C: 0x40 bytes, u8 at +0x40, u32 at +0x44, u8 at +0x48 | 0x58: 0x40 bytes, u8 at +0x40, 16 bits at +0x48, u8 at +0x50 |

The Switch init clears only the low 16 bits of the value at +0x48; the rest of each object is
padding. The test save has 337 records in use on both, in the same order, and the last byte
holds 1 or 2 in some of them.

## Checking a change

`python3 -m unittest tests.test_switch_save` checks the moves on a synthetic save. With a real
one, import it into a test user folder (`python3 scripts/switch_save.py <folder> run_uo/user`)
and load each slot: the Load list, Unit Formation, the Overworld menu's gold and renown, the
world map, and a town's Station a Guard list exercise most blocks.

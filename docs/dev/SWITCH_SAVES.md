# Switch saves on the PS4 port (issue #71): findings so far

Status (2026-11-09): **not possible yet**. No Switch save file has been seen. The Switch
version's save is a related but different layout, so a converter has to move whole blocks and
check every one of them against a real save.

## The PS4 save

`savedata/1/CUSA36381/SAVEDATA0/UCSAVEFILE.DATA`, 5,109,536 bytes (0x4DF720), not encrypted or
compressed. It holds the game's save struct as it is in memory:

| Offset | Value |
|---|---|
| 0x00 | 0 |
| 0x04 | `UCSD` (0x44534355) |
| 0x08 | 0x1C50D5 (the same in every save: a layout version) |
| 0x0C | 0x4DF720 (the size) |
| 0x10 | two bytes that change between saves (slot? 0x102 / 0x1ff), then the save time (year 0x7ea...) |

The PS4 eboot (v01.04 `eboot.elf` from the preparation, 0x250f60) clears 0x4DF720 bytes, writes
`UCSD` and 0x1C50D5 at offset 4, then sets up the sub-structs at 0x10, 0x425DA0, 0x4D35F8,
0x4D3E38, 0x4D4238 and 0x4D5360.

## The Switch save struct (1.05 update executable)

`tools/re/nso_extract.py exefs_upd/main <folder>` gives the code (`text.bin`, address 0). The
same function is at 0x2c8854: it clears **0x4E2410** bytes (11,504 more), writes the same `UCSD`
and the same version 0x1C50D5, then sets up its sub-structs at 0x10, **0x427E90**, **0x4D56E8**,
0x4D7E28, 0x4D8648 and 0x4DA398.

- From the second sub-struct on, everything is 0x20F0 bytes later than on the PS4: the first
  sub-struct (PS4 0x10-0x425DA0) is 0x20F0 larger on the Switch. Where inside it is not known.
- The remaining 0xC00 of the size difference is further on (not located).
- The version number is the same although the layout is not, so the game itself would accept a
  wrongly placed save: a converter must not rely on the header.
- How the Switch writes the file (the struct as it is, compressed, or with a Nintendo save
  header) is not known.

## Next steps

1. Get a Switch save (`UCSAVEFILE.DATA` or what the Switch has; the issue's reporter offered
   200 hours of play). The owner asked for it on #71.
2. Compare the two init functions' sub-calls (PS4 0x51f590 vs Switch 0x2d5b84 for the first
   sub-struct) to find where the 0x20F0 bytes go, then the same for the 0xC00.
3. Write the converter as a script that moves each block to its PS4 offset, and check it by
   loading the converted save at several points of the story.

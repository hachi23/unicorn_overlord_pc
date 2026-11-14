# PC rows in the game's Options screen

The game's own System → Options screen ends with a **PC** section: Display Mode, VSync, Render
Resolution, FSR 1 Upscaling, Sharpening, Sharpening Strength, Button Icons, Stick Dead Zone
and Performance Overlay. They look and work like the game's rows (choice boxes, Off/On switches, a
slider, help text at the bottom, Restore Defaults). Changes apply at once and go to `vw.ini`;
nothing is written to the game's system data. The Insert menu keeps every setting, including
the ones not in the screen.

Code: `src/unicorn_native_settings.h` (the game side, installed by `src/probe.c`) and
`vwgpu_setting_rows/value/set` in `gpu/shim/vw_settings.cpp` (the rows, their texts, and how a
value maps to a `VwSettings` field). Tests: `tests/test_native_settings.c` (`build.sh --test`)
and `tests/test_graphics_settings.cpp`.

Checked on 2026-10-19 (Linux, RX 560X): the title's Options (20 rows + the PC rows) and the battle map's
Stage Menu → System → Options (21 rows with Difficulty + the PC rows); changing a choice, a switch and the
slider (including L1/R1 steps of 10); closing and reopening; Restore Defaults; button icons
switching at once.

There is no frame rate setting: the game paces itself at its 60 Hz vblank. Measured on the
title (2026-10-19): no limiter 60.0 FPS with 17 ms worst frames; a 60 FPS limiter on top
59.6-59.8 FPS with 20-25 ms; 120 and 240 the same as none; 30 a steady 30.0. `VW_FPS_LIMIT`
remains for measurements. FSR 1 only acts when the render resolution is smaller than the
window (720p in a 1432x806 window: visibly sharper, edge energy 25.3 -> 31.7).

## How the screen works

All addresses are CUSA36381 v01.04 image offsets (the Ghidra image from
`tools/re/make_ghidra_elf.py`).

- Class `UcUI_Option`. `FUN_00735490` builds it, `FUN_00738f70` is its update (input, Restore
  Defaults), `FUN_007397f0` closes it (writes changed values), `FUN_00735000` sets the help text.
- Rows: a table at `0xd7c9f0`, 21 × `{float default, int label, int help, int kind, int flag}`.
  Kinds: 0 heading, 1 list of choices, 2 Off/On, 3 slider. Two orders pick and order the rows:
  `0xd7c940` (21, with Difficulty, in game) and `0xd7c9a0` (20, the title); the global at
  `0x234eee8` selects one.
- Row objects (vtable at +0): `+0xd8` set label, `+0xe0` set value, `+0xe8` value, `+0xd0` refresh.
  Choice rows keep the index at +0x3368, the last index at +0x3364 and the first value text at
  +0x3360; switches 0/1 at +0x38d0; sliders the value at +0x336c, ticks at +0x3378.
- Choice rows take `{id, count, first text}` from `0xd7cba0` (Difficulty) or `0xd7cbac` (Voice
  Language), picked by id in the builder. Sliders take `{id, format text, step, min, max,
  decimals}` from a pointer table at `0x14fcdb0` indexed by id − 4 (14 entries).
- Values live in the game's system data as 21 floats indexed by row id (`*(0x234ef00)+0x100` +
  `0x195698`; Difficulty in the save data). The screen keeps each row's value on opening at
  `+0x62b8` and, on closing, writes the rows whose value differs.
- Texts: ids in `MSGSHEET/UcOption.FMS` read through `FUN_0002b590(sheet, id)`. The help id of a
  choice or switch row is its help id + the value.
- Masks of row ids: `0x1441` the headings (the cursor skips them), `0x1d680c` the Off/On rows
  (refreshed once the screen is shown; without it a switch draws no box).

## How rows are added

`native_settings_install` checks every instruction it changes against v01.04 first and changes
nothing on another version (the log then says so; the Insert menu still works).

1. Copies of the three tables with ids 21 and up, and of the choice and slider tables. 24 `lea`
   instructions are repointed; seven bounds (0x13/0x14/0x15) and the row count are raised.
2. Ids past 20 would index past the 21 floats: three branches (value on build, value on open,
   write on close) load an array of the port's instead (`NativeTables.values`).
3. The choice-table pick becomes a lookup by id; the slider table is extended.
4. `FUN_0002b590` is replaced: ids from `0x40000` are the port's strings
   (`0x40000 + row × 64 + slot`: label, help, slider format, choice texts).
5. The builder is wrapped to load the current settings first; the update is wrapped to apply a
   changed row through `vwgpu_setting_set` and to set its value on open, so closing the screen
   never writes the port's rows to the game's data.

Limits: at most 11 rows (ids 21–31 fit the 32-bit masks, and none of them matches the game's
`(id & 0x1b) == 1` test for Difficulty and Voice Language). Render Resolution applies at the next
start. Not checked yet: Windows, the world map's menu, a Japanese system language (the texts are
English).

## Exit Game (src/unicorn_exit_game.h)

Added the same way: every instruction it changes is checked first (v01.04 and the Hong Kong 1.05
build through `unicorn_address`); on anything else it changes nothing and logs that the Insert
menu's Quit game still works.

- **Title menu.** The title's build (`0xa455b0`) is wrapped. After the game's buttons it adds one
  of its own, made with the game's allocator and button code (`0x9ed290`), with the label
  "Exit Game" set as a string (`0x2c590`). The button is made once per title object and freed
  with it (`0xa432f0`). The title's "which button was pressed" compare (`0xa4265d`) gets a stub:
  the port's button plays the confirm sound and calls `vwgpu_quit` (saves vw.ini, closes).
- **System submenus** (world map `0x7827d0`, list at +0x598; battle map `0x7de9e0`, list at
  +0x4d8). The builder is wrapped to append one entry after Load. The entry uses Return to
  Title's action (0xe) and carries a marker at entry +0x3604. Four stubs per menu:
  - the choice: remember that the marked entry was chosen;
  - the help line: "Close the game and return to the desktop.";
  - the Yes/No question: "Exit the game? Any unsaved progress will be lost.";
  - Yes: `vwgpu_quit` instead of the go-to-title flag (`0x21d7500`).
- Tests: `tests/test_exit_game.c`. Checked in the game: title (v01.04 and HK 1.05), the battle
  map and the world map (issue #34's save): help line, No returns to the menu, Yes fades out and
  closes, and Return to Title still asks its own question.

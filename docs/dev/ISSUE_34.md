# Issue #34: dialogue shows only "–" (handoff, 2026-10-26)

Start here. The next agent has **full control of the test PC**: run the game, drive it, take
frame dumps, and test everything listed below without asking. Ask before pushing to `main`,
merging, or posting on GitHub.

## The report

[#34](https://github.com/hachi23/unicorn_overlord_pc/issues/34): in some conversations the
text box shows the speaker's name and portrait, but the text is a single "–" with the
advance arrow (screenshot in the issue). Port v1.2.0, English-patch dump, game 1.04.
Three reporters, all NVIDIA (RTX 5080, 3080 Ti, 4070 Ti), Windows 10/11; no AMD report yet.

| Scene | Reporter | Result |
|---|---|---|
| Queen scenes, tutorial | thesupakorn | text fine |
| Josef carrying Alain on the horse | thesupakorn | "–" |
| Valmore in the throne room | thesupakorn | "–" |
| Scarlett at the shrine | thesupakorn | "–" |
| Alain training with Lex; Alain saving Scarlett in the church | thesupakorn | text fine |
| Every rapport conversation (first: Josef & Clive, Rapport C) | F34R7 | "–" |

F34R7's log (attached to the issue) has no errors: render scale 4K (Pro path), all hooks
applied, normal `Mapped 47071832 bytes ... 90011 relocations` (the known 1.04 eboot).

## What was checked (2026-10-26, AMD RX 9070 XT, English-patch dump)

- **Saves load** after importing them with `tools/import_save.py` (below). thesupakorn's
  save (described as "clean save at the start") is actually at **chapter 1's first battle**
  (Josef), past the broken prologue scenes. In that battle all dialogue showed text.
- F34R7's save (slot 2) is in the Bellum Ghoria area. Its Josef & Clive rapport conversation
  is unread, triggered "Near Fort Paradis, condition: liberate Fort Paradis". From the
  Rapport menu, Cross only says "This facility has not been registered", and the user could
  not find the spot on the map. **Not yet seen on our machine.**
- Not yet done: a New Game through the prologue scenes in the table.

## Leads

1. **English patch text file vs. the official one.** The patch replaces only
   `Unicorn_CN/TW.CPK`: 32 MsgSheets, `GUI/Interface.mbs`, and an extra
   `MsgSheet/UcScriptMsgConv_original.fms` (the game never asks for it). The story text is
   `MsgSheet/UcScriptMsgConv.fms` (`tools/re/fms.py` lists it):
   - official Japanese (UnicornPatch.CPK) 5,936,720 bytes, 24,513 non-empty strings;
     official TW 5,831,232 / 25,750; patch English 6,094,176 / 26,751.
   - The patch also wrote the string `EQUIP` at 0x3bd9f8, inside the zero area before the
     text (0x20..0x3c6cd8), which is zero in both official files.
   - The patch has no "–", "-" or "_" placeholder strings, so the dash is probably what the
     game draws when it cannot resolve a line, not text in the file.
   - How a script line finds its string is unknown (not the ordinal: the empty/non-empty
     pattern differs between the official JP and TW files). Next: find the FMSB loader in
     Ghidra (`MsgSheet` / `.fms` strings at ELF offset 0xb5dc79 / 0xb5dc94 in
     `eboot_ghidra.elf`) and the lookup used by the message window.
2. **Port or patch?** Not settled. Ways to settle it:
   - Same scene on the **official Asia dump** (Chinese or Japanese text via PR #27's Text
     Language). If the official text shows and the English does not, it is the patch (or
     the port's handling of a larger file).
   - Ask a reporter whether the same scenes work on a PS4 or shadPS4 with the same patch.
   - The voice: whether the line's voice plays while the box shows "–".
3. **GPU (NVIDIA only so far).** All three reports are NVIDIA; ours (AMD) showed text in the
   battle. If the prologue scenes show text here, ask a reporter for a frame where it fails
   with `VW_FRAME_DUMP_TRIGGER`, or test on an NVIDIA card. Text drawn but invisible would
   still lay out a line; "–" with the arrow right after it suggests an empty line instead.

## Reproducing

Attachments (download, never commit: they are player files):

- thesupakorn: `https://github.com/user-attachments/files/33919525/UCSAVEFILE.zip` (chapter 1
  first battle)
- F34R7: `https://github.com/user-attachments/files/33913998/UCSAVEFILE.zip` (Bellum Ghoria,
  rapport conversations unread) and log
  `https://github.com/user-attachments/files/33912623/run-20261024-214606-000338-45b7cc31.log`

**Importing a save.** Copying `UCSAVEFILE.DATA` into a folder is not enough: the Load list
comes from a 100-slot table in `GLOBALDATA/UCGLOBALDATA.DAT` (records of 0x48 bytes at
0x1818; empty records carry `FNSD` at +4, used ones are the first 0x48 bytes of the save,
`UCSD` header). Close the game, then:

    python tools/import_save.py "<dist>/data/user/savedata/1/CUSA36381" 1 <UCSAVEFILE.DATA>

Slot n is folder `SAVEDATA<n>`; the global file is backed up once as `UCGLOBALDATA.DAT.bak`.

**Driving the game without focus** (keyboard injection loses to the terminal taking focus):

    powershell -File tools/uo_pad.ps1 -Launch            # starts dist build wired to run_uo/drive
    powershell -File tools/uo_pad.ps1 "cross,wait1000,dump"

`dump` writes a frame (`.raw` + `.png`, 960x540) into `run_uo/drive/dumps`; read the png.
Title from boot: ~40 s, Cross (skip the opening), Cross (press any button), then Continue /
New Game / Load Game. `touchpad` opens the world map. `tools/uo_shot.ps1 -Out x.png` takes a
screenshot of the window instead (needs it in front). The game's own keyboard keys (Space
Cross, Left Shift Circle, E Square, Q Triangle, I/K/J/L d-pad, WASD left stick, Tab
touchpad) work too when the window has focus.

**Reading the text files:** `tools/re/cpk.py list|extract <cpk> [out] [filter]` (with CRILAYLA
decompression) and `tools/re/fms.py <file.fms> [substring]`.

## Next steps, in order

1. New Game on the English-patch dump; play to Josef carrying Alain (minutes after the
   opening). Dump a frame of each scene in the table. Note voice yes/no.
2. If it reproduces: same scene on the official Asia dump (TW and Japanese). Then find the
   message lookup in Ghidra and compare what it returns for that line in the two files.
3. If it does not reproduce on AMD: it is NVIDIA-specific; compare GPU paths
   (`VW_ASYNC_SHADERS`, readback options, render scale 1080p vs 4K) with a reporter, or on
   an NVIDIA machine.
4. Reach the Josef & Clive rapport conversation in F34R7's save (Fort Paradis, Cornia) and
   check it.
5. Reply on #34 with the result (ask the user before posting).

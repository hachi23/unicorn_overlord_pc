# File mods and third-party patches

Both work for every game and are applied by `run.py` at each start. The game folder is never
changed.

## File mods

Put each mod in its own folder under `mods/` in the data directory (`VW_DATA_DIR`, by default
the port folder):

```text
mods/
  Mod name/
    <files laid out as in the game folder, e.g. dvdroot_ps4/... or data/...>
```

These layouts are also accepted:

- `Mod name/app0/...`
- One extra folder level, as left by extracting an archive into a folder of its own name:
  `Mod name/Mod name v1.2/...`. Readme files and images beside it are ignored.

Name case does not matter: a mod file replaces the game file with the same name in any case.
The log reports what each start used, for example `Mods: 12 game files replaced, 0 added`. If a
mod that should replace files reports `0 game files replaced`, check its paths. Extract ZIP/7z
archives first.

When two mods replace the same file, the later one in the order wins the whole file, and the log
names the winner. A `<game folder>-mods/` folder beside the game folder (the shadPS4 convention)
loads before the listed mods.

Each start builds a tree of links in `out/mod-game-*` and removes it when the game ends. `/app0`
is read-only; saves stay where they are. Mods that replace `eboot.bin`, `sce_module` or
`sce_sys`, DLL injectors and script plugins are not supported, and symbolic links inside a mod
are refused.

The order and the disabled mods are kept in `mods.json` in the data directory. New folders are
enabled and go to the end of the list, in alphabetical order.

```json
{"order": ["First", "Second"], "disabled": ["First"]}
```

| Variable | Effect |
|---|---|
| `VW_MODS_DIR` | Another mods folder. |
| `VW_MODS_ENABLED=0` | Starts without any mod. |
| `VW_MODS_CONFIG` | Another `mods.json`. |

## Unicorn Overlord: Switch text mods

The Switch version keeps its English text in `Unicorn_US.CPK` (`MsgSheet/*.fms`), in the same
format as the PS4 version (the fan English patch is that text). A Switch text mod that ships
`Unicorn_US.CPK` works as is: put the file in its own mod folder,

```text
mods/
  UO Text Fixes/
    Unicorn_US.CPK
```

Its files are merged into the PS4 language archives the game reads, `Unicorn_TW.CPK` and
`Unicorn_CN.CPK`, and replace their text. The rest of those archives stays (the fan patch's
English interface). The log shows, for example,
`Mods: Unicorn_TW.CPK: 33 files from UO Text Fixes, 14 left out (another platform)`.
`Unicorn_FR/DE/IT/ES.CPK` are merged the same way. When several mods bring the same file, the
later one in the order wins.

- Switch textures (`.ftx`: NVT on the Switch, GNF on the PS4) are left out, so a mod's
  translated help pictures keep the PS4 ones.
- Mods that replace the Switch's `Unicorn.CPK` or other archives, and the Switch `exefs`, are
  not supported.
- With the text language `japanese` no language archive is read, so the mod has no effect.
- The Switch story text (`UcScriptMsgConv`) mostly has no line breaks, and the PS4 dialogue
  boxes do not add them: lines over 54 characters are broken at spaces when the archive is
  merged, as the PS4 text is. Menus and other sheets are left as they are, so a long translated
  menu text may still run past its box (the start-up warning runs past the screen edges).
- Checked in game with a Brazilian Portuguese Switch translation: title menu and opening
  dialogue, accented letters (ã, ç, é, í) included.

## Unicorn Overlord: translations

A translation is a mod with the game's text sheets as JSON, one file per sheet:

```text
mods/
  My translation/
    MsgSheet/
      UcOption.json      {"0": "Opciones", "1": "Volumen de la música", ...}
      UcSysMsg.json
      UcScriptMsgConv.json   (the story)
```

To start one, export the text of your copy of the game:

- Packaged port: run `"Unicorn Overlord.exe" --export-text` (Windows) or
  `./Unicorn-Overlord-x86_64.AppImage --export-text` (Linux). It writes every sheet to
  `data/mods/My translation/MsgSheet/`, from the text the game shows now (English with the fan
  patch). `--export-text <folder>` writes somewhere else.
- From the source: `python3 scripts/fms.py export <game folder> <mod folder>`.

Each file maps an entry number to its text. Change the texts and keep the numbers; entries you
delete keep the game's text, so a file may hold only the lines you changed. The files are UTF-8;
`\n` is a line break, and codes such as `#(2)` (a button icon) and `%.0f` (a number) must stay.
A sheet keeps its number of entries: an entry number the sheet does not have stops the start
with an error naming the file. A whole `.fms` sheet in `MsgSheet/` replaces the game's sheet.

At each start the sheets are merged into the language archives the game reads
(`Unicorn_TW.CPK`, `Unicorn_CN.CPK`); the game folder is not changed. The log shows, for example,
`Mods: Unicorn_TW.CPK: 3 files from My translation`. Several translations combine in the mod
order, entry by entry: a later mod's entry wins. Turn a translation off in `mods.json` like any
mod.

- The fonts are the game's: a character they lack shows as a box. The dialogue and menu fonts
  have the Latin accented letters (checked with Portuguese); other scripts are untested.
- Text longer than the original may run past its box.
- With the text language `japanese` no language archive is read, so the translation has no
  effect.

## Third-party patches (shadPS4 XML)

Put shadPS4/GoldHEN patch XML files in `patches/` in the data directory. Only the `Metadata`
entries for the game's title ID, its version and `eboot.bin` are used. Each patch's default
comes from its `isEnabled` attribute; `patches.json` overrides it:

```json
{"enabled": ["file.xml/Patch name"], "disabled": ["file.xml/Other patch"]}
```

Supported line types are `bytes`, `bytes16/32/64`, `float32/64`, `utf8` and `utf16`. A patch with
`mask` lines or an address outside the eboot is skipped entirely, with a warning in the log.
Third-party patches are applied after the built-in ones (`patches/<game>.xml`); where both write,
the third-party patch wins.

```sh
VW_PATCHES_DIR=/path VW_PATCHES_CONFIG=/path/patches.json python run.py
```

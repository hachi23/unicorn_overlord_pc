# Playing in another language (experimental)

> Just want to know which language you get and how to change it? Read
> [LANGUAGES.md](LANGUAGES.md) first. This page has the details, and mods.

The PS4 release this port runs has Japanese and Chinese text, and the fan English patch adds
English. The Switch version's text adds English, French, German, Italian and Spanish as
languages of their own, and two kinds of mod add other languages, or change the English.

## English, French, German, Italian or Spanish from the Switch version

The PS4 game still has the code for its Western languages: their fonts, line breaking and
title logo. Only the text files are missing, and the Switch version has them. You need the
Switch version of Unicorn Overlord dumped from your own console (update 1.05 recommended).

1. **Find the language file** in the Switch dump's `romfs` (from the update, when you have it):
   `Unicorn_US.CPK` (English), `Unicorn_FR.CPK` (French), `Unicorn_DE.CPK` (German),
   `Unicorn_IT.CPK` (Italian) or `Unicorn_ES.CPK` (Spanish). You can copy several.
2. **Copy it into the `languages` folder** in the data folder (the port makes that folder at
   start; see [Before you start](#before-you-start) for where the data folder is):
   ```text
   data/
     languages/
       Unicorn_FR.CPK
   ```
3. **Pick the language.** Press **Insert** (R3+L2 on a controller) and set **Text language** to
   **Français (Switch text)**, or the one you copied. Then restart the game.
4. **Check that it loaded.** The newest log in `data/logs/` has lines like:
   ```text
   Languages: Unicorn_FR.CPK from languages: 34 files, 14 pictures left out (Switch format)
   Text language: French
   ```
   Without the file the log says `French needs Unicorn_FR.CPK ...` and the game starts in its
   default language.

Saves work in every language: switch back and forth as you like.

What was checked: the title screen, the load screen, loading tips and the opening story in
French and German. The game wraps the lines itself, as in the Western releases.

Known limits of the Switch languages:

- **Tutorial pictures** with words in them stay those of your PS4 dump: the Switch pictures
  are in another picture format.
- **A few system messages** name the Nintendo Switch: the Switch text is used as it is.

## Mods

Two kinds of mod add other languages, or change the English:

| Mod | What it is | Example |
|---|---|---|
| **Switch text file** | A fan translation or text fix made for the Switch version, shipped as `Unicorn_US.CPK` (or `Unicorn_FR/DE/IT/ES.CPK`) | the Brazilian Portuguese Switch translation |
| **JSON translation** | The game's text as plain JSON files that you edit yourself | your own translation, or a few fixed lines |

> **Both are experimental.** They were checked on the title menu and the opening dialogue only,
> with a Brazilian Portuguese Switch translation. Battles, later chapters and most menus have
> not been checked with a mod. Please report what you see with the **Translation report** issue
> template, whether it works or not.

Your game folder is never changed: the port merges the mod's text into its own copy of the
language files at each start. Turn the mod off and the game is back as it was.

## Before you start

- Use a port version with mod language support: **v1.4.0 or newer**.
- Finish the one-time setup and start the game once, so the port has a `data` folder.
- Find your **data folder**. It is next to the program:
  - Windows: the `data` folder beside `Unicorn Overlord.exe`.
  - Linux: the `data` folder beside `Unicorn-Overlord-x86_64.AppImage`.
  - From the source tree: the port folder itself, or `VW_DATA_DIR` if you set it.

## Use a Switch text file

1. **Get the mod and unpack it.** If it is a `.zip` or `.7z`, extract it. The port cannot read
   archives. Find the file `Unicorn_US.CPK` in it (or `Unicorn_FR.CPK`, `Unicorn_DE.CPK`,
   `Unicorn_IT.CPK`, `Unicorn_ES.CPK`).
2. **Make the mods folder.** In the data folder, create a folder named `mods` if it is not
   there yet.
3. **Make a folder for the mod** inside `mods`, with any name, and put the `.CPK` file in it:
   ```text
   data/
     mods/
       Portuguese/
         Unicorn_US.CPK
   ```
   An extra folder level from unpacking (`mods/Portuguese/Portuguese v1.2/Unicorn_US.CPK`) also
   works. Readme files and pictures beside the file are ignored.
4. **Set the text language.** Press **Insert** (R3+L2 on a controller) and set
   **Text language** to the one the file is named after: `Unicorn_US.CPK` is
   **English (Switch text)**, `Unicorn_FR.CPK` **Français (Switch text)**, and so on. The game
   then wraps the lines itself. The mod's text also replaces **Traditional Chinese** and
   **Simplified Chinese**, as in earlier versions. With **Japanese** or **Korean** it has no
   effect.
5. **Restart the game.** The port merges the mod at each start.
6. **Check that it loaded.** Open the newest log in `data/logs/` and look for a line like:
   ```text
   Mods (experimental): Unicorn_TW.CPK: 33 files from Portuguese, 14 left out (another platform)
   ```
   The files "left out" are the Switch versions of the tutorial pictures. The PS4 game cannot
   read them, so the pictures stay those of your dump.

## Make or change a JSON translation

1. **Export the game's text.** This writes every text sheet as a JSON file into a new mod:
   - Windows: open a command prompt in the port folder and run
     `"Unicorn Overlord.exe" --export-text`
   - Linux: `./Unicorn-Overlord-x86_64.AppImage --export-text`
   - From the source tree: `python3 scripts/fms.py export <game folder> <mod folder>`

   The files go to `data/mods/My translation/MsgSheet/`. To write them somewhere else, add a
   folder name: `--export-text <folder>`. The text is what your game shows now: English with the
   fan patch, Chinese without it.
2. **Edit the text.** Each file maps an entry number to its text:
   ```json
   {"0": "Opciones", "1": "Volumen de la música"}
   ```
   - Change the text and keep the numbers.
   - Delete the entries you don't change: a missing entry keeps the game's text. A file may hold
     only the lines you changed.
   - Save the files as UTF-8.
   - Keep the special codes as they are: `\n` is a line break, `#(2)` is a button icon,
     `%.0f` is a number.
   - `UcScriptMsgConv.json` is the story, `UcOption.json` the Options screen, `UcSysMsg.json`
     the system messages.
3. **Set the text language** to Traditional or Simplified Chinese, as in step 4 above.
4. **Restart the game** and look for `Mods (experimental): Unicorn_TW.CPK: 3 files from My
   translation` in the newest log in `data/logs/`.

If a file has a mistake, the game does not start and the error names the file: for example an
entry number the sheet does not have, or broken JSON.

## Turn a mod off, or use more than one

The port lists every mod folder in `mods.json` in the data folder. A new folder is turned on and
goes to the end of the list.

- **Turn one off:** add its folder name to `disabled`:
  ```json
  {"order": ["Portuguese", "My fixes"], "disabled": ["Portuguese"]}
  ```
- **Use several:** when two Switch text files, or two JSON translations, change the same text,
  the one later in `order` wins. JSON translations are applied after Switch text files, entry by
  entry, so a small JSON mod can fix single lines of a Switch translation.
- **Start once without any mod:** set the environment variable `VW_MODS_ENABLED=0`.

## Known limits

- **Only text.** The Switch files' pictures (help and tutorial images) are left out because the
  Switch picture format does not work on the PS4. Mods that replace the Switch `Unicorn.CPK`,
  other archives or `exefs` are not supported.
- **Long text can run past its box** when the text replaces Chinese. (In English, Français,
  Deutsch, Italiano or Español the game breaks the lines itself and none of this applies.)
  Story lines over 54 characters are broken at spaces
  automatically, because the Switch story text has almost no line breaks. The speech bubble holds
  two lines: a story line of three or more lines (common in the fan English patches, #73) is
  broken into two when they fit the bubble, and otherwise drawn smaller so that it fits
  (`VW_TEXT_FIT=0` turns both off). Menus are left as they are, so a long translated menu text
  can run past its box. The start-up warning screen already runs past the screen edges with the
  Portuguese translation.
- **Letters the game's fonts lack show as a box.** The dialogue and menu fonts have the Latin
  accented letters: ã, ç, é and í were checked with Portuguese. Other letters and alphabets are
  untested.
- **Chapter title cards show one box** (#49) in the Chinese text languages: the large font
  used there has only the Chinese titles' characters. A Western language from the Switch version
  (above) has its own fonts.
- **Only the text is replaced.** Everything else in the language file stays as in your dump:
  with the fan English patch that is its English interface, with the official Asia dump the
  Chinese one.
- **French, German, Italian and Spanish Switch files are untested.** They are merged the same
  way as `Unicorn_US.CPK`, and they also carry a `GUI/Interface.mbs` file of their own.

## Reporting

Open an issue with the **Translation report** template. Attach the diagnostics ZIP (start the
program with `--export-diagnostics`), name the mod and where you got it, and add screenshots of
anything wrong: text in the wrong language, boxes instead of letters, text past its box.

More about mods in general: [MODS.md](MODS.md).

# Playing in another language (experimental)

The PS4 release this port runs has Japanese and Chinese text, and the fan English patch adds
English. Two kinds of mod add other languages, or change the English:

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
4. **Set the text language to Chinese.** Press **Insert** (R3+L2 on a controller) and set
   **Text Language** to **Traditional Chinese** or **Simplified Chinese**. The mod's text
   replaces those two. With **Japanese** or **Korean** the mod has no effect.
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
- **Long text can run past its box.** Story lines over 54 characters are broken at spaces
  automatically, because the Switch story text has almost no line breaks. Menus are left as they
  are, so a long translated menu text can run past its box. The start-up warning screen already
  runs past the screen edges with the Portuguese translation.
- **Letters the game's fonts lack show as a box.** The dialogue and menu fonts have the Latin
  accented letters: ã, ç, é and í were checked with Portuguese. Other letters and alphabets are
  untested.
- **Chapter title cards show one box** (#49), as with the fan English patch: the large font
  used there has only the Chinese titles' characters.
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

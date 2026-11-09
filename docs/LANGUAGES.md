# Languages: a simple guide

This page explains which language the game shows, how to change it, and how to add more. For
the details of fan translations and mods, see [LANGUAGE_MODS.md](LANGUAGE_MODS.md).

## The short version

| You have... | The game shows | To do |
|---|---|---|
| The Asia PS4 version **with the fan English patch** | English | Nothing |
| The official Asia PS4 version (no patch) | Traditional Chinese | Pick another language in the menu (below) |
| Either one, plus the **Switch version** dumped from your own console | Whatever you pick | Copy the Switch language file, then pick it (below) |

Voices are a separate setting: Japanese or English, in the game's own **Voice Language** option.

## Where to change the language

1. While playing, press **Insert** on the keyboard (or **R3 + L2** on a controller). The port's
   menu opens.
2. Find **Text language (on restart)** and pick a language.
3. Close the menu and **restart the game**. The language changes at the next start, not at
   once.

Your saves work in every language. You can switch back and forth.

> This is the port's menu, not the game's. Leave **Text Language** in the game's own options on
> **Game Default**.

## What each choice means

| Choice | What you get | What it needs |
|---|---|---|
| **Game default** | English with the fan English patch. Traditional Chinese without it. | Nothing |
| **Japanese** | The Japanese text | Nothing (it is in every PS4 version) |
| **Traditional Chinese** | Chinese. With the fan English patch: its English, shown the old way (see below) | Nothing |
| **Simplified Chinese** | Chinese. With the patch's first release: its English, shown the old way | Nothing |
| **Korean** | Korean | `Unicorn_KO.CPK` from the Korean PS4 release. The Asia version has none. |
| **English (Switch text or English patch)** | The official English from the Switch version, or else the fan English patch | The Switch file, or the patch |
| **Français, Español, Deutsch, Italiano (Switch text)** | French, Spanish, German, Italian | That language's file from the Switch version |

### English patch: "Game default" or "Traditional Chinese"?

Both show the patch's English text. The difference is how the game draws it:

- **Game default** (recommended) uses the game's own English mode: the fonts of the Western
  release, the game's own line breaks, and **chapter title cards with their title**.
- **Traditional Chinese** draws the English text with the Chinese release's fonts, the way the
  port did up to v1.4.0. Chapter title cards show **a single box** instead of the title (issue
  #49).

If you use a translation mod (Portuguese, your own JSON translation...), the port keeps the
Chinese way automatically, because the mod's text goes there. See
[LANGUAGE_MODS.md](LANGUAGE_MODS.md).

## Add French, German, Italian, Spanish or the official English

The PS4 game still contains everything for these languages except the text. The Switch version
has the text, in one file per language. You need the Switch version **dumped from your own
console** (the 1.05 update if you have it).

1. **Find the file** in the Switch dump's `romfs` folder:

   | Language | File |
   |---|---|
   | English | `Unicorn_US.CPK` |
   | French | `Unicorn_FR.CPK` |
   | German | `Unicorn_DE.CPK` |
   | Italian | `Unicorn_IT.CPK` |
   | Spanish | `Unicorn_ES.CPK` |

2. **Copy it into the `languages` folder**, inside the port's `data` folder. The port creates
   `languages` the first time it starts. You can copy several files.
   ```text
   data/
     languages/
       Unicorn_FR.CPK
       Unicorn_DE.CPK
   ```
   The `data` folder is next to the program: beside `Unicorn Overlord.exe` on Windows, beside
   the `.AppImage` on Linux.
3. **Pick the language** in the menu (Insert, or R3 + L2) and restart.

Two small things stay as they are: the **tutorial pictures** that contain words show the PS4
ones (the Switch pictures use a format the PS4 game can't read), and **a few system messages**
mention the Nintendo Switch.

**The file from the 1.05 update is best.** The file from the Switch base game (1.0.0) works
too, but it lacks the text of the options and messages added in later updates. Those few lines
show in the game's own language instead: English with the English patch, and **blank** with the
official Chinese dump (its letters are not in the Western fonts). The Italian 1.0.0 file also has
a typo of its own in the auto-advance time ("% lf sec"). The log then says:

```text
Languages: Unicorn_FR.CPK: 5 text sheets fitted to this version's entries (an older Switch archive: only its text is used): ...
```

## Did it work?

Open the newest file in `data/logs/` and look for these lines near the top:

```text
Languages: Unicorn_FR.CPK from languages: 34 files, 14 pictures left out (Switch format)
Text language: French
```

With the English patch on **Game default**:

```text
Languages: Unicorn_US.CPK from the English patch: 35 files
Text language: English
```

If the file is missing, the log says so and the game starts in its default language:

```text
Text language: French needs Unicorn_FR.CPK (from the Switch version, in data/languages), which is missing; the game's default
```

## Common questions

**I changed the language and nothing happened.**
Restart the game. The port's menu says "(on restart)" for this reason.

**The chapter title is a box.**
You are on Traditional or Simplified Chinese, or a translation mod is active. Pick
**Game default** (with the English patch) or a Switch language.

**The text is in Chinese but I have the English patch.**
Run setup again (**Change game folder** in the port's menu) and check that you picked the folder
with the patch. With the patch's second release the English is only in Traditional Chinese, so
Simplified Chinese shows Chinese.

**Can I play in Portuguese, or another language the game doesn't have?**
Yes, with a fan translation mod. See [LANGUAGE_MODS.md](LANGUAGE_MODS.md).

**Can I bring my Switch save over?**
Not yet. The Switch save is laid out differently, and a converter needs a real Switch save to
check against (issue #71).

**Something looks wrong.**
Please report it with the **Translation report** issue template, with a screenshot and the log
from `data/logs/`.

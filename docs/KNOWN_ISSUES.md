# Known issues

This is an early, work-in-progress port. What is listed here is known; please don't open a new
issue for it unless you have new information (a log, a save, a way to reproduce it).

## Test system

All numbers and test results here come from one laptop:

| Part | Model |
|---|---|
| CPU | AMD Ryzen 5 2500U (4 cores / 8 threads, 2.0-3.6 GHz) |
| GPU | AMD Radeon RX 560X (Polaris 11, 4 GB, laptop), on AC power |
| Integrated GPU | Radeon Vega 8 (not used by the game) |

Results from other hardware are welcome in the issues.

## How far the game has been tested

- Played through the prologue, into chapter 1 and its first real battle (Palevia Town). Saving
  and loading (Continue on the title screen) work.
- **Not tested yet:** the end of the first real battle, the world map and anything past
  chapter 1.

## Supported dump only

**CUSA36381 (Asia) version 01.04**: the base game with the 1.04 update merged on
top. Also the **Hong Kong 1.05 package** (`HP0177-CUSA36381_00-UNICORNOVERLORD0-A0100-V0105`,
#33): its `param.sfo` says version 01.00 (master 1.05), and its `eboot.bin` is another build with
the same code at other addresses, which `src/unicorn_build.h` maps. It has Traditional and
Simplified Chinese, Korean and Japanese text, no English; tested to the opening scenes with every
PC feature (Options rows, render resolution, 21:9, button icons, text language). The fan English patch only replaces the text files (`Unicorn_CN.CPK`, `Unicorn_TW.CPK`) and
`param.sfo`; its `eboot.bin` is the official one, so both work. Setup recognizes the patch by
its text, so both releases of it work: the first (one English file in both slots) and the second
(#58: an update package with English in `Unicorn_TW.CPK` only, its 493 re-wrapped story lines and
the 14 tutorial pictures in English; `Unicorn_CN.CPK` stays the official Simplified Chinese). The
second spells its files `UnicornPatch.cpk` and `Unicorn_TW.cpk`: copied over the base on Linux,
both spellings stay, and setup installs the patch's files under the names the game opens.

Text language: the game picks it from the PS4 system language, and on any language but Korean or
Chinese it shows Traditional Chinese (English with the English patch). With the first release
of the patch both Chinese choices show English; with the second, Simplified Chinese is Chinese. **Text language** in
the Insert menu (R3+L2 on a controller) picks Japanese, Traditional or Simplified Chinese at the next
start. Voice language is the game's own Voice Language option.

Korean: the Asia release has no Korean text. Its base game (01.00) and the 1.04 update both ship
only `Unicorn_CN.CPK` and `Unicorn_TW.CPK`, plus the Japanese text in `Unicorn.CPK`; the Korean
fonts (`Font_4K/*_KO*.ftx`) are there, a Korean text file is not. The executable would load
`Unicorn_KO.CPK` (Text Language: Korean) if one is put in the game folder; it is probably part of
the separate Korean release (untested). Without it, Korean falls back to the game's default.

The start program refuses other regions (US, EU, Japan) and other versions; they have different
executables and the port's code hooks would not match. It also refuses any other build of
`eboot.bin` (another region or update package): setup and every start check its SHA-256 against
the two known builds (#14). `VW_ALLOW_OTHER_EBOOT=1` starts one
anyway, for testing only.

## Performance

Measured on the test system above, 1080p:

| Scene | Draws per frame | FPS |
|---|---|---|
| Palevia Town battle map (Continue from a chapter 1 save) | ~1,100 | 60, steady |
| New Game opening (cavalry charge in the rain) | ~4,300 | 49-54 |
| First battle map, prologue (earlier measurement) | ~3,650 | ~53 |

- The busy scenes are limited by the CPU, not the GPU: the PS4's GPU reads its command stream in
  hardware, the port decodes it in software. Two threads share that work (stage A decodes, about
  3.8 µs per draw; stage B records Vulkan commands) and both are close to full in the opening.
  A faster CPU (higher single-core speed) helps most.

- 720p and 900p render resolutions lower the GPU's work, not the CPU's: they help weak GPUs,
  not the busy scenes above.
- The first time a scene is shown, its shaders are compiled, which can stutter. Later runs use
  the shader cache in `user/`.
- Starting the game: about 1 second until the game's own code runs (the image preparation is
  cached), then the game's warning and logo screens.

## Graphics

- **Chapter title card shows one box with the English patch** (#49): the card that opens a
  chapter ("Call to Action") shows a single square instead of the title. The game draws those
  titles in a large font that, in the Chinese releases the patch builds on, holds only the
  Chinese titles' characters. The rest of the chapter is unaffected. Not fixed yet: pointing the
  font at the English release's large font crashed the game when the card opened.
- **Ultrawide (21:9, 32:9) is experimental.** It was made on a 16:9 screen and checked there
  with black bars; nobody has tried it on an ultrawide monitor yet. Menus and text stay 16:9 in
  the middle. Please report how it looks with the **Ultrawide report** issue template (README,
  Ultrawide).
- The two GPU readback options are **on by default** because they are faster. If something
  looks wrong (black screen, missing or flickering graphics, wrong lighting or shadows), turn
  them off: in-game menu (**Insert**) → Performance → "Game reads GPU data without waiting" and
  "Deferred GPU readbacks", then **Apply and restart**. If the game then fails to start, the
  next start goes back to the previous options by itself.
- A render resolution change applies after a restart: the Insert menu has **Apply and
  restart**; in the game's Options screen it takes effect the next time the game starts.
- Every render resolution other than 1080p uses the game's own PS4 Pro mode at that size.
  Checked: 720p (logos, the opening, a battle map), 4K (title, menu and battle). 1440p was removed:
  the foreground blur at the bottom of the battle map broke into blocks while the camera moved.
  900p and 1800p have not been checked in game.
- Soft colour steps (banding) in dark fog and sky gradients, for example in the opening: the game
  draws these layers into 8-bit targets, as on a PS4, and the port adds no precision loss
  (checked frame by frame). It comes and goes with the scene.
- Button icons: Auto shows Xbox icons for every controller that is not a PlayStation or Nintendo
  pad, and for the keyboard (the game has no keyboard icons). The icons are chosen when the game
  starts; a prompt already on screen keeps its icon after a change, and whether new prompts follow
  a change without a restart has not been checked. Restart the game after changing it.
- On Linux with AMD GPUs, `RADV_DEBUG=nohiz` is set automatically to avoid rendering errors.
- On laptops with two GPUs the port picks the discrete one.
- **NVIDIA:** not tested by the developer (no NVIDIA hardware). v1.0.0 crashed at start on an
  RTX 4050 laptop inside NVIDIA's pipeline-binary support (v1.1.0 has the same code); v1.2.0
  leaves that feature off on NVIDIA, so shaders compile the normal way. NVIDIA takes workaround
  paths AMD does not (clip distances, integer fragment inputs, shared-memory barriers), so
  reports of graphics errors on NVIDIA are very welcome. GTX 10-series (Pascal) cards have no
  fragment barycentrics and use a different integer-input workaround (`VW_INTERP_INT_FIX=0`
  turns it off).
- Needs a Vulkan 1.3 GPU with sparse buffers and a recent driver: older Intel integrated graphics
  on Linux (Gen9, such as UHD 620) stop at start with "Required Vulkan feature unavailable" (#55). macOS is
  not supported.

## Audio

- The game outputs 7.1. On a stereo device the port mixes it to stereo itself (since v1.2.0);
  before, the system's conversion played it about 13 dB too quietly. The log line
  `Runtime: audio device N ch` shows what the device reported. `VW_AUDIO_DOWNMIX=0` restores
  the old behaviour.

## Builds

- New packages include `LICENSE`, third-party notices in `licenses/` and a help file. Windows
  downloads are ZIP archives; keep their notices alongside the executable. Older test builds
  may lack these files. Matching source and debug symbol archives accompany new builds.
- CI tests native code without a GPU or game dump. Successful CI does not establish a full
  playthrough or driver compatibility; packaged builds still need Windows/Linux gameplay tests.

## Not supported (by design, for now)

- Everything online: PSN sign-in, network features and trophies. They all report "signed out"
  or succeed without doing anything, so the game runs offline.
- Remapping sticks, triggers and the menu shortcut. Buttons can be remapped in the in-game
  Controls tab and are saved in `vw.ini`. The `VW_KEY_MAP` (keyboard) and `VW_PAD_MAP`
  (controller) environment variables override saved bindings, e.g. `VW_KEY_MAP="cross=Q"`.
- PS4 package (.pkg) installation of the game: the port installs an already extracted game
  folder. DLC packages are read as they are (README, DLC).
- The game's Options screen has the main PC settings; the GPU readback options, frames queued,
  background shader compilation, overlay layout and button remapping are only in the Insert menu.

## Open questions

- The 1.04 update's `UnicornPatch.CPK` is 1.0 GB while the base game's is 3.3 GB. Nothing has
  been found missing so far.

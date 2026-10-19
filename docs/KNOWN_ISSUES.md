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

Only **CUSA36381 (Asia) version 01.04**: the base game with the 1.04 update (English) merged on
top. The start program refuses other regions (US, EU, Japan) and other versions; they have
different executables and the port's code hooks would not match.

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

- The two GPU readback options are **on by default** because they are faster. If something
  looks wrong (black screen, missing or flickering graphics, wrong lighting or shadows), turn
  them off: in-game menu (**Insert**) → Performance → "Game reads GPU data without waiting" and
  "Deferred GPU readbacks", then **Apply and restart**. If the game then fails to start, the
  next start goes back to the previous options by itself.
- A render resolution change applies after a restart: the Insert menu has **Apply and
  restart**; in the game's Options screen it takes effect the next time the game starts.
- Every render resolution other than 1080p uses the game's own PS4 Pro mode at that size.
  Checked: 720p (logos, the opening, a battle map), 1440p and 4K (title, menu and battle).
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
- Needs a Vulkan 1.3 GPU and a recent driver. macOS is not supported.

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
- DLC and PS4 package (.pkg) installation: the port installs an already extracted game folder.
- The game's Options screen has the main PC settings; the GPU readback options, frames queued,
  background shader compilation, overlay layout and button remapping are only in the Insert menu.

## Open questions

- The 1.04 update's `UnicornPatch.CPK` is 1.0 GB while the base game's is 3.3 GB. Nothing has
  been found missing so far.

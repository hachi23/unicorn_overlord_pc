# Unicorn Overlord PC (vwport)

[![Build](https://github.com/hachi23/unicorn_overlord_pc/actions/workflows/build.yml/badge.svg)](https://github.com/hachi23/unicorn_overlord_pc/actions/workflows/build.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

A native Linux and Windows port of the PlayStation 4 version of **Unicorn Overlord**.

| Dump | State |
|---|---|
| CUSA36381 (Asia), app 01.04 (base game with the 1.04 update; Japanese, Chinese or, with the fan English patch, English text), or the Hong Kong 1.05 package | Prologue, chapter 1 and its first battle played; saving and loading work; every system function the game imports is implemented |

> **Work in progress.** The game has not been played to the end, and it runs at about 50-60 FPS
> on a mid-range laptop. Read [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) before reporting a bug.

### What it is

Not an emulator, and not a decompilation. The PS4 and a PC use the same kind of processor
(x86-64), so the game's own executable from your dump runs **directly on the PC's CPU**. What a
PC doesn't have is the PS4's operating system and GPU, so this project supplies both:

1. **Preparation** (`scripts/`): reads the game's `eboot.bin`, fixes up its addresses and links
   the game's bundled system modules into one image.
2. **Loader** (`src/probe.c`): loads that image into memory, applies a few small per-game code
   patches (for example the render resolution) and jumps into the game's code.
3. **Runtime** (`src/runtime_*.c`): about 700 PS4 system functions the game calls (memory,
   threads, files, audio, video decoding, controller, saves, and offline stand-ins for PSN),
   written for these games.
4. **Renderer** (`src/native_*.c`, `gpu/platform/`, **experimental**): the game's calls into its
   graphics library are taken over and drawn directly with Vulkan; its GPU shaders are translated
   to SPIR-V from your `eboot.bin` at the first start. There is no GPU emulation (see
   [below](#renderer-experimental)).
5. **Start program** (`launcher/`): a one-time setup that installs your dump, then starts the
   game; the in-game menu is Dear ImGui (`gpu/platform/vw_overlay.cpp`).

Only the dump listed above is supported.

> **No game files are included.** You need a dump of your own copy. This project is not
> affiliated with Sony Interactive Entertainment, Atlus, Sega or Vanillaware.

## Download

Download [the newest release for Windows and Linux](https://github.com/hachi23/unicorn_overlord_pc/releases/latest).
The release includes checksums, source and matching debug symbols.

Test builds of `main` are on the [`latest` pre-release](https://github.com/hachi23/unicorn_overlord_pc/releases/tag/latest):

- **Linux:** `Unicorn-Overlord-x86_64.AppImage`. Make it executable (`chmod +x`) and run it. It
  keeps its `data/` folder next to the `.AppImage`.
- **Windows:** `Unicorn-Overlord-PC-windows.zip`. Extract it into its own folder and run
  `Unicorn Overlord.exe`; it keeps `data/` beside itself. Keep `LICENSE` and `licenses/` with it.

These are untested builds of the newest code. See [docs/RELEASING.md](docs/RELEASING.md) for how
builds and releases are made.

## Requirements

- 64-bit Linux, or Windows 10 (1903+) / 11.
- A Vulkan 1.3 GPU with buffer device addresses, descriptor indexing, 64-bit shader integers,
  subgroup ballot/vote and push descriptors (AMD, NVIDIA and Intel GPUs of the last years with a
  current driver). If one is missing the game stops at start and the log names it
  (`Native: <GPU> has no ...`). Sparse buffers are no longer needed (v1.6.0).
- Your own dump of the game with the 1.04 update merged in (or the Hong Kong 1.05 package), as a folder containing `eboot.bin`,
  `sce_module`, `sce_sys` and the `.CPK` files (about 7 GB).

## Playing

**First start:** choose your dump's folder. The start program checks it (title and version),
installs it into `data/game` and starts the game. On the same drive the install takes a
moment and needs no extra space (hard links); from another drive it copies about 7 GB with a
progress window, and a cancelled install continues on the next start. **Later starts** go
straight into the game; preparing the game image is skipped while nothing changed.

Packaged builds contain the start program and the game executable. From the source tree:

```bash
python launcher/unicorn_launcher.py
```

The game's own **System → Options** screen (from the title or the in-game menu) has a **PC**
section at the end: display mode, VSync, render resolution (applied at the next
start), FSR 1 upscaling, sharpening and its strength, button icons, the stick dead zone and the
performance overlay.
Settings are saved in `vw.ini`; **Restore Defaults** resets them too.

> **Every render resolution change requires a restart.** Press **Insert** to open the overlay
> menu, change the resolution under **Performance**, then choose **Apply and restart**. If you
> change it in the game's **System → Options → PC** section, close and relaunch the game.

In game, **Insert** (R3+L2 on a controller) opens the port's overlay menu with every setting, and a
**Quit game** button at the bottom (the game has none of its own). It has
four tabs:

- **Image:** display mode, VSync, **Anime4K**, FSR 1 and RCAS sharpening. Anime4K (Fast or
  Quality) redraws the game's line art sharper; when the window is more than 1.2 times the game's
  image both ways, it also doubles the image before FSR 1 and the scaling. It costs GPU time per
  frame, measured on an RX 560X-class GPU at 1080p: Fast 3.3 ms, Quality 7.5 ms (a frame at 60 FPS
  has 16.7 ms), so Quality is meant for a stronger GPU.
- **Performance:** render resolution, frames queued and the GPU readback options.
- **Overlay:** the performance overlay.
- **Controls:** keyboard and controller button bindings, saved in `vw.ini`, and the
  **button icons** the game shows: Auto (follows the controller in use), PlayStation, Xbox or
  Nintendo. The Xbox and Nintendo icons are the game's own, from its Xbox and Switch releases.
  The **stick dead zone** (15% by default) stops sticks that rest off centre from drifting:
  raise it if the cursor or camera still moves by itself.

Settings are kept in `vw.ini` in the data directory. The Overlay tab also has a
**Change game folder** button to reopen setup.

**Updates:** at start the program looks for a newer release on GitHub and asks before
downloading it; the update keeps your settings, saves and installed game. Turn it off with
**Check for updates at start** in the Insert menu.

Something wrong? See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) and
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). Every run writes a log to `logs/` in the
data directory. When the game stops with an error, the start program offers to save a
diagnostics ZIP (the run log with your folders redacted, shown for review first) to attach to
your issue. No game files or saves are included.

### Render resolution

The game renders at 1920×1080 by default. Set another size in the menu (Performance, applied on
restart) or with `VW_RENDER_SCALE`: `720p` and `900p` for weak GPUs, `1800p` and `4K`
for supersampling: the game is drawn larger and averaged down to the window, which gives smooth
edges and sharper art (the port has no separate anti-aliasing setting; the game's art is mostly
2D sprites, which anti-aliasing filters blur). Every size uses the game's own PS4 Pro path (`VW_NEO_MODE=1`)
at that size, which lays out the 2D layers from the target size, so the framing and the HUD stay
the same at every resolution.

### Ultrawide (experimental)

**Aspect ratio** in the menu (Performance, applied on restart), `aspect_ratio=` in `vw.ini` or
`VW_ASPECT_RATIO`: `21:9` (2560×1080 at native resolution) or `32:9` show more of the battle map
and scenes at the sides instead of stretching them; menus and text stay 16:9 in the middle.
`VW_ASPECT_RATIO` also takes any `W:H`, such as `43:18` for 3440×1440. It combines with the
render resolution: 4K at 21:9 renders 5120×2160. On a 16:9 screen a wider image gets black bars
above and below.

It is experimental: it was made on a 16:9 laptop and has not been tested on an ultrawide monitor.
Reports are welcome, whether it looks right or wrong. Open an issue with the **Ultrawide report**
template and attach:

- the diagnostics ZIP: start the program with `--export-diagnostics` (or answer **Yes** in the
  error dialog after a crash). It holds the log with the `Aspect ratio:` and `Render scale:`
  lines and your `vw.ini` settings, with your folders redacted.
- screenshots of the battle map, a story scene with text, a menu, and anything stretched,
  cut off or misplaced (Win+Shift+S on Windows, Spectacle or your desktop's tool on Linux);
- your monitor's resolution and the display mode (Windowed, Borderless or Fullscreen).

### Renderer (experimental)

Since v1.6.0 the port draws with its own renderer, and the shadPS4 GPU emulation it used before is
gone. The game's calls into its graphics library are recorded and drawn directly with Vulkan;
the PS4 command buffers are no longer decoded. It is faster, most of all on weak GPUs: on an
RX 560X the New Game opening holds 60 FPS where v1.5.2 dropped to 36-54, and on a Vega 8
integrated GPU it runs 46-60 FPS where v1.5.2 ran 38-59.

On its first start it translates the game's shaders from your `eboot.bin` into
`<user data>/native_shaders` (a few seconds; again after a game or port update). Nothing from the
game is shipped with the port. Every display option works with it: render resolution, aspect
ratio, FSR 1, sharpening and Anime4K, and so do the text languages, the English patch, mods and
patches. The menu shows **Renderer: Native (experimental)** under About.

**It is experimental.** It was compared picture by picture with v1.5.2 through the prologue and
chapter 1 (story scenes, the battle map, menus, saving and loading, the English patch, French
from the Switch text, 21:9 and 32:9, the Hong Kong 1.05 package) on AMD GPUs on Linux. Windows,
NVIDIA and Intel GPUs, the Steam Deck and later chapters have not been checked by the developer.
If something is missing or looks wrong, open an issue with the diagnostics ZIP and a screenshot;
a screenshot of the same place with **v1.5.2** (the last release with the old renderer, still on
the releases page) helps most. The log lines that start with `Native:` are the useful ones.

### Other languages (experimental)

**New to this? Start with [docs/LANGUAGES.md](docs/LANGUAGES.md)**: which language you get, how
to change it, and how to add more, step by step.

With the Switch version dumped from your own console, the game also runs in English, French,
German, Italian or Spanish: copy that language's `Unicorn_XX.CPK` from the Switch dump into
`data/languages/` and pick it under **Text language** (Insert menu). Fan translations made for
the Switch version (`Unicorn_US.CPK`, such as the Brazilian Portuguese one) and translations you
write yourself as JSON files work as mods in `data/mods/`. Only the title menu and the opening
dialogue have been checked so far. Step by step:
[docs/LANGUAGE_MODS.md](docs/LANGUAGE_MODS.md). Reports are welcome with the
**Translation report** issue template.

### DLC

Put the DLC you own in a `dlc` folder in the data directory (next to `vw.ini`): the PS4
package as it is (for example `HP0177-CUSA36381_00-PRIVILEGEDEMBLEM-A0000-V0100.pkg`), or its
extracted folder. A `.zip` must be unpacked first. At the next start the log lists each one
(`DLC: PRIVILEGEDEMBLEM (...)`) and the game sees it as owned. Only the package's name is read:
Unicorn Overlord's DLC unlocks content that is already in the game. The game's DLC menus
show what it unlocked.

### Keyboard

The game's prompts show controller buttons (Xbox letters when you play with the keyboard only).
Press **F1** in the game to see which key is which button, with your own bindings; change them
in the port's menu (Insert) > Controls. The defaults:

| Key | Button |
|---|---|
| WASD | left stick |
| Arrows | right stick |
| Space | Cross |
| Left Shift | Circle |
| E | Square |
| Q | Triangle |
| 1 and 3 | L1 and R1 |
| R and F | L2 and R2 |
| Z and C | L3 and R3 |
| I, K, J, L | d-pad |
| Enter | Options |
| Tab (Backspace: its right side) | touchpad |
| Insert | the port's menu |
| F1 | the key list |

### Mouse

On the battle map the mouse moves the game's cursor (the glove); the system pointer is hidden
over the window there. Left click is Cross and right click is Circle, everywhere. Menus are
still chosen with the keys or a controller: the game has no pointer in its menus. The click
that brings the window to the front is not passed to the game. `VW_MOUSE=0` turns the cursor
movement off. Works with v01.04 and the Hong Kong 1.05 package.

## Building

Linux needs GCC or Clang, CMake, Ninja, Python 3, and the Vulkan, SDL3 and FFmpeg development
packages. `shell.nix` lists them.

```bash
git clone --recursive https://github.com/hachi23/unicorn_overlord_pc
cd unicorn_overlord_pc
bash build.sh                                  # out/vwport and out/gpu/libvwgpu.so
VW_GAME_DIR=/path/to/CUSA36381 bash run.sh     # builds, prepares the game and starts it
```

`run.sh` builds first and then runs `run.py`, which does the same on every platform. `run.py`
prepares the game image in `out/`, applies the mods and patches, and starts `vwport`. Saves
and shader caches go to `user/` in the data directory (`VW_DATA_DIR`, by default the port
folder).

On Windows, build in an MSYS2 CLANG64 shell. See
[packaging/windows/README.md](packaging/windows/README.md).

To package a release, run `python packaging/package.py unicorn`. On Linux it builds
`packaging/deps.sh` (pinned static FFmpeg with only the decoders the port uses, SDL3, zlib; a few
minutes the first time) and a `vwport` with them and the GPU library linked
in (`build.sh --static`), which needs only libc, libm and the Vulkan loader. `dist/<Title> PC/`
then holds the start program, `_internal/` (its Python runtime, the runtime scripts and
`vwport`), `LICENSE`, `licenses/` and `README.txt`. Releases also retain matching debug symbols and an archive of the source.

### Tests

```bash
python -m unittest discover -s tests
bash build.sh --test                           # C tests; the shader translator test needs spirv-val
bash tools/native/check.sh [path/to/eboot.bin] # everything, and every game shader through spirv-val
```

The C++ tests are `EXCLUDE_FROM_ALL` targets in [gpu/CMakeLists.txt](gpu/CMakeLists.txt). For
example:

```bash
ninja -C out/gpu gpu-platform-test && out/gpu/gpu-platform-test
```

To check that every system function a game imports is implemented, without starting it:

```bash
out/vwport out/boot-linked.bin --app0 <game folder> --check-imports
```

## How it works

| Part | Where | What it does |
|---|---|---|
| Preparation | `scripts/` | Reads `eboot.bin`, applies its relocations and links the game's own system modules into one image (cached in `out/` between starts) |
| Loader | `src/probe.c` | Loads the image, applies the per-game code patches (render resolution) and starts the game's code |
| Runtime | `src/runtime_*.c` | The PS4 system functions: memory, threads, files, audio, video, controller, saves and services |
| Renderer (experimental) | `src/native_rec.c`, `src/native_vk.c` | Hooks the game's graphics library (Gnmx), records each frame and draws it with Vulkan ([notes](docs/dev/NATIVE_RENDERER.md)) |
| Shader translator | `src/native_shader.c`, `src/native_shader_spirv.c` | GCN shaders from `eboot.bin` to SPIR-V at the first start (`tools/native/ns_dump.c` from the command line) |
| Graphics platform | `gpu/platform/` | Window, Vulkan device, presenter (FSR 1, CAS, Anime4K), and the PS4 libraries around drawing: GnmDriver, VideoOut, event queues |
| In-game menu | `gpu/platform/vw_overlay.cpp` | The Dear ImGui menu and FPS overlay; settings in `gpu/platform/vw_settings.cpp` |
| Per-game settings | `scripts/game_profiles.py` | Launch defaults and GPU workarounds for each title |
| Mods and patches | `scripts/mods.py`, `scripts/patches.py` | Loose-file mods and XML code patches ([docs](docs/MODS.md)) |
| Start program | `launcher/` | One-time setup (check and install the dump), then starts the game |
| Game hooks | `src/unicorn_*.h` | Render size, button icon layouts |

Developer notes (state, open problems, reverse-engineering findings) are in
[docs/dev/](docs/dev/): [handoff](docs/dev/unicorn/HANDOFF.md),
[native settings research](docs/dev/unicorn/NATIVE_SETTINGS.md),
[renderer](docs/dev/NATIVE_RENDERER.md) and [its tools](tools/native/README.md),
[Ghidra tools](tools/re/README.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bugs: open an issue with the bug report template and
attach the run log.

## Credits

This port stands on the work of others. In order of descent:

- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native port that this approach comes from.
- **Supermedo**: [bloodborne_pc for Windows](https://github.com/Supermedo/bloodborne_pc).
- **Felipe Prado**: [gow3_pc](https://github.com/FelipePrado0/gow3_pc), the God of War III port
  this repository was forked from.
- **cuesta4**: [shadPS4 fork](https://github.com/cuesta4/shadPS4), for its GNM blend rewrite,
  depth/stencil state and image aliasing fixes.
- **shadPS4 team**: the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader
  recompiler, which drew the game up to v1.5.2, and its PS4 function names and library layouts,
  which the port still follows (see [gpu/VENDOR.txt](gpu/VENDOR.txt)).
- **yogesh1239**: CI builds for Windows and Linux, run logs, contributing docs and templates.
- **rufaswan**: [Web2D_Games](https://github.com/rufaswan/Web2D_Games), whose notes on
  Vanillaware's sprite files (FMBS) made the Xbox and Nintendo button icons possible.

Libraries and tools used:

| What | Used for |
|---|---|
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | ATRAC9 audio decoding |
| [FFmpeg](https://ffmpeg.org) | Movies (H.264/HEVC), MP3/AAC audio, PNG |
| [SDL3](https://github.com/libsdl-org/SDL) | Window, keyboard and controllers |
| [Dear ImGui](https://github.com/ocornut/imgui) | In-game menu and overlay |
| [glslang](https://github.com/KhronosGroup/glslang) | Compiles the presenter's shaders at build time |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | `spirv-val` checks the translated shaders in the tests |
| [Vulkan](https://www.vulkan.org) headers and loader | Graphics API |
| AMD FidelityFX FSR 1 and CAS | Upscaling and sharpening |
| [Anime4K](https://github.com/bloc97/Anime4K) | Line-art restore and upscale (Image tab) |
| [zlib](https://zlib.net) | Support library |
| [DejaVu fonts](https://dejavu-fonts.github.io) | Menu font |
| [Pillow](https://python-pillow.org), [PyInstaller](https://pyinstaller.org) | Launcher images and packaging |

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.

Unicorn Overlord is a trademark of its owners. This project
contains no game code or assets; it only runs a copy you dumped yourself.

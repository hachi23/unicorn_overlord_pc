# Unicorn Overlord PC (vwport)

[![Build](https://github.com/hachi23/unicorn_overlord_pc/actions/workflows/build.yml/badge.svg)](https://github.com/hachi23/unicorn_overlord_pc/actions/workflows/build.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

A native Linux and Windows port of the PlayStation 4 version of **Unicorn Overlord**.

| Dump | State |
|---|---|
| CUSA36381 (Asia), app 01.04 (base game with the 1.04 update; Japanese, Chinese or, with the fan English patch, English text) | Prologue, chapter 1 and its first battle played; saving and loading work; every system function the game imports is implemented |

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
4. **GPU** (`gpu/`): the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core turns the
   PS4's GPU commands and shaders into Vulkan, with this port's multi-threaded draw pipeline on top.
5. **Start program** (`launcher/`): a one-time setup that installs your dump, then starts the
   game; the in-game menu is Dear ImGui (`gpu/shim/vw_overlay.cpp`).

Only the dump listed above is supported.

> **No game files are included.** You need a dump of your own copy. This project is not
> affiliated with Sony Interactive Entertainment, Atlus, Sega or Vanillaware.

## Download

Download [v1.0.0 for Windows and Linux](https://github.com/hachi23/unicorn_overlord_pc/releases/tag/v1.0.0).
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
- A Vulkan 1.3 GPU.
- Your own dump of the game with the 1.04 update merged in, as a folder containing `eboot.bin`,
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
start), FSR 1 upscaling, sharpening and its strength, button icons and the performance overlay.
Settings are saved in `vw.ini`; **Restore Defaults** resets them too.

> **Every render resolution change requires a restart.** Press **Insert** to open the overlay
> menu, change the resolution under **Performance**, then choose **Apply and restart**. If you
> change it in the game's **System → Options → PC** section, close and relaunch the game.

In game, **Insert** (R3+L2 on a controller) opens the port's overlay menu with every setting. It has
four tabs:

- **Image:** display mode, VSync, FSR 1 and RCAS sharpening.
- **Performance:** render resolution, frames queued and the GPU readback options.
- **Overlay:** the performance overlay.
- **Controls:** keyboard and controller button bindings, saved in `vw.ini`, and the
  **button icons** the game shows: Auto (follows the controller in use), PlayStation, Xbox or
  Nintendo. The Xbox and Nintendo icons are the game's own, from its Xbox and Switch releases.

Settings are kept in `vw.ini` in the data directory. The Overlay tab also has a
**Change game folder** button to reopen setup.

**Updates:** at start the program looks for a newer release on GitHub and asks before
downloading it; the update keeps your settings, saves and installed game. Turn it off with
**Check for Updates** in the game's Options (PC section) or in the Insert menu.

Something wrong? See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) and
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). Every run writes a log to `logs/` in the
data directory. When the game stops with an error, the start program offers to save a
diagnostics ZIP (the run log with your folders redacted, shown for review first) to attach to
your issue. No game files or saves are included.

### Render resolution

The game renders at 1920×1080 by default. Set another size in the menu (Performance, applied on
restart) or with `VW_RENDER_SCALE`: `720p` and `900p` for weak GPUs, `1800p` and `4K`
for smoother edges and sharper art. Every size uses the game's own PS4 Pro path (`VW_NEO_MODE=1`)
at that size, which lays out the 2D layers from the target size, so the framing and the HUD stay
the same at every resolution.

### Keyboard

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
| Tab | touchpad |
| Insert | the port's menu |

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
`packaging/deps.sh` (pinned static FFmpeg with only the decoders the port uses, SDL3, fmt, Zydis,
xxHash, zlib; a few minutes the first time) and a `vwport` with them and the GPU library linked
in (`build.sh --static`), which needs only libc, libm and the Vulkan loader. `dist/<Title> PC/`
then holds the start program, `_internal/` (its Python runtime, the runtime scripts and
`vwport`), `LICENSE`, `licenses/` and `README.txt`. Releases also retain matching debug symbols and an archive of the source.

### Tests

```bash
python -m unittest discover -s tests
bash build.sh --test
```

The C++ tests are `EXCLUDE_FROM_ALL` targets in [gpu/CMakeLists.txt](gpu/CMakeLists.txt). For
example:

```bash
ninja -C out/gpu graphics-settings-test && out/gpu/graphics-settings-test
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
| GPU | `gpu/shadps4/` | The shadPS4 video core with this port's threaded draw pipeline ([notes](docs/dev/parallel_gpu.md)) |
| In-game menu | `gpu/shim/vw_overlay.cpp` | The Dear ImGui menu and FPS overlay; settings in `gpu/shim/vw_settings.cpp` |
| Per-game settings | `scripts/game_profiles.py` | Launch defaults and GPU workarounds for each title |
| Mods and patches | `scripts/mods.py`, `scripts/patches.py` | Loose-file mods and shadPS4 XML patches ([docs](docs/MODS.md)) |
| Start program | `launcher/` | One-time setup (check and install the dump), then starts the game |
| Game hooks | `src/unicorn_*.h` | Render size, button icon layouts |

Developer notes (state, open problems, reverse-engineering findings) are in
[docs/dev/](docs/dev/): [handoff](docs/dev/unicorn/HANDOFF.md),
[native settings research](docs/dev/unicorn/NATIVE_SETTINGS.md),
[GPU pipeline](docs/dev/parallel_gpu.md), [Ghidra tools](tools/re/README.md).

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
  recompiler (vendored in `gpu/shadps4/`, see [gpu/VENDOR.txt](gpu/VENDOR.txt)), and its PS4
  function names and library layouts.
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
| [sirit](https://github.com/shadps4-emu/sirit), [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [glslang](https://github.com/KhronosGroup/glslang) | Shader generation |
| [Vulkan](https://www.vulkan.org) headers, [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Graphics API |
| AMD FidelityFX FSR 1 and CAS | Upscaling and sharpening |
| [Zydis](https://github.com/zyantific/zydis), [xbyak](https://github.com/herumi/xbyak) | x86 decoding and code generation |
| [fmt](https://github.com/fmtlib/fmt), [Boost](https://www.boost.org), [magic_enum](https://github.com/Neargye/magic_enum), [robin-map](https://github.com/Tessil/robin-map), [half](https://half.sourceforge.net), [xxHash](https://github.com/Cyan4973/xxHash), [zlib](https://zlib.net), [miniz](https://github.com/richgel999/miniz) | Support libraries |
| [DejaVu fonts](https://dejavu-fonts.github.io) | Menu font |
| [Pillow](https://python-pillow.org), [PyInstaller](https://pyinstaller.org) | Launcher images and packaging |

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.

Unicorn Overlord is a trademark of its owners. This project
contains no game code or assets; it only runs a copy you dumped yourself.

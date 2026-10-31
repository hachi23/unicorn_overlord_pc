# Unicorn Overlord (CUSA36381, Asia, v01.04) — handoff

Repository `hachi23/unicorn_overlord_pc`, branch `main`. A new clone needs
`git submodule update --init`. The game's x86-64 code runs natively, `src/runtime_*.c` replaces
the PS4 libraries, `gpu/shadps4` translates the GPU to Vulkan. This file is the port's history;
the current state and next steps are in `docs/dev/HANDOFF.md`. Linux builds need a clean shell
environment (no cross-toolchain CC/CXX/CMAKE_SYSROOT/LDFLAGS exported).

## State (2026-10-13)

- Boots, plays the opening, reaches the title (New Game / Load Game / Library / System, English
  through the EN patch), starts a new game, plays the prologue story scenes and the first battle
  (defence of the capital): tutorial, unit movement, unit panels, text log, minimap all work.
- 2026-10-13 (later): no missing imports are left (`--check-imports`: 0). Played past the
  prologue battle into chapter 1 (Alain and Lex, Scarlett in the cathedral) and the first real
  battle (deploy from the Command Post, move, fight, liberate and garrison Palevia Town). The
  game autosaves before that battle (`SAVEDATA0`, 5 MB, icon and param written).
- Not reached yet: the end of the first real battle, the world map, a load from the title, a
  movie playing in the game (CRI Mana is linked, but no movie was found in the CPKs yet).
- Performance on the test laptop (Ryzen 5 2500U, the game on the discrete RX 560X, AC power):
  first battle map ~19 FPS at first, ~27 after the draw-pipe batching, **~53 FPS** after the
  2026-10-13/14 work (below; frames take one or two 60 Hz vblanks). Goal: a steady 60.

## The dump

Asia version (CUSA36381): the base game (v1.01) with the 1.04 update applied, dumped from your
own console. Extract the base and update packages with a PS4 PKG extractor, then merge them the
way the console applies an update (update files replace base files). On Linux, hard-linking the
base files (`cp -al`) avoids a second copy of the 6.3 GB `Unicorn.CPK`. Names are matched
case-insensitively, as on the PS4: the update ships `Unicorn_CN.cpk`/`Unicorn_TW.cpk`, the
executable opens `Unicorn_CN.CPK`.

Open question: the update's `UnicornPatch.CPK` is 1.0 GB, the base's 3.3 GB. Nothing missing has
shown so far (everything through the first battle renders and the text is English).

## Running

```
VW_GAME_DIR=/path/to/CUSA36381 VW_DATA_DIR=$PWD/run_uo bash run.sh
python3 launcher/unicorn_launcher.py          # the launcher (data: run_uo/)
python3 packaging/package.py unicorn          # dist/Unicorn Overlord PC/: launcher + LICENSE
```
(with a clean shell environment). Saves, caches and vw.ini are
in `run_uo/`. `run.sh` builds and then runs `run.py`, so since 2026-10-13 UO runs with run.py's
defaults: deferred and stale readbacks on (graphics options in vw.ini). The battle rendered
correctly with them; switch them off in the menu if something looks wrong.

The per-game settings are in `scripts/game_profiles.py` (`CUSA36381`: the native PSN modules).

## Release package (2026-10-13)

`dist/Unicorn Overlord PC/` holds two files: the launcher (PyInstaller, one file) with run.py,
scripts/, patches/ and `bin/vwport` inside, and LICENSE. `vwport` links pinned static libraries
from `packaging/deps.sh` (FFmpeg reduced to H.264/HEVC/MP3/AAC/PNG decoding and MP4 demuxing,
SDL3, fmt, Zydis, xxHash, zlib; checksums in the script, built once into `out/deps/prefix`), so
it needs only libc, libm and the Vulkan loader (before: ~130 libraries, the system FFmpeg's
whole tree, tied to its soname). Each `--run` process uses its own unpacked vwport; a `vwport`
beside the launcher replaces it. Windows: `packaging/windows/build.sh` does the same with
`-static` (libc++ inside); not run yet.

Offline system libraries: `runtime_services.c` (users, system, PlayGo, DiscMap, devices),
`runtime_online.c` (network, PSN, trophies), `runtime_dialogs.c` (common dialogs, IME, sign-in),
sharing `runtime_offline.h`.

## Render resolution (PS4 Pro path)

`VW_RENDER_SCALE=1800p|4K` (or the launcher's Resolution, `render_resolution=` in vw.ini)
sets `VW_NEO_MODE=1` (sceKernelIsNeoMode returns 1) and, below 4K, patches the six `mov ecx,
imm32` operands of the game's width/height getters (`src/unicorn_render_scale.h`). Checked: 4K
title and battle. Native (1080p) is the base PS4 path. 1440p was removed (2026-10-19): on the
battle map the foreground blur at the bottom broke into blocks while the camera moved (Windows,
RX 9070 XT; 4K and 1080p clean). The probe also writes the size into the 3840.0f/2160.0f pair at
0xaf31a4 (one pass rectangle in FUN_00233cd0); whether that fixes 1440p was not checked.
The battle map has its own targets of min(screen, 2880x1620), the Pro's limit at any Pro size
(`unicorn_map_*` in the same header, 2026-10-31): the probe raises the limit to the render size, so
at 4K the map is no longer drawn at 2880x1620 and enlarged (#56).

## Graphics options in the game code (Ghidra survey, 2026-10-13)

The game has no graphics settings of its own (no AA, shadow or quality options; it is a 2D
sprite renderer). The only switch is the PS4 Pro check `FUN_00218680` (sceKernelIsNeoMode),
read through `FUN_0003f470` (bool) and the size getters:

| Where | What the Pro path changes |
|---|---|
| `FUN_0003e2b0`/`FUN_0003e2d0` (+ inlined in `FUN_0003cb60`) | Render/display size 3840x2160 instead of 1920x1080 (the patched immediates) |
| `FUN_0003fa20` (display setup) | `FUN_002153a0(ctx, 0xff, 1, 0)` instead of `FUN_00215330(ctx, 0xff, 0)`: the video-out/render target mode for 4K; then the viewport from the getters |
| `FUN_00041c50` | Viewport width/height floats from the getters |
| `FUN_00503ec0` (startup) | A larger memory budget (base: `0x29f19c00`) |
| `FUN_00232650`, `FUN_003871e0` | UI/text layout sizes from tables (ids 0x22e, 0x235) times 0.5: the Pro path loads `Font_4K/*.ftx` and other double-size UI assets |

So the Pro path is the game's own high-resolution mode (4K fonts and UI art), not only a bigger
render target: above 1080p it gives sharper text and UI as well as supersampling.
Image options on the port side: render resolution (supersampling to the window), FSR 1 and RCAS
(for a window larger than the render size), in the launcher and the in-game menu.

## What the port needed (commit 1f99c4c)

Imports: 720, 44 required modules. `out/vwport … --check-imports` lists
every import the game would stop at without running it; names come from shadPS4's
`aerolib.inl`.

- **Bundled PSN libraries run natively.** `libSceNpToolkit2.prx`, `libSceNpCppWebApi.prx` and
  `libSceJobManager.prx` from the game's `sce_module` are linked like libc and Fios2
  (`TITLE_MODULES` in `scripts/link_modules.py`): their ~200 C++ imports disappear and only the
  PSN C APIs reach the runtime (290 → 206 missing at that step).
- **PSN C APIs offline** (`src/runtime_services.c`): context/request/handle creation and callback
  registration return ids, terminate/delete/abort succeed, everything that needs an account
  returns NP_SIGNED_OUT; sign-in dialog finishes at once; share utility, HTTP options,
  NpWebApi2, Matching2, TUS/TSS, Score, WordFilter, InGameMessage, InvitationDialog.
- **Newer SDK kernel/libc**: `_sceKernelSetThreadDtors` (called as each guest thread finishes),
  thread-atexit hooks, POSIX rwlocks, `scePthreadGetname`/`AttrGetstack` (real host stack)/
  `Getthreadid`, `sceKernelCancelEventFlag`, `AvailableDirectMemorySize`, `Getdents`, `Chmod`,
  `Utimes`, `IsNeoMode` (0), sanitizer queries, `sceLibcHeapGetTraceInfo` (as shadPS4),
  Win32 RTC file times, `sceSysmoduleLoadModuleInternalWithArg`/`GetModuleInfoForUnwind`.
- **`sce::Json` (libSceJson2, a firmware library; only `libSceNpCppWebApi` imports it)**:
  `src/runtime_json.c`, values/objects/arrays/strings, `Parser::parse` (static) and
  `Value::serialize`; each guest object holds one pointer to host memory. Type numbers come
  from the library's own `set(6)`/`set(7)` before `referArray`/`referObject`.
- **Movies**: `sceVideodec2*` in `src/runtime_videodec.c` with the same FFmpeg decoder as
  libSceVideodec (layouts and error codes from shadPS4; picture information after each picture).
- **Others**: Rtc RFC 3339/2822/asctime parsing and RFC 3339 formatting,
  `sceSaveDataTransferringMount` (another title's save, read-only: not found unless present),
  `sceSigninDialogGetResult` (cancelled: signed out), `sceImeDialogGetPanelSizeExtended`,
  `sceHttpUriEscape`, `sceAjmStrError`, DiscMap `7C980FFB0AA27E7A`, ulobjmgr register/unregister.
- **NID names**: `src/import_names.inc` maps NIDs to names for the runtime's lookup; ~340 were
  added. POSIX imports are looked up as `#libkernel` (runtime.c canonical_name): name them so.

Nothing is missing any more; unit tests: `bash build.sh --test` (videodec, rtc, json, ...).

## Performance work

Measured in the first battle (scripts below drive there):
- ~6000 draws per frame (`perf probe` on `Vulkan::Rasterizer::DrawRecord`: 584,069 in 5 s at
  ~20 FPS): every sprite part, unit, building and effect is its own draw.
- Two threads saturate: `shadPS4:GpuComm` (PM4 decode, stage A) and `vw:DrawRec` (draw
  recording, stage B), ~6 µs per draw each. 60 FPS needs < ~2.7 µs.

Done — **draw pipe batching** (commit b426b8b, `vk_draw_pipe.h`):
each draw was handed to stage B with a seq_cst store to the line B polls, plus a read of B's
position per Begin, plus a locked add on a counter sharing a line with A's. Now draws are
published every 16 (`VW_PIPE_BATCH`) and before A drains, waits for ring space, yields
(`YIELD` in liverpool.cpp) or ends a submission; the space check uses the last position seen.
18.6 → 27.3 FPS. bbport (the project this port was forked from) still publishes per draw.

Then (2026-10-13/14, details in `docs/dev/parallel_gpu.md`, sections from "Constant engine dumps
without a drain" on): constant engine dumps no longer drain the draw pipe (31 → 39 FPS);
TLS model, inlined per-packet calls, vertex input memo, no locked add per draw, false-sharing
fixes, vertex streams through the constant ring, a guest-mapping memo and prepared-draw
prefetch (39 → ~53 FPS). Benchmark: `drive_battle.sh` to the first battle map in its
tutorial state (the units wait), then `ab.sh` (toggle A/B in one run; FPS and stage A/B µs
per draw).

Where it stands: stage A ~4.5 µs/draw, stage B ~4.0 µs/draw, GPU ~12.8 ms busy per ~18 ms
frame. The game paces its frames on end-of-pipe interrupts (`Guest equeue waits`), and stage
A reaches a frame's EOPs ~9 ms after the game submits (`EOP latency`): stage A's serial
decoding (~15 ms of work per frame) sets the frame rate. Next: split stage A's work across
threads (see the cleanup handoff, Open item 1).

## Test machine notes

- Two GPUs: Radeon RX 560X (Polaris 11, `card1`, 01:00.0) and Vega 8 (Raven, 04:00.0). The
  renderer prefers the discrete one; `out/vw-gpu-capabilities --device` prints the one it uses.
  Both get `RADV_DEBUG=nohiz` automatically (game_profiles.radv_nohiz).
- `perf` works for user-space profiling of the game; uprobes (`perf probe -x
  out/gpu/libvwgpu.so`, addresses from `readelf -Ws | c++filt`) need sudo.

## Driving the game (scratchpad scripts, not in the repo)

The pad is a file (`VW_PAD_FILE`): `press.sh <seconds> <buttons>` writes button names
(cross, circle, options, up, …) or `lx=`/`ly=` for the left stick. Frame dumps
(`VW_FRAME_DUMP_TRIGGER`) show the game's own output without screen capture.
- `uo_run.sh [seconds]`: launch through run.sh, log to `uo.log`, print the first STOP.
- `uo_to_battle.sh`: launch and press through title → New Game → story (≈120 Cross presses with
  1.7 s gaps) to the first battle; timing varies, a few extra presses may be needed.
- `fps.sh [seconds]`: frames presented while a dump is requested every frame (a lower bound).

## Lessons

- A missing import first: check whether it is only missing a name (`import_names.inc`), and
  whether the game ships the library in `sce_module` (link it natively) before writing a stub.
- Cross-thread handoffs per draw cost more than the work for sprite-heavy games: look for
  seq_cst stores and shared cache lines in `perf annotate` (the sample lands on the instruction
  after the stall).

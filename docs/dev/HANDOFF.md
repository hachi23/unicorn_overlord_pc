# Handoff (2026-10-27)

Read this first. The port's history and reverse-engineering notes are in
[unicorn/HANDOFF.md](unicorn/HANDOFF.md), the GPU pipeline in [parallel_gpu.md](parallel_gpu.md).
Development moves to the Windows desktop (7800X3D, RX 9070 XT) again; build there with MSYS2
CLANG64 ([../../packaging/windows/README.md](../../packaging/windows/README.md)). The Linux
laptop session that wrote this did everything below on Linux only.

## Where things stand

**v1.3.0 is released** (tag on `ae192a9`, 2026-10-27): CI built and published it. It was played
on Linux (the laptop) but **not on Windows**. The release steps ask for a Windows play test
([../RELEASING.md](../RELEASING.md)), so do that first (list below).

What went into v1.3.0:

| PR | Change | Checked on Linux |
|---|---|---|
| #44 | CI: Windows job exited 127 on every PR. `msys2-env.sh` put the runner's own older `C:/msys64` first on PATH inside a CLANG64 shell, so `build.sh` ran `deps.sh` with another MSYS2's bash. It now adds `C:/msys64` only outside CLANG64. | Windows CI green |
| #36 | Story dialogue, backlog and rapport text showed "–" (#34): the Options-row text hook took every id from `0x40000` as the port's own; the story sheet has 489,999 ids. Port ids now start at `0x100000`. | New Game through the tutorial battle: Josef/Alain on the horse and the Valmore/Baltro throne room show text (the scenes reporters listed) |
| #37 | Ultrawide 21:9 / 32:9, Hor+ (#29). **Marked experimental** (menus, README, KNOWN_ISSUES); "Ultrawide report" issue template; the log prints `Aspect ratio: <ratio> (<w>x<h>), experimental`. Nobody has tried a real ultrawide monitor. | 16:9 unchanged; unit test |
| #38 | Stick dead zone 15% (0–40%) (#28); Check for Updates row removed from the game's Options screen (it holds 11 PC rows, ids 21..31, all used now). | settings test |
| #39 | Windows: guest memory failure (error 1450, "Error 3", #35) stops with a message; launcher offers to end a leftover `vwport.exe`. | tests only (Windows code) |
| #40 | Setup warns when `eboot.bin` is another build of v01.04 (#14's reporter). | tests |
| #43 | Anime4K restore/upscale (#32), Insert menu → Image. | Polaris: Fast 3.3 ms, Quality 7.5 ms at 1080p |
| #46 | Xbox icons: Cross showed **B**, Circle **A** (#41). In `Interface_Button.mbs` `BUTTON_A_3`/`BUTTON_B_3` draw their own letter while the groups are named by Switch position; A/B now take each other's Xbox icon. X/Y were already right (Square shows X; swapping them too was wrong, seen in Options). | battle map: Cross prompt green A, Circle (Paused) red B |

#36–#43 were merged through one integration PR (#45): the ruleset requires branches to be up
to date with `main`, so merging them one by one would have cost a CI round each. Merging a
branch's unchanged head marks its PR as merged.

Performance A/B on the laptop (New Game opening, `VW_VSYNC=0`, ~4,200 draws/frame), `main`
before the merges against after: 51.7 vs 51.8 FPS and 53.7 vs 55.7 FPS (two runs each; the
spread is heat). No regression. All native and Python tests pass.

## Open work for the next agent, in order

1. **Windows play test of v1.3.0** (the release zip, not a dev build): first-start setup,
   title, New Game opening, Continue into the Palevia battle, System → Options PC rows
   (11, the last is Performance Overlay), Insert menu: Aspect Ratio 21:9 then Apply and restart
   (black bars on a 16:9 screen, wider scene), Anime4K Fast/Quality, Stick Dead Zone with a
   controller, Xbox controller prompts (Cross prompt = A), save and load. Report anything
   broken as a fix PR and a v1.3.1.
2. **Pull request #27, Text Language** (branch `claude/text-language`, updated 2026-10-27 to
   `f756bf8`, a merge of `main`). The owner decided: the setting lives **only in the Insert
   menu** and `vw.ini` (`text_language=`), not on the game's Options screen (no room: 11 rows).
   The merge resolved every conflict that way: native row removed, `vw.ini` keeps the key,
   the test asserts there is no "Text Language" row. Setup (`launcher/vw_launcher.py
   choose_game`) asks for the dump kind first (#27), then warns about another `eboot.bin`
   build (#40); the official Asia copy and the English-patched one share the supported eboot
   (SHA-256 `de8e4970…`), so the two checks don't overlap. Python tests pass (127); **the
   native build and tests were not run** (stopped for the move). To do: build, run
   `packaging/test-native.sh`, wait for CI on the PR, then check on Windows: setup with the
   English dump, Insert menu → Text language → Japanese → Apply and restart → Japanese text,
   back to Game default. Then merge. When it ships, close #14 with a note (its last open point
   was that reporter's official Asia copy).
3. **Issue #47, RX 470 on Windows does not start** (v1.2.0): `Failed to create graphics
   pipeline: ErrorUnknown` 1.4 s in, during pipeline warm-up (`vk_graphics_pipeline.cpp:437`).
   AMD proprietary driver 2.0.279 (Vulkan 1.3.260, about late 2023); pipeline binaries are off
   there (no `VK_KHR_pipeline_binary`), so it is a plain `vkCreateGraphicsPipelines` failure
   in the driver. Not answered yet: the owner should be asked before replying. Suggested reply:
   install AMD's legacy driver for RX 400/500 (Adrenalin 26.5.2, May 2026), try v1.3.0, attach
   the new log. If it still fails, the next step is to log which pipeline fails (shader hashes,
   key) and try that pipeline's state on the 9070 XT with the same extensions turned off.
4. **Audio too quiet on Windows** (owner's report, from 2026-10-20). v1.2.0 shipped a stereo
   downmix at full volume (#24); not re-checked since. Ask the owner whether it is still quiet.
5. **Close as not planned:** #42 (Android port: the port runs the PS4's x86-64 code natively,
   phones are ARM). #33 (Hong Kong 1.05): the owner said to ignore it.

## Driving the game for tests

Scripted pad input needs no window focus: `VW_PAD_FILE=<file>` holds tokens (`cross`, `down`,
`lx=0..255`, ...), re-read when the file changes; frame dumps with
`VW_FRAME_DUMP_TRIGGER=<file>` and `VW_DUMP_DIR`.

- Windows: `tools/uo_pad.ps1 -Launch`, then `tools/uo_pad.ps1 "cross,wait1000,dump"`;
  `tools/uo_shot.ps1 <png>` screenshots the window.
- Linux: `tools/uo_pad.sh --launch <dump>`, then `tools/uo_pad.sh "cross wait1000 dump"`.
- Saves from reporters: `tools/import_save.py <savedata/1/CUSA36381> <slot> <UCSAVEFILE.DATA>`
  (the Load list comes from a table in `GLOBALDATA/UCGLOBALDATA.DAT`; copying the file is not
  enough). Close the game first.
- Route: boot ~40 s, Cross (skip the opening), Cross (Press Any Button); the title cursor is on
  Continue. New Game is Down, Cross. Pressing Cross every ~2 s gets through the opening to the
  tutorial battle (~5 min). There: Cross on Ilenia's unit, Cross (Move), stick to Valmore's
  crowned 9 (`lx=80+ly=240:500` from Ilenia, then small nudges), Cross, Cross (Battle), then
  **wait without input**: Cross spam reopens Ilenia's menu and pauses the battle. Then once
  more against the next crowned 9; the story scenes follow.
- Benchmarks: `VW_VSYNC=0 VW_FRAME_STATS=1`, read the `Frame stats:` lines (FPS, us/draw,
  draws/frame) in the New Game opening.

### Earlier (2026-10-20, v1.1.0 and v1.2.0)

v1.2.0: NVIDIA pipeline-binary crash (#23, `VK_KHR_pipeline_binary` off on NVIDIA,
`VW_PIPELINE_BINARIES=0`), Windows audio 7.1 → stereo downmix at full volume (#24).
v1.1.0 (PR #22): AppImage Wayland crash (`restore_library_path`), Apply and restart exit 23,
900p → 1080p rendering at 4K, signature-mismatch 4K, Change game folder 30 s wait, atomic
`vw.ini`/`launcher.json` writes, save backup rotation.

How the updater decides: `scripts/updates.py` docstring. The running version is
`_internal/runtime/VERSION` (`git describe --match 'v[0-9]*'`); `vX.Y.Z-N-g<hash>` is newer than
vX.Y.Z, a version without a `v` tag is older than every release. `check_updates=0` in `vw.ini`
or `VW_UPDATES=0` turns the check off.

## State

Unicorn Overlord (CUSA36381 v01.04) boots, plays the prologue and chapter 1's first battle,
saves and loads. Test laptop: Ryzen 5 2500U (4c/8t), RX 560X, CachyOS; a Windows desktop
(7800X3D, RX 9070 XT) runs it too. Linux and Windows builds come from CI
(`.github/workflows/build.yml`); `main` is protected by the "Protect main" ruleset (pull
requests only, Linux and Windows checks required, no force-push or deletion).

Done on 2026-10-19 (pull request #16):

| Change | Where | Checked |
|---|---|---|
| Image preparation cached between starts | `scripts/prepare_cache.py`, `run.py` | launch → game code 3.2 s → 0.75 s (source), 0.86 s (package) |
| One-folder package (no self-unpacking) | `packaging/package.py`, `packaging/linux/appimage.sh` | ~470 → ~90 ms per program start; CI builds both |
| Setup installs the dump into `data/game` | `launcher/vw_launcher.py` | real dump hard-linked in 0.26 s; copy/cancel/resume in tests |
| 720p and 900p render sizes | `src/probe.c` (`unicorn_render_size`) | 720p: logos, opening, battle map framed as at 1080p |
| Xbox and Nintendo button icons | `src/unicorn_button_icons.h`, `src/probe.c`, `src/runtime_pad.c` | Xbox LS prompt in the tutorial; set at start |
| PR #13 merged (notices, diagnostics, native CI tests) | `packaging/notices.py`, `scripts/diagnostics.py` | 99 Python tests, `build.sh --test`, 15 GPU tests |
| Odin Sphere removed | everything | builds and tests pass |

Done later on 2026-10-19 (branch `claude/native-settings`, on top of #16):

| Change | Where | Checked |
|---|---|---|
| PC section in the game's System → Options (10 rows) | `src/unicorn_native_settings.h`, `gpu/shim/vw_settings.cpp` | title and in-game screens; choice, switch, slider, close/reopen, Restore Defaults; `vw.ini` |
| Ghidra image: base-relative pointers were all written as 0 (vtables empty) | `tools/re/make_ghidra_elf.py` | slider vtable `+0xe8` now `0x73add0` |

Details: [unicorn/NATIVE_SETTINGS.md](unicorn/NATIVE_SETTINGS.md). A Ghidra project made
before this fix has no vtables or function tables: re-import a new `eboot_ghidra.elf` (about
15 minutes, [../../tools/re/README.md](../../tools/re/README.md)).

## Findings worth knowing

- **Benchmarks: run with `VW_VSYNC=0`.** With VSync on, KWin throttles presentation of a
  window it considers hidden (another desktop, covered): the game then shows 20 FPS and every
  per-draw cost looks 9× higher (stage B waits on present, stage A spins on stage B). The game
  paces itself at 60 Hz either way.
- **Benchmark scene:** New Game → the opening (cavalry charge in the rain): ~4,100-4,400
  draws/frame, 51-52 FPS on the laptop, stage A ~3.9 µs/draw (~85% busy), stage B ~77% busy.
  Light scene: Continue → Palevia battle map, ~1,100 draws/frame, 60 FPS.
- **Boot:** the game decompresses ~1.5 GiB (≈300 files) of CRILAYLA data while its warning and
  logo screens show, on one thread (`CRI FS Data Decompress`). A native decoder was 1.28× faster
  in a benchmark but equal in the game (memory-bound); it was removed. What would help is
  decompressing the known boot files ahead of time on several threads.
- **Opening "banding":** every pass of the opening draws into RGBA8 targets, as on a PS4; frames
  show no lost precision. A deband filter in the presenter would be the only fix.
- **Button icons:** the sprite file has four variants per button (Switch, PS4 dark, PS4 light,
  Xbox; rufaswan's Web2D_Games documents the format). Changing the setting while playing
  switches the style at once (`Runtime: button icons: Nintendo` in the log); whether icons
  already on screen change was not checked.
- **bbport (bloodborne_pc 0.6) replaced shadPS4** with its own command processor, shader
  translator and renderer (2026-10-13/15). It is still a PM4 → Vulkan translator with the same
  two-stage pipeline; its measured CPU gains on 8-thread CPUs come from fewer draw-prep workers,
  and its constant "resolve workers" gave no FPS there. Its button prompt swap (textures at bind
  time) is how live icon switching could be done here.

## Tried and dropped (2026-10-19)

- Command-stream prefetch in `ProcessGraphics` (the header load is its hottest instruction):
  3.92 → 3.89 µs/draw, same FPS. Reverted.
- One draw-prep worker instead of four on 8 threads (bbport's finding): within noise here.

## Next steps

1. **Play further** (world map, chapter 2+): nothing past chapter 1's first battle is tested.
   A 1.0 needs a full playthrough on Linux and Windows, ideally by testers on other hardware.
2. **CPU: both GPU stages are near full in busy scenes.** Stage A (decode 35%, packet building
   ~26%: constant/vertex ring copies) and stage B (flat profile: bindings, vertex inputs,
   begin rendering). Plan, in order:
   - move the ring copies to resolve workers (bbport `BB_RESOLVE_WORKERS`: stage A 4.4 → 3.3 µs
     per packet there); the copies must finish before the packet's end-of-pipe signal and before
     a later constant-engine dump that writes their bytes (`BeforeConstRamDump`);
   - then stage B's pure lookups (buffers, vertex/index, textures) to stage A's workers, with
     generation checks on stage B (bbport's unimplemented "stage 2").
   Measure in the opening with `VW_FRAME_STATS=1 VW_VSYNC=0` (us/draw, idle, EOP latency).
3. **Native settings, what is left:** a restart prompt for Render Resolution in the game's
   screen (now it applies at the next start); the PC texts are English only. The screen is
   full (11 PC rows): a new row means dropping one.
4. **Live button icon switching:** swap the icon texture at bind time (bbport
   `vk_button_prompts.cpp`) instead of relying on the sprite table being re-read.
5. Optional: deband filter; parallel boot decompression.

## Working on it

- Windows build: MSYS2 CLANG64 shell, `bash build.sh` (development) or
  `bash packaging/windows/build.sh unicorn` (the release folder); details and the Linux/Windows
  differences in [../../packaging/windows/README.md](../../packaging/windows/README.md).
- Linux build: `bash build.sh` (unset a cross toolchain's CC/CXX/CMAKE_SYSROOT/LDFLAGS first,
  and drop its directory from PATH: a `vulkan/vulkan.h` not found error means it is still there).
  Tests: `python -m unittest discover -s tests`, `bash build.sh --test`, the GPU tests
  (`packaging/test-native.sh` lists them).
- Run: `VW_GAME_DIR=<dump> VW_DATA_DIR=run_uo python3 run.py`; scripted input
  `VW_PAD_FILE=<file>` (button names), frame dumps `VW_FRAME_DUMP_TRIGGER=<file>` +
  `VW_DUMP_DIR`, draw lists `VW_CAPTURE_TRIGGER`, decompression stats `VW_CRILAYLA_STATS=1`.
- Title menu from boot: ~40 s, Cross (skip the opening), Cross (Press Any Button), then
  Continue / New Game (Down) / Load Game.
- Ghidra: [../../tools/re/README.md](../../tools/re/README.md).
- Commits on the public repository use the GitHub noreply address
  (`88461786+hachi23@users.noreply.github.com`).

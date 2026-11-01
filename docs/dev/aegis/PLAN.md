# 13 Sentinels: Aegis Rim on vwport: plan and handoff (2026-11-01)

Goal: run **13 Sentinels: Aegis Rim, CUSA19610 (US), app 01.02** on the vwport runtime as a
**second game in this repo** (one runtime, a per-game table; not a fork). English text and
voice, plus the PC features Unicorn Overlord has.

Written on the Windows desktop against `main` at d183d18 (PR #68). The Linux laptop's copy of
this repo is newer: **rebase this plan's one code change onto it and re-check every file and
function name below against that tree before relying on it.**

## Background

- Releases: PS4 (JP 2019, West 2020), Switch 2022. No official PC version.
- One other PC port exists: kxn/13_sentinels_aegis_rim_pc (GPL-2.0-or-later, based on
  bloodborne_pc, created 2026-10-21, v1.1.0). It supports only the Asia build CUSA18093, which
  has **no English** (CN/TW/KO). Its open issue #1 is light flicker in some scenes; its
  changelog lists a "Linux character-lighting difference". Game-specific code worth reading:
  `gpu/games/aegis_*.cpp` (damage rumble, save thumbnail), `gpu/shim/bbport_guest_code_hooks.cpp`
  (code-hook trampolines), `scripts/aegis_prepare.py`, `scripts/aegis_verify.py`. Its addresses
  are for a different eboot and will not match ours.
- Dump layout (US 01.02): `eboot.bin` (the update's), `sce_module/` = `libc.prx`,
  `libSceFios2.prx`, `libSceNpToolkit2.prx`; data in `ROBO_{US,FR,GE,IT,SP}.CPK` (base),
  `ROBO_PATCH_US.CPK`, `ROBO_PATCH2_{US,FR,GE,IT,SP}.CPK` (update). Merged `param.sfo` is the
  update's (CATEGORY `gp`, APP_VER 01.02); base is `gd` 01.00. Content ID
  `UP0177-CUSA19610_00-13SENTINELSEFIGS`.

## Setting up on the Linux laptop

1. **Dump.** Copy the merged dump folder from the desktop (about 4.6 GB; it was merged with
   hard links, a normal copy turns them into files). Or extract the base and 01.02 PKGs again
   and copy the update's files over the base, overwriting.
2. **Branch.** `git switch -c claude/aegis-rim` from the up-to-date `main`, then add the
   profile in `scripts/game_profiles.py`:
   ```python
   'CUSA19610': {  # 13 Sentinels: Aegis Rim (US)
       'native_modules': ('libSceNpToolkit2.prx',),
   },
   ```
   Without it the first boot stops at `tlpXm5KLgqk#libSceNpToolkit2`
   (`sce::Toolkit2::NP::V2::Core::Response<Empty>` constructor).
3. **Run.**
   ```sh
   VW_GAME_DIR=<dump> VW_DATA_DIR=<aegis data dir> VW_ALLOW_OTHER_EBOOT=1 VW_UPDATES=0 \
   VW_FRAME_CAPTURE_KEY=1 ./run.sh
   ```
   Keep a separate data dir so UO's `vw.ini`, saves and prepared image are untouched. F11 dumps
   a frame (`tools/raw2png.py`). `VW_PAD_FILE` and `ydotool` drive it unattended (see the main
   HANDOFF).
4. **Ghidra** (tools/re/README.md, unchanged steps; analysis took about 4 minutes):
   ```sh
   python scripts/prepare.py <dump> --out <re>/out
   python scripts/link_libc.py <dump> --out <re>/out
   python scripts/link_modules.py <dump> --out <re>/out   # after step 2, so Toolkit2 is linked
   python tools/re/make_ghidra_elf.py . <re>/out <re>
   GHIDRA_HEADLESS_MAXMEM=10G <ghidra>/support/analyzeHeadless <re>/ghidra-project aegis \
       -import <re>/eboot_ghidra.elf -scriptPath tools/re/ghidra_scripts \
       -postScript LabelImports.java <re>/imports.tsv
   ```

## What is known (measured on the desktop)

| Check | Result |
|---|---|
| prepare / link_libc / link_modules | run unchanged: entry `0x3bdbd0`, image 23,253,824 bytes, 426 imports, 52,375 relocations, 38 required modules |
| Imports vs Unicorn Overlord | **424 of 426 already imported by UO**; the other two (`time`, `localtime_s`) are libc, served by the native libc |
| Linked (with Toolkit2) | libc@0x1630000, Fios2@0x1760000, Toolkit2@0x18f0000; 159 native bindings, 582 imports to the host runtime |
| Ghidra | 426 imports, 350 named, 384 thunks named |
| Boot with the UO build (RX 9070 XT, Windows) | 120 s, **no unsupported-import stop**. Vulkan presenter up (SDK 0x09508001), Xbox 360 pad, WASAPI audio (8 ch mixed to stereo), Ajm (ATRAC9), save `SAVEDATA00` created then mounted (later read-only), CRI FS threads, 12 `RenderDLThreadLocal` workers, `CriManaDecodeThread` (a movie started) |
| Not yet known | **what is on screen**; whether input reaches menus; any battle |

Open questions from the boot:
- About 77 short-lived threads named `guest` per run. Find their creator (Ghidra: callers of
  `scePthreadCreate` with that name) and decide: job pattern or a retry loop over something we
  answer wrong.
- Save data mounted read-only on later mounts: check that this is the game's own request.
- No `sceKernelIsNeoMode` import. UO's render resolution forces the PS4 Pro path; that path
  may not exist here, so resolution needs a different hook.

## Plan

### Phase 1: playable
1. Boot to the title with frame dumps at logos, title, new game, first scene. Fix in the order
   they appear. Expected: Mana/USM movies (Videodec2), US CPK loading, 2D adventure scenes.
2. **Destruction-mode battles** (real 3D, particles, post-processing): the main GPU risk.
   Expect shader-recompiler, buffer-cache and readback issues. Compare with kxn's flicker and
   lighting notes.
3. Save, load, autosave; trophies and PSN answered offline (Toolkit2, NpManager, Http,
   SigninDialog: 43 imports).
4. Performance pass with the multi-threaded draw pipeline; record FPS in adventure and battle.

### Phase 2: two games in one repo
1. Every place that assumes CUSA36381 becomes a per-game table: `scripts/patches.py`
   (`eboot_supported`, sha256s), `run.py` (error text), `launcher/vw_launcher.py` (game info,
   dump picker, `check_game`), `src/probe.c` (title-ID checks, render-scale and button-icon
   install), `scripts/game_profiles.py`.
2. Keep the `unicorn_*.h` hooks gated on UO's eboot hash; new hooks go in `src/aegis_*.h`
   gated on this eboot's hash (record it on the Linux machine).
3. Tests: extend the Python tests for game detection with both title IDs.

### Phase 3: PC features (Ghidra on this eboot)
In order of value/effort:
1. **Render resolution**: callers of `sceVideoOutRegisterBuffers` and render-target setup,
   1920/1080 immediates (`FindImm`).
2. **Button icons** (Xbox/Nintendo): the Switch release has the art; find the icon texture
   loader as for UO (`src/unicorn_button_icons.h`).
3. Settings: the Insert overlay works with no game work. Native rows in the game's options
   screen (UO: `src/unicorn_native_settings.h`, docs/dev/unicorn/NATIVE_SETTINGS.md) later.
4. Ultrawide: last; adventure 2D layers and the battle camera each need work.
5. Optional, from kxn's ideas: damage rumble, save thumbnails.

### Phase 4: release
Launcher and README for two games, KNOWN_ISSUES section, CI, release notes. Keep PKG and
download filenames and personal paths out of docs (see the main HANDOFF).

## First session on Linux
1. Steps 1-3 of setup; one run; save the log and F11 frames.
2. If it reaches the title: try new game, read the log, note the first visual bug.
3. If it shows nothing: check the log for the first error after `CriManaDecodeThread`, then
   the `guest` thread question.
4. Update the "What is known" table here.

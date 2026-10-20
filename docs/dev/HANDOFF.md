# Handoff (2026-10-20)

Read this first. The port's history and reverse-engineering notes are in
[unicorn/HANDOFF.md](unicorn/HANDOFF.md), the GPU pipeline in [parallel_gpu.md](parallel_gpu.md).
Development moves to the Windows desktop (7800X3D, RX 9070 XT) from here; build there with
MSYS2 CLANG64 ([../../packaging/windows/README.md](../../packaging/windows/README.md)).

## Open work for the next agent (2026-10-20, evening)

v1.2.0 is released (#23 NVIDIA pipeline-binary crash, #24 Windows audio downmix). In order:

1. **Pull request #27** (branch `claude/text-language`): test, then merge. It holds:
   - **Text Language** (`text_language=` in vw.ini, `VW_TEXT_LANGUAGE`): a row in System >
     Options (PC) and in the Insert menu (Overlay tab), applied on restart. The probe replaces
     the game's choice at `0x233e34` (checked bytes; `src/probe.c`
     `install_unicorn_text_language`). Game values: 0 Japanese (no language CPK: the base
     text in `Unicorn.CPK`), 6 Korean, 7 Traditional, 8 Simplified Chinese.
   - **Setup asks which dump** (`launcher/vw_launcher.py` `choose_dump`, `detect_dump`): the
     Asia version with the fan English patch, or the official Asia version, each with its
     caveats; the official one picks its text language. The folder is checked against the
     choice (the English patch has one English file in both `Unicorn_CN.CPK` and
     `Unicorn_TW.CPK`).
   - Insert menu title "PC settings" when the game's title is not Latin text (showed boxes).
   - `build.sh --static` on Windows builds in `out/gpu-static`: sharing `out/gpu` with the
     development build kept MSYS2's shared FFmpeg from the cache ("unable to find library
     -ldl"). The local release build works again (`packaging/windows/build.sh`; if `py` is
     not found inside MSYS2, run `py -3 packaging/package.py unicorn` from a normal shell
     with `clang64/bin` on `PATH`).
   - Docs: KNOWN_ISSUES (both dumps, text language, no Korean), TROUBLESHOOTING (base game
     is app 01.00).
   Checked on Windows with the official Asia dump: Options in Japanese and in Simplified
   Chinese, default Traditional Chinese, Korean falls back with a log line, the PC row shows.
   Not checked: a full first-time setup through the new window, Japanese past the menus
   (story text, fonts), the English-patch dump with the new build.
   **Packaging deletes `dist/<Title> PC/` including `data/`**: move `data/` aside first
   (done by hand on 2026-10-20) or teach `package.py` to keep it.
2. **Findings behind #27** (also in KNOWN_ISSUES):
   - The fan English patch keeps the official `eboot.bin` and `sce_module` (same SHA-256),
     and `Unicorn.CPK`/`UnicornPatch.CPK` are byte-identical. It replaces only
     `Unicorn_CN/TW.CPK` (one English file in both: 32 MsgSheets + `GUI/Interface.mbs`, no
     `Help/*.ftx`, so 14 tutorial pictures fall back to the base ones), `param.sfo` and
     package metadata.
   - Language choice (eboot `0x233e2f`): system language 9 → KO, 10 → TW, 11 → CN, anything
     else → TW; the port reports 1 (English), hence Chinese on the official dump. Table of
     CPK names at `0x140c610` (US FR ES DE IT KO TW CN; the Asia build only reaches 6..8).
   - No Korean text in the Asia release: base 01.00 and the 1.04 update ship only CN/TW text
     (Korean fonts are there). A `Unicorn_KO.CPK` from the Korean release should load
     (untested).
3. **Issue #14** (reporter: resolution and button icons do nothing): their log has all three
   code hooks failing (`Render scale: unavailable (code signature)`, `Button icons: the
   game's decompressor is not where v01.04 has it`, `Options: the game differs`) and maps
   47,530,584 bytes with 89,798 relocations, against 47,071,832 / 90,011 for both of our
   dumps. Their `eboot.bin` is a third build. Not yet asked: its SHA-256 (`certutil -hashfile
   eboot.bin SHA256`; ours `de8e4970ae6ab39278bd61cd7818d7844adf934e1909e1356476a28d239414c4`)
   and how they merged the update. Their screenshots are official Asia (Chinese UI), so after
   #27 they can also pick Japanese or stay on Chinese; English needs the patch.
4. **Setup should hash-check `eboot.bin`** (proposed, not started): `check_game` only reads
   `param.sfo`, so an executable like #14's passes and silently loses the PC features. Reject
   or warn with a clear message, and log the hash.
5. **Windowed mode** (asked, not started): the window is already resizable
   (`gpu/shim/window.cpp`, the swapchain follows), but it opens at 1920x1080, so on a 1080p
   screen the edges are off-screen. Fit the first size to the display's usable area (16:9)
   and remember size and position in vw.ini.
6. Update this list and the PR #24 handoff items (NVIDIA static review, risks 1-4) into
   "Next steps" once #27 is merged.

### What landed on 2026-10-20 (v1.1.0, pull request #22)

| Change | Where | Checked |
|---|---|---|
| AppImage crashed on Wayland (Steam Deck game mode, Plasma): children inherited PyInstaller's `LD_LIBRARY_PATH` and its old Vulkan loader (`vkCreateWaylandSurfaceKHR` null) | `launcher/vw_launcher.py` (`restore_library_path`) | v1.0.0 crashes under gamescope 1280x800; v1.1.0 release AppImage runs there |
| Apply and restart closed the game (exit 23): descriptors closed before `execl` while the GPU thread ran | `src/probe.c` (`runtime_restart_with`, close-on-exec) | restarts in about 1 s |
| 900p then 1080p rendered at 4K: the restart inherited `VW_NEO_MODE`/`VW_INTERNAL_*` | `src/probe.c` (`forget_render_scale_env`) | 1080p to 900p shows 1600x900, back shows 1920x1080 |
| Signature mismatch rendered 900p at 4K | `src/probe.c` (check before the Pro path) | forced mismatch: 1920x1080 buffers |
| Change game folder waited 30 s on Linux (own pid) | `launcher/vw_launcher.py` (`wait_for`) | 30.1 s to 0.0 s |
| `vw.ini`/`launcher.json` emptied by a crash mid-write | `gpu/shim/vw_settings.cpp`, `launcher/vw_launcher.py` | in-game change saved, no `.tmp` left |
| Restarts pushed good save backups out | `scripts/save_backup.py` | failing test first, 113 Python tests |

v1.1.0 is published (tag on `69feb5b`); the Windows zip of it was only checked by CI.

**Driving the game for tests (Linux):** `VW_PAD_FILE=<file>` takes pad tokens
(`cross`, `lx=255`, ...); the Insert overlay only reads real input, so use `ydotool` (start
`ydotoold`; Insert is key 110, the overlay navigates with the arrow keys and Space). On
Windows, an AutoHotkey or PowerShell `SendInput` script does the same. In battle, Cross on the
Command Post does nothing during the tutorial; menus accept Cross.

How the updater decides: `scripts/updates.py` docstring. The running version is
`_internal/runtime/VERSION` (`git describe --match 'v[0-9]*'`); `vX.Y.Z-N-g<hash>` is newer than
vX.Y.Z, a version without a `v` tag (today's builds) is older than every release. `check_updates=0`
in `vw.ini` or `VW_UPDATES=0` turns the check off; "Skip this version" is kept in
`data/launcher.json` (`skipped_update`).

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
3. **Native settings, what is left:** run it on Windows (CI builds it; the hooks use the
   game's System V calling convention, as the button icon hook does); a restart prompt for
   Render Resolution in the game's screen (now it applies at the next start); a Japanese system
   language (the PC texts are English only).
4. **Live button icon switching:** swap the icon texture at bind time (bbport
   `vk_button_prompts.cpp`) instead of relying on the sprite table being re-read.
5. Optional: deband filter; parallel boot decompression; ultrawide (see unicorn/HANDOFF.md).

## Working on it

- Windows build: MSYS2 CLANG64 shell, `bash build.sh` (development) or
  `bash packaging/windows/build.sh unicorn` (the release folder); details and the Linux/Windows
  differences in [../../packaging/windows/README.md](../../packaging/windows/README.md).
- Linux build: `bash build.sh` (unset a cross toolchain's CC/CXX/CMAKE_SYSROOT/LDFLAGS first).
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

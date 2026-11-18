---
title: "Handoff — Unicorn Overlord PC Linux validation"
created: 2026-11-18
source_session: unknown
goal: "Complete Linux validation of the portable launcher, native presentation, and renderer-memory changes."
workspace: "Fresh Linux checkout of hachi23/unicorn_overlord_pc"
---

# Handoff — Unicorn Overlord PC Linux validation

**Next session goal:** Build and test the supplied integration candidate on Linux. Reproduce any failures and report results for the actual GPU and display session.

## Suggested first message

```
Continue from docs/dev/HANDOFF-LINUX-TESTING.md.

Goal for this session: Complete Linux validation of the exact launcher, Fit/Stretch, mouse, and renderer-memory changes.

Read the handoff file in full first. Key refs: handoff/linux-integration and fix/native-renderer-memory. Preserve my existing install and saves.
```

## Current state

| Area | Status |
|---|---|
| COMPLETED | Windows native gameplay on an RX 9070 XT reached battle from a copied save and ran 5m42s before a normal exit. Fit used 1280×720 inside 1280×800 and 1600×720 windows. Stretch used 1280×800 and adjusted to 1400×900 after resize. These runs predate the cached-repaint change. |
| IN PROGRESS | The latest Windows pass had 148 Python tests pass and 31 skip. Three focused native graphics tests passed. A freshly frozen Windows launcher relocated `profiles/data/game` and reported zero missing imports. Cached-frame Fit passed a geometry regression test. Real Linux gameplay and packaging are pending. |
| NEXT | Clone the GitHub branch, initialize submodules, and run the Linux build, suites, and game checks below. |
| BLOCKERS | Gameplay requires an actual Linux Vulkan session and a supported local game dump. #118 Linux CI failed before compilation because the toolchain PPA returned HTTP 503. |

## Artifacts and revisions

- `handoff/linux-integration` at `7eb2d34d354349642c976fd8883f316044db30fd` is the source baseline before the handoff document was added. This source has yet to run on Linux.
- `review/combined-116-118` at `a2490cc` contains launcher hardening and the test-fixture fix `689b350`.
- `fix/presented-image-mouse-geometry` at `1815bc2` contains `7421652` for actual presented coverage and `1815bc2` for Fit during overlay-only repaints.
- `fix/native-renderer-memory` at `d9fbb30` is a separate local branch, outside the integration candidate.
- [PR #116](https://github.com/hachi23/unicorn_overlord_pc/pull/116) at `9978bf4` had green Linux and Windows checks. [PR #118](https://github.com/hachi23/unicorn_overlord_pc/pull/118) at `39821bc` had a green Windows check. Both were open at handoff. [Linux CI attempt 3](https://github.com/hachi23/unicorn_overlord_pc/actions/runs/44327815527/attempts/3) failed in toolchain setup.

## Set up on Linux

Clone into a fresh directory on the Linux PC.

```bash
git clone --branch handoff/linux-integration https://github.com/hachi23/unicorn_overlord_pc.git unicorn-overlord-linux
cd unicorn-overlord-linux
git rev-parse HEAD
git branch native-memory-handoff origin/fix/native-renderer-memory
git submodule update --init --recursive
```

Read `README.md`, `CONTRIBUTING.md`, `shell.nix`, and `.github/workflows/build.yml` for requirements. Use an available Linux toolchain to avoid the failing PPA. Record the distro, GPU, driver, compiler, and Wayland or X11 session.

```bash
bash build.sh
python3 -m unittest discover -s tests
bash build.sh --test
cmake --build out/gpu --target graphics-settings-test gpu-platform-test perf-overlay-test
out/gpu/graphics-settings-test
out/gpu/gpu-platform-test
out/gpu/perf-overlay-test

export VW_GAME_DIR=/absolute/path/to/your/CUSA36381
export VW_DATA_DIR="$(mktemp -d -t uo-linux-test.XXXXXX)"
export VW_RENDERER=native
export VW_MOUSE_DEBUG=1
python3 run.py --check-imports
python3 run.py
```

## Linux acceptance checks

1. **Native gameplay.** Use CUSA36381 app v01.04 or the supported Hong Kong 1.05 dump. Verify zero missing imports, title and menus, a battle, and a clean exit. Use a compatible copied save or New Game. Test Fit and Stretch on a non-16:9 window, resize, and compare the logged mouse view with the displayed image. Try switching Stretch to Fit during an overlay-only repaint while the game supplies no new frame.
2. **Packaged launcher.** Follow the package/AppImage recipe in `packaging/package.py`, `packaging/linux/appimage.sh`, and CI. Launch from an unrelated directory. Test default and nested `VW_DATA_DIR=profiles/data`, relative `--run` paths, migration of a relocated managed copy, and preservation of an external standalone dump named `data/game`. Confirm diagnostics leaves the config untouched. Relative AppImage runtime paths resolve beside the AppImage file.
3. **Renderer memory.** Build the separate branch in another worktree and run `bash build.sh --test`, including `native-memory-test`. Original GTX 1080 logs hit Vulkan image allocation failure after about 68–91 minutes or the 64 MiB vertex limit after about 44 minutes. With matching hardware and workload, run past those durations and capture the scene, GPU, and logs. A successful forced-allocation test does not establish a GTX 1080 hardware pass.
4. **Remaining input risk.** `src/runtime_pad.c` calls SDL mouse/window queries on the guest input thread, though SDL documents them as main-thread-only. This predates the reviewed changes. If Linux shows lost motion, focus issues, or resize jitter, reproduce that symptom before changing the event flow.

## Relevant files

- Launcher: `launcher/vw_launcher.py`, `scripts/portable_paths.py`, `run.py`, `tests/test_launcher_paths.py`.
- Graphics: `gpu/platform/presenter.cpp`, `gpu/image_geometry.h`, `src/unicorn_mouse.h`, `tests/test_graphics_settings.cpp`.
- Memory: `src/native_vk.c`, `tests/test_native_memory.c` on the memory branch.

## Suggested skills

- Matt Pocock’s `diagnosing-bugs` to reproduce a Linux failure before changing code.
- Matt Pocock’s `code-review` and `codebase-design` to review any resulting patch.
- `principle-prove-it-works` to build and exercise the exact revised artifact.

## Policy and data

The branch contains source and the handoff. Supply the game dump and any test save on the Linux machine. Use isolated data directories and redact personal paths from shared logs. Recheck PR heads and follow the repository’s required checks before any future merge.

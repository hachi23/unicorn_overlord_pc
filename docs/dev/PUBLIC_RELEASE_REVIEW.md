# Public release review — 18 October 2026

Reviewed published main at `b6092bef89e853f93ecc6ea8e4e9dee5922845fe`.
This is a source review, not verification of unpublished local fixes or a full playthrough.

## Release judgment

Suitable for an explicitly experimental public test release after the items below are
addressed; evidence does not yet support calling it a stable, fully tested release.
The README only records prologue/chapter 1/first-battle testing. Use a tagged commit and test
the exact downloadable artifacts on clean Windows and Linux systems, including new game,
controller/keyboard input, audio/video, world map, battle, save/load, and settings restart.
Record OS, GPU/driver, CPU, RAM, resolution, settings and how far each tester played.

## Priority findings

1. **Release packaging:** `packaging/package.py` copies the project LICENSE but does not
   assemble third-party license notices. The workflow uploads the Windows executable alone;
   the sidecar LICENSE in the dist folder is not among its published artifacts.
   `packaging/linux/appimage.sh` copies only the project LICENSE. Assemble and inspect the
   notices/source information appropriate to the exact bundled components before release.
   Existing PRs #10 and #11 discuss related packaging/known-issue work; coordinate with them.
2. **Native test coverage in CI:** `.github/workflows/build.yml` builds both platforms and
   runs Python unittest discovery on Linux, but does not invoke `bash build.sh --test` or
   explicitly build/run the C++ tests. A successful build does not validate native behaviour.
3. **Diagnostics completeness:** fix the saved-log omissions and user export workflow below.
4. **Release communication:** distinguish mutable `latest` test builds from versioned tested
   releases; list supported dump/version, known problems and actual tested progress. README
   references an `odin-sphere` branch; open PR #11 already proposes correcting this.
5. **Protection setup:** the repository ruleset list returned empty during this review.
   `.github/main-protection.json` is a template, not proof of active protection. Legacy
   branch protection was not verified. Configure required checks/review rules separately,
   accounting for GitHub's inability to approve your own PR as a solo maintainer.

## Logging: useful foundation, incomplete support workflow

Already implemented:

- `run.py`: timestamped files, elapsed time per line, newest 10 retained, combined native
  stdout/stderr, exit code, port version/OS/Python/GPU and game identification.
- `src/probe.c`: Windows exception details, registers and frame chain; Linux signal/fault
  location and host backtrace. Hang dumps are opt-in via `VW_HANG_DUMP`.
- `launcher/vw_launcher.py`: log viewer and nonzero-exit notification.
- `tests/test_run_log.py`: combined output, retention and final-copy coverage.

Gaps to fix and test:

- `run_script(capture=True)` captures stdout directly and leaves stderr outside `RunLog`.
  Errors from the mods preparation step can appear in the launcher without reaching the
  persistent log. Preserve the machine-readable stdout result while logging stderr safely.
- Unhandled Python exceptions reach the outer console/launcher traceback, but there is no
  exception handler that writes that traceback to `LOG`. A saved log may contain only an
  exit code after a launcher/preparation error.
- `last_run.log` is copied only on close. During a hang or wrapper termination it may still
  refer to a previous run. Show/open the active timestamped log and clearly identify it.
- Filenames have one-second precision and use append mode: rapid restarts can combine runs.
  Add a unique run identifier, and retain each run separately.
- Retention limits file count, not file size. Debug/trace output can grow without a byte cap.
- GPU driver/API identification in `vk_instance.cpp` uses LOG_INFO, while the shim defaults
  to warning. Emit essential GPU/driver/API data unconditionally once per run. Add CPU/RAM
  and effective graphics settings, selected mods/patches and build ID to the support header;
  never dump the full environment.
- The Log window displays at most the last 4,000 captured lines and has no save/export
  control. Add an Export diagnostics action that copies the complete selected run log and
  a small allowlisted settings/system summary to a user-chosen archive. Preview/redact local
  paths. Never include game files, keys, saves, caches or the entire data directory.
- Keep matching debug symbols for published builds so native module offsets can be resolved.
  Validate crash, hang and GPU-device-loss reports using the packaged builds.

Acceptance checks: preparation failure and Python exception are saved; native crash output
and exit code are preserved; active hang log is exportable; rapid restarts remain separate;
export excludes private/game files and contains the correct version, GPU driver and settings.

## Validation performed

`python -m unittest discover -s tests`: 79 discovered, OK, 29 skipped (native binaries absent).
ResourceWarnings also report unclosed subprocess stdout streams in the logging path.
Native build/tests, GPU runtime, packaged applications and game play were not executed here.

## Performance claim

The maintainer reports roughly 50–60 FPS on RX 560X / Ryzen 5 2500U (CPU model tentative).
That is encouraging evidence for that tested configuration, not a guarantee for all modern
PCs or integrated GPUs. State resolution, driver, power mode, settings and tested scenes.
Validate world map, busy battle, menus and longer sessions; report frame-time spikes/1% lows
as well as average FPS, separating cold shader caches from warmed runs. Cross-vendor driver
and Vulkan feature compatibility need testing independently of GPU performance.

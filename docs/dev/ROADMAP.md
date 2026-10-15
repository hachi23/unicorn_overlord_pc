# Roadmap: enhancements taken from uncharted_pc

Plan written 2026-10-15 after comparing this port with
[Supermedo/uncharted_pc](https://github.com/Supermedo/uncharted_pc), which comes from the same
bloodborne_pc lineage. Its runtime, launcher and packaging line up with ours file for file (their
`bbport_*` names are our `vw_*`), so most of what it adds can be ported rather than rewritten.

Each step is one pull request: the Linux and Windows builds check it, and the PR says what was
played (see `CONTRIBUTING.md`). Order: legal first, then small wins, then the larger pieces.

| # | Step | Size | Status |
|---|---|---|---|
| 1 | Third-party licenses in the download | S | Planned |
| 2 | Clear message when the GPU device is lost | XS | Planned |
| 3 | Versioned releases and a launcher update check | S | Planned |
| 4 | Add to Steam, desktop shortcut, `--play` | S | Planned |
| 5 | Controls page and `input.ini` | M | Planned |
| 6 | Launcher translations | M | Planned |
| 7 | README: features, known problems | S | Planned |

## 1. Third-party licenses in the download

**Why.** The package bundles FFmpeg, SDL3, fmt, Zydis, xxHash and zlib (statically linked, see
`packaging/deps.sh`), plus shadPS4, Dear ImGui, sirit, magic_enum, miniz, xbyak, LibAtrac9, VMA,
the FSR 1 / CAS shaders, the DejaVu font and the PyInstaller-bundled Python and Pillow. Most of
their licenses require the notice to travel with the binaries. Today `dist/` holds only our
`LICENSE`.

**How (uncharted_pc: `packaging/windows/package.sh` lines 62-77).**
- `packaging/package.py`: after copying `LICENSE`, create `dist/<Title> PC/licenses/` and copy
  each component's license file into it as `<name>-LICENSE.txt`. Take the files from the
  submodules and `gpu/third_party/` where they exist, and from `out/deps/src/<name>/` for the
  deps.sh libraries. Keep the list in one table in `package.py`, and fail the build if a listed
  file is missing so a dependency bump cannot drop a notice silently.
- AppImage: `packaging/linux/appimage.sh` copies the same folder into the AppDir.
- Add a short `README.txt` to the package: what the files are, where `data/` goes, the
  troubleshooting link.

**Done when** both CI artifacts contain `licenses/` with one file per bundled component, and a
test in `tests/` checks the table against the files.

## 2. Clear message when the GPU device is lost

**Why.** A lost Vulkan device currently ends in `ASSERT_MSG(... eErrorDeviceLost, "Device lost
during submit")` (`gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp`), or in a generic
assertion in `vk_semaphore.cpp`. Players report "it crashed" with no hint that it is the driver
or the GPU.

**How (uncharted_pc: `vk_semaphore.cpp`, `DeviceLost()`).** One `[[noreturn]]` helper that
prints `GPU crash: the graphics device was lost`, flushes stdout (so `run.py` writes it to the
run log), and exits with a distinct code. Call it from the wait in `vk_semaphore.cpp` and from
the submit in `vk_scheduler.cpp`. In `docs/TROUBLESHOOTING.md`, add what to try (update the
driver, lower the render resolution, turn off the overlay) and what to attach to an issue.

## 3. Versioned releases and a launcher update check

**Why.** Players who download a build have no way to learn that a newer one exists.

**Prerequisite.** The update check reads GitHub's `releases/latest`, which skips pre-releases,
and the CI-built `latest` is a pre-release. So first tag a tested commit (`v0.1.0`, see
`docs/RELEASING.md`) and publish it as a normal release. `latest` stays as the bleeding-edge
pre-release.

**How (uncharted_pc: `launcher/uncharted_launcher.py`, `check_update`, `latest_release`,
`offer_update`).**
- `launcher/vw_launcher.py`: on start, a background thread asks
  `https://api.github.com/repos/hachi23/unicorn_overlord_pc/releases/latest` and compares its tag
  with the bundled `VERSION` (CI writes it, `packaging/package.py`). If newer, show a line with
  a link to the release page. Opening the page is enough to start; downloading and replacing
  the files can come later.
- A `check_updates` setting in `launcher.json` (default on). Fail silently when offline.
- Builds from source (`VERSION` missing or `git describe` with `-dirty`) never check.

## 4. Add to Steam, desktop shortcut, `--play`

**Why.** Most Linux players run it on a Steam Deck. Today they have to add the AppImage by hand,
and Steam then shows the launcher window rather than the game.

**How.**
- `--play`: `vw_launcher.py` starts the game straight away with the saved `game_dir` and
  `vw.ini`, without the window, and waits for it. That way Steam tracks the game and the Deck's
  controls apply. If no game folder is saved yet, open the normal window.
- Add to Steam (uncharted_pc: `launcher/bbport_steam.py`, 144 lines, reads and writes Steam's
  binary `shortcuts.vdf`): add the AppImage (Linux) or the `.exe` (Windows) with `--play`, our
  icon and the data folder as the start directory. Steam must be closed while the file is written
  (`steam_running()`). Port the module with its parser and add a round-trip test.
- Desktop shortcut: a `.desktop` file in `~/.local/share/applications` on Linux, and a `.lnk` on
  Windows (PowerShell `WScript.Shell`, as uncharted_pc does).
- Windows: uncharted_pc ships a tiny `play.exe` (`packaging/windows/play.c`) because its main
  exe is a GUI app. With PyInstaller `--onefile`, `"Unicorn Overlord.exe" --play` is enough, so
  skip that.

## 5. Controls page and `input.ini`

**What exists.** `src/runtime_pad.c` already remaps buttons, but only through environment
variables: `VW_PAD_MAP` (controller) and `VW_KEY_MAP` (keyboard), e.g.
`VW_KEY_MAP="cross=Q,square=Left Shift"`. The sticks (WASD and the arrows) and Tab/Backspace (the
touchpad halves) are fixed. Nothing is saved, and the launcher has no page for it.

**How (uncharted_pc: `src/runtime_pad.c` lines 1-200, the `actions[]` table and
`load_input()`).**
- Runtime: read `input.ini` from the data folder (`VW_INPUT_CONFIG`, set by `run.py`) with lines
  of `key.<action>=<SDL key name>[|<second key>]` and `pad.<action>=<SDL button>`. The actions
  are the PS4 buttons plus `ls_up/down/left/right`, `rs_*` and `touchpad_left/right`. Keep
  `VW_KEY_MAP`/`VW_PAD_MAP` working, applied on top of the file. Mouse buttons are optional:
  Unicorn Overlord has no free camera, so skip mouse look.
- Launcher: a Controls panel listing each action with its key and its controller button. Click
  one, then press the new key. "Reset to defaults" restores the layout. The default layout stays
  the one in the README.
- Tests: a C test for the parser (unknown names are ignored, `|` gives two keys, a stick action
  without its opposite direction still works).

## 6. Launcher translations

**Why.** The supported dump is the Asia release, and much of the audience reads Japanese,
Chinese or Korean.

**How (uncharted_pc: `launcher/bbport_i18n.py`, `bbport_lang.py`).** Wrap the launcher's strings
in `_()`. Keep translations in one Python dict per language (no gettext build step, so
PyInstaller needs nothing extra). Pick the language from the OS locale, with an override in
`launcher.json`. Start with Japanese, Simplified Chinese and Korean. The in-game ImGui menu
stays English for now (its font has no CJK glyphs).

## 7. README: features and known problems

Model it on the uncharted_pc README: a short feature list (native x86-64, Vulkan, render
resolution up to 4K, FSR 1 + RCAS, controller and keyboard, Linux AppImage and Windows exe),
then **Known problems**, taken from `docs/dev/unicorn/HANDOFF.md` and kept current: how far the
game has been played, and the open issues. Add a community link (Discord or GitHub Discussions)
once one exists.

## Not taken from uncharted_pc

- **TAA, FSR 3.1/4, DLSS, frame generation.** The temporal stack was removed on purpose in the
  cleanup (`docs/dev/CLEANUP_HANDOFF.md`). Unicorn Overlord is hand-drawn 2D: blending across
  frames smears the sprites, and each game needs its own motion vectors to make it work. The
  render-resolution option (supersampling) already gives clean edges. Frame generation adds
  input lag for no gain in this game.
- **120 FPS and higher.** Vanillaware's game logic counts 60 Hz frames. uncharted_pc's own known
  problems come from its frame-rate work. Not worth the risk.
- **Mouse look.** There is no free camera.
- **Game-specific crash fixes** (e.g. the frame-reuse wait for `m_gfxEopTick`). They belong to
  Naughty Dog's engine.
- **Vulkan ICD selection** (`launcher/bbport_vulkan.py`). It is only needed when the package
  bundles Mesa, which ours does not (the AppImage uses the host's Vulkan loader). Revisit if
  Steam Deck or NVIDIA users report that Vulkan was not found.

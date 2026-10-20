# Troubleshooting

## Where things are

The **data directory** holds settings, saves and logs:

| How you run the port | Data directory |
|---|---|
| Packaged build (`.exe` or `.AppImage`) | `data/` beside the program |
| From the source tree | `run_uo/` in the repository (or `VW_DATA_DIR`) |

Inside it: `vw.ini` (settings), `launcher.json` (the game folder), `game/` (the installed dump),
`user/` (saves and shader caches), `out/` (the prepared game image, rebuilt when needed), `logs/` (one unique log per run,
newest 10 kept), `last_run.log` (live copy of the newest log segment). Each run has at most
two 8 MiB segments (`.log` and `.log.1`) and a small system summary (`.json`). Saves are backed up automatically
before each start.

## Setup refuses the game folder

- The folder must contain `eboot.bin`, `sce_module/` and `sce_sys/` directly.
- It must be **CUSA36381 version 01.04**: the base game with the 1.04 update applied on top
  (update files replace base files). The base game alone is app version 01.00 and is refused. The
  official Asia version and the fan English patch both work (setup asks which one you have).
- To choose another folder later: in-game menu (**Insert**) → Overlay → **Change game folder**,
  or start the program with `--choose-game`.
- Installing needs about 7 GB free in the data directory when the dump is on another drive (on
  the same drive the files are hard-linked and need no space). A cancelled or failed install
  continues where it stopped on the next start.

## The game does not start or closes at once

1. Open `last_run.log` or `logs/` in the data directory (the error dialog offers to open it).
2. Look for a line starting with `STOP:` (something the port does not support yet) or
   `Guest fault` / `Host fault` (a crash, followed by where it happened).
3. Check the GPU line near the top: the port needs a Vulkan 1.3 GPU and an up-to-date driver.
4. `Failed to query surface capabilities: ErrorUnknown`: the graphics driver cannot show any
   Vulkan window at the moment. Seen on Windows with an AMD driver after the PC woke from sleep
   (other Vulkan programs failed too); restarting the PC fixes it.
5. If the game closes right after the `GPU selected:` line, start it once with the environment
   variable `VW_PIPELINE_BINARIES=0` (already the default on NVIDIA) and attach both logs to
   a bug report.

## The log says the graphics device was lost

`GPU crash: the graphics device was lost` (exit code 23) identifies a Vulkan device-loss
failure. This alone does not establish whether the cause is in the port, driver or hardware.
Try a current driver and native render resolution; report the exact scene, graphics settings
and driver version with the failed run's diagnostics.

## Black screen, wrong colours or missing graphics

- Turn the GPU readback options off in the in-game menu (**Insert**, Performance tab) and
  restart.
- Set the render resolution back to native (1080p).
- If an option change stops the game from starting, the next start restores the previous
  options by itself.

## The game freezes

Start it with `VW_HANG_DUMP=5`. Every 5 seconds the log then shows where each game thread is.
Attach that log to the bug report.

## Reporting a bug

When the game stops with an error, answer **Yes** in the error dialog: it shows the newest run's
log with your folders redacted, lets you edit it and saves a ZIP. You can also start the program
with `--export-diagnostics` (for example `"Unicorn Overlord.exe" --export-diagnostics`). Attach it to an issue using the bug report template.
The export includes only that run's log segments and system/settings summary. It excludes
game data, saves, keys and shader caches. Known local paths are redacted; review for other
personal information before sharing. During a hang, run the program a second time with
`--export-diagnostics` to export the active run without closing the game. Very large legacy logs are limited to 16 MiB per exported segment.

You can also attach the timestamped `.log` and `.log.1` manually, after checking for personal
paths. If you restarted the game, select the earlier failed run rather than `last_run.log`. See
[CONTRIBUTING.md](../CONTRIBUTING.md#reporting-bugs) for what helps most.

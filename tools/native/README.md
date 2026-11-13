# Renderer tools

For working on the renderer ([docs/dev/NATIVE_RENDERER.md](../../docs/dev/NATIVE_RENDERER.md)).
Linux, run from anywhere in the repository. The scripts that start the game need
`VW_GAME_DIR` (the game folder) and use `run_uo/` as the data folder; they kill a running game
first (`pkill -x vwport`). Don't pipe their output into another command: the game inherits the
pipe and the command never ends.

## Building and checking

| Tool | What it does |
|---|---|
| `build.sh` | Static build into `out/static`, copied to `out/vwport`. Removes a foreign toolchain from the environment (CC, CXX, CMAKE_SYSROOT, LDFLAGS) first |
| `check.sh [EBOOT]` | Every check that needs no game window: build, C/C++/Python tests, and with an eboot every game shader translated and checked with `spirv-val` (in a temporary folder, deleted afterwards) |
| `ns_dump.c` | The shader translator from the command line. Build: `cc -std=c11 -O2 -Isrc tools/native/ns_dump.c src/native_shader*.c -o out/ns_dump`. `ns_dump EBOOT` lists every instruction of every shader; `ns_dump EBOOT HASH` disassembles one with its header; `ns_dump EBOOT --spirv DIR` translates all (`NS_HEADERS=1`: one line per shader) |

## Running the game

| Tool | What it does |
|---|---|
| `go.sh [seconds]` | Start the game and stop at the title menu (after 42 s of boot by default). `VW_PROBE=<binary>` runs another build |
| `tour.sh MOVE OUT_DIR steps...` | From the title: Load Game, move MOVE rows from the last saved slot, load, then run each step (a `tools/uo_pad.sh` token string) and save a frame after it as `OUT_DIR/NN.png` |
| `parity.sh OUT_DIR SCENARIO [key=value...]` | The same scenario (`title`, `opening`, `options`, `load:MOVE`) with this build and a reference build (`VW_PARITY_REF`, default `out/ref/vwport_shadps4`), frames side by side in `OUT_DIR/side_NN.png`, with the RMSE of each pair. `key=value` are `vw.ini` lines for both runs; environment such as `VW_TEXT_LANGUAGE`, `VW_ASPECT_RATIO`, `VW_LANGUAGES_DIR` passes through |
| `bench.sh [seconds]` | The New Game opening uncapped (`VW_VSYNC=0`), printing the flip thread's `Frame stats` lines. `VW_PROBE=<binary>` measures another build |
| `dump.sh` | While the game runs: the next frame's recorded lists (text) and its display image (PNG) into `run_uo/drive/native/` |
| `../uo_pad.sh` | Drive the running game's pad from scripts (`cross wait800 lx=0:1500 dump`); `dump` saves the presented frame as PNG |
| `../uo_story.sh` | New Game through the prologue into chapter 1, saving frames at checkpoints |

A reference build: check out the commit to compare against (for the last shadPS4 renderer,
`0b09c1a`) in another worktree, run its `tools/native/build.sh`, and copy its `out/vwport` to
`out/ref/vwport_shadps4` here. Kill it with `pkill -x vwport_shadps4`.

Examples:

```sh
bash tools/native/check.sh ~/dumps/CUSA36381/eboot.bin
VW_GAME_DIR=~/dumps/CUSA36381 bash tools/native/parity.sh /tmp/p/title title > /tmp/p.log
VW_GAME_DIR=~/dumps/CUSA36381 VW_ASPECT_RATIO=32:9 bash tools/native/parity.sh /tmp/p/uw load:0 > /tmp/p.log
VW_GAME_DIR=~/dumps/en VW_LANGUAGES_DIR=~/switch VW_TEXT_LANGUAGE=french bash tools/native/parity.sh /tmp/p/fr opening > /tmp/p.log
VW_GAME_DIR=~/dumps/CUSA36381 bash tools/native/bench.sh 60 > /tmp/bench.log
```

## Reading the game

| Tool | What it does |
|---|---|
| `analysis/names.py HASH...` | A shader's reflection names (uniforms, buffers, inputs) from the eboot |
| `analysis/slots.py FRAME_DUMP HASH...` | For draws using a shader in a frame dump: the bound constant and texture slots |
| `analysis/show.py`, `sumfn.py`, `fsum.py` | Filters over frame dumps and Ghidra output (research aids) |
| `analysis/shaders.py`, `fam.py`, `stab.py`, `srefs.py` | The 2026-11-12 survey of the eboot's shader table and shader families (`UO_ELF` = `eboot_ghidra.elf`) |
| `VW_RE_WATCH=<vtable>:<bytes>` | Not a script: run the game with it (Linux) and every 250 ms it prints which 4-byte fields of a live object of that class changed (`src/re_watch.h`). How the battle map cursor's position was found (`src/unicorn_mouse.h`) |
| `ghidra/gh.sh Script.java out.txt [args]` | A Ghidra script, headless, on the project in `UO_GHIDRA_PROJECT` (`GHIDRA_HOME`, default `/opt/ghidra`) |
| `ghidra/*.java` | Call trees, decompilation to C, the Gnmx survey and prologue checks used to find the hooks |

`spirv-cross --version 450 --vulkan-semantics run_uo/user/native_shaders/<hash>.spv` shows a
translated shader as GLSL. `tools/re/map_addresses.py` maps addresses between two builds
(how the HK 1.05 hooks were found).

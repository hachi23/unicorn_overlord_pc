# Native renderer: plan and handoff (2026-11-12)

## Start here (2026-11-13: released as experimental)

The native renderer ships as an opt-in, experimental setting. The owner has stopped working on
it; this section is for whoever picks it up. Everything below it is the history of how it was
built and stays accurate unless this section says otherwise.

**What players get.** Performance menu > **Renderer** > *Native (experimental)* (`renderer=native`
in `vw.ini`, or `VW_NATIVE=own`). shadPS4 stays the default.

**How it works, in four lines.**
1. `src/native_rec.c` hooks 29 entry points of the game's statically linked Gnmx library (v01.04
   only) and records draws, state and resources into per-context lists; with `own` it never runs
   the game's own Gnmx code, so the PS4 command buffers carry only sync, labels and flips.
2. `src/native_vk.c` replays each frame with Vulkan on shadPS4's device: one 544-byte record per
   draw (texture slots, constant-buffer addresses, vertex streams), vertex pulling, bindless
   textures, indirect batching. The presenter samples its display image (`native_present_image`).
3. Shaders: at first start `vwgpu_native_convert` (`gpu/shim/native_convert.cpp`) runs shadPS4's
   recompiler in native mode (`Profile::native_renderer`) over every shader in the player's
   `eboot.bin` into `<user>/native_shaders` (1726 of 1728). Never commit or ship those files.
4. Save thumbnails: the game detiles a render target on the CPU; the hook on 0x20df20 reads the
   native image back instead (`native_target_read`).

**Safety nets.** It turns itself off, and shadPS4 draws, when: the game is not v01.04; the hook
sites do not look as expected; shader conversion fails; the GPU lacks a needed feature
(`features_present`: Vulkan 1.3, Int64, buffer device address, bindless arrays, demote,
subgroup ballot); or Vulkan setup fails. Each case prints a `Native:` line. The launcher's
startup-option revert (`scripts/graphics_options.py`) covers the renderer setting: a start after
**Apply and restart** that never reaches the game goes back to shadPS4.

**Verified (by picture against shadPS4, RX 560X and Vega 8, Linux).** Boot intro, title, the New
Game opening through the chapter 1 title card, the tutorial battle map, menus, save/load with
thumbnails, two player saves from issue #34 (Ravaged Swamp quest, battle map, stage menu), and
every display option (720p + FSR, 4K, 21:9, Anime4K). Performance: RX 560X opening 60 FPS vs
36-54; Vega 8 46-60 vs 38-59.

**Not verified; the work that is left, most useful first.**
1. Later chapters. No saves past chapter 1 were available. Load late saves in both renderers
   (`tools/native/tour.sh`) and compare pictures; watch for `Native:` warnings (`no converted`,
   `not handled`, `skipped`). The two unconverted shaders (e041714c, 546af9ea) may show up there.
2. Windows. The code is portable (MinGW, UTF-8 manifest, `sysv_abi` hooks) and builds in CI, but
   it has not been run on Windows.
3. NVIDIA and Intel GPUs, and the Steam Deck.
4. Depth testing and culling are not applied (nothing seen so far needed them); the two compute
   shaders other than the fill are not handled; render targets are assumed RGBA8.
5. CPU reads of rendered memory other than the thumbnail path return nothing.
6. Long sessions: target, texture and pipeline counts are not bounded yet; watch memory over hours.
7. Recording still runs on the game thread (~0.6 µs per draw).

**Tools.** `tools/native/build.sh` (static build), `go.sh MODE` (start to the title),
`tour.sh MODE MOVE OUT_DIR steps...` (load a save, capture after each pad step), `bench.sh`,
`check.sh` (picture grids). Section 5 has the rest. The scripts take the game folder from
`VW_GAME_DIR`; the analysis scripts read the linked image from `UO_ELF` (`eboot_ghidra.elf`,
from `scripts/`'s preparation) and `ghidra/gh.sh` needs `UO_GHIDRA_PROJECT` (and `GHIDRA_HOME`).

Section 1's goal of hand-written GLSL and removing shadPS4 was dropped: shaders are converted
with shadPS4's recompiler at first start, and shadPS4 remains as the default and fallback.

---

Read the rest if you are continuing the native renderer work. It records the goal, what was
found in the game's executable, what is built, what is and is not verified, and the steps left.

- Worktree: `~/unicorn_overlord_pc_native`, branch `claude/native-renderer` (from `unicorn/main`
  at `3cfcc33`). **Nothing is committed yet**; the owner has not asked for a commit.
- Game: Unicorn Overlord CUSA36381 **v01.04** only. All addresses below are v01.04 image
  offsets. The Hong Kong 1.05 build is not mapped (`unicorn_address()` has no ranges for this
  code), so the feature turns itself off there.
- Ghidra: project `~/uo_re2/ghidra-project` (program `eboot_ghidra.elf`), driven headless with
  `tools/native/ghidra/gh.sh`. The Ghidra MCP instance holds another project; do not switch it.

## 1. Goal and the owner's decisions

The owner wants a **genuine native PC port**: no shadPS4 emulation code left, except possibly
the OS layer. Concretely:

1. No GPU command-buffer emulation, no texture/buffer cache keyed on guest memory, no shader
   recompiler. `gpu/shadps4` (about 100,000 lines) goes away.
2. The game's graphics are drawn by our own Vulkan renderer with **hand-written GLSL** that
   recreates each game shader.
3. The x86-64 eboot keeps running natively, and `src/runtime_*.c` (our own OS layer) stays.

4. **The native port must run faster than the current shadPS4 build** (owner, 2026-11-12).
   Baseline to beat, measured with `VW_FRAME_STATS=1 VW_VSYNC=0` on the laptop (RX 560X):
   New Game opening, ~4,100-4,400 draws/frame, **51-52 FPS**, CPU-bound in the translator
   (stage A ~3.9 µs/draw, stage B ~77% busy). Light scene (Palevia battle map, ~1,100
   draws) at 60. The native renderer has no PM4 decode, no guest-memory caches and no shader
   recompile, so the target is the opening **uncapped above 60 FPS** with recording + replay
   costing **under 1 µs/draw** on the CPU. See section 6a for the design rules this implies.

Reference project the owner pointed at: `junminlee2004/GR2port` (Gravity Rush 2). It cuts in
at the scene level and rewrites the frame; we cut in lower (see section 3) because this game is
a 2D sprite engine whose draws are already simple.

## 2. What the executable looks like (Ghidra findings)

Layers, top to bottom:

| Layer | Where | Notes |
|---|---|---|
| Render passes | `0x3220`, `0x3390`, `0x3564` (sprites), `0x470c0`, `0x232140`-`0x252090` (post) | reached through display-list packets run by a `RenderDLThread` |
| Engine device layer | `0x22d00`-`0x4df80`, about 45 functions | class `FnPS4_Device`; device = `FUN_00000700()->+0x40` |
| **Gnmx (Sony's library, statically linked)** | `0x214000`-`0x21d000` | **this is where we hook** |
| `sceGnm*` imports | 97 imported, 23 called | one draw call, `sceGnmDrawIndex` |

Gnmx entry points identified (first argument is the graphics context unless noted; "LCUE"
functions take `context + 0x1c8`):

| Address | Meaning |
|---|---|
| `0x2146d0` | render target write mask |
| `0x214730` | blend control (slot, bits) |
| `0x2147a0` | colour buffer control (mode, raster op) |
| `0x214810` | depth-stencil control |
| `0x214870` | depth clear value (context, float) |
| `0x215a50` | DB render control |
| `0x214ac0` | depth render target |
| `0x218ae0` | colour render target (slot, target*) |
| `0x214de0` / `0x2150b0` | primitive type / index size |
| `0x214e50` | drawIndex(count, indices) |
| `0x214ed0` | dispatchDirect |
| `0x215960` | screen scissor (l, t, r, b) |
| `0x215ab0` / `0x215b10` | clip control / primitive setup |
| `0x21c1f0` | setupScreenViewport(l, t, r, b, zscale, zoffset); also sets the scissor |
| `0x218bf0` | LCUE setTextures(stage, slot, T#) |
| `0x218df0` | LCUE setBuffers(stage, slot, V#) (read-only buffers) |
| `0x218fe0` | LCUE setSamplers(stage, slot, S#) |
| `0x2191b0` | LCUE setConstantBuffers(stage, slot, V#) |
| `0x2193a0` | LCUE setRwBuffers |
| `0x219580` | LCUE setVertexBuffers(stage, first slot, count, V#) |
| `0x2199b0` / `0x219b50` / `0x219d00` | LCUE set VS / PS / CS |
| `0x21be90` | submit(context) |
| `0x21c050` | submitAndFlip(workload, handle, buffer, mode, arg, context) |
| `0x21be50` | reset context (not hooked; the game's own) |
| `0x215560` | allocate from the command buffer (not hooked; the game uses the memory) |
| `0x215b70` / `0x215c50` | dmaData / fill (memory copies by the GPU) — **not hooked yet** |
| `0x215d40`, `0x215f60`, `0x215df0`, `0x215410` | wait-on-address, end-of-pipe label write, wait for graphics writes — **not hooked yet** |
| engine `0x3ede0` | fills memory with a compute shader: how the game clears targets (hooked) |
| engine `0x3ebc0` | clear a render target (calls `0x3ede0` or draws a quad) |
| engine `0x44690` / `0x44910` / `0x44a90` | texture from GNF data / empty writable texture / render target object |

Shader stages are numbered 0 CS, 1 PS, 2 VS. Every hooked function starts with
`push rbp; mov rbp, rsp; push <reg>` (5 or 6 bytes), so one generic trampoline serves all.

Facts about the data:

- **Shaders**: 1,743 in the eboot (620 VS, 1,120 PS, 3 CS), none in the CPK archives. Each is in
  a container starting 0x24 bytes before its `Shdr` header; a table of 24-byte
  `{pointer, size, 0}` entries at `0x1403080`-`0x1600c40` points at them. PSSL reflection names
  follow each blob (32 constant buffer names, 152 `g_*` uniforms, vertex semantics).
  Grouped by name signature: 358 VS and 447 PS families, mostly feature permutations of one
  sprite shader (fog, gradient map, reflection, multi-texture, mask, dissolve) plus about 15
  effects (aurora, mist, cloud, rain, snow, god rays, glare, heat shimmer, blur, post).
- **Shader identity at run time**: the Gnmx shader object's word 2 `<< 8` is the code address;
  the footer `OrbShdr` is at `(code[1] + 1) * 8` bytes; the hash is the u32 at footer + 0x10.
  The old shader cache files are named by the same hash.
- **Constants**: the engine binds the same data to VS and PS at a slot. Slot 0 is the matrix
  and per-pass data, slot 1 a material colour.
- **Vertex inputs**: every input has its own buffer slot (address + stride), even when several
  share one interleaved stream. Plain sprites use slots 0, 1, 2 (position, colour, uv); the
  two-key-frame sprite uses 2, 3, 4, 5 and uv on 8; the four-key-frame sprite 0..7 and uv on 8.
- **Textures**: BC7 and BC4 (fonts) in tile mode 13; render targets are RGBA8 in tile mode 14;
  the display buffer has tile mode 10. No mip levels seen.
- **Title screen frame**: 5 lists (4 display-list contexts, then the main context, which
  flips), 239 draws, 7 VS + 5 PS + 1 CS, about 150 render targets (many 256² and 512²),
  primitive types 5 (fan) and 6 (strip), three blend values, two samplers.

## 3. Architecture chosen

Hook the Gnmx entry points and keep everything above them untouched. The engine's init, frame
submit, allocators and display lists run as shipped; only the library that used to write
console command buffers is replaced.

```
game render passes -> engine device layer -> [hook] Gnmx entry
                                               |-> recorder: one command list per context
                                               '-> (transition only) the game's own Gnmx code
submit / submitAndFlip -> lists join a frame -> native Vulkan renderer replays the frame
```

Two modes while shadPS4 is still present, selected by `VW_NATIVE`:

- `trace`: record only, beside the existing renderer (statistics and frame dumps).
- `1`: record, and draw each frame with the native renderer **off screen**, beside the
  existing renderer, so the two images can be compared.

The end state is a third mode where the game's Gnmx code is no longer called, the native
renderer owns the window, and `gpu/shadps4` is deleted.

## 4. What exists in the worktree

| File | What it is | State |
|---|---|---|
| `src/native_gfx.h` | modes, command list format, recorder/renderer interface | done |
| `src/native_gnm.h` | decoders for T#, V#, render target, shader header, **fetch shader** | done; all confirmed by pictures |
| `src/native_rec.c` | hooks for 28 entries, per-context lists, frame assembly, text dump; reads each VS's fetch shader and records only the streams it reads, in input order | works |
| `src/native_vk.c` | Vulkan 1.3 renderer (see "Renderer design" below), PNG writer, statistics | **title, logos, intro and the castle garden match the reference** |
| `gpu/shim/native_convert.cpp` | `vwgpu_native_convert`: every VS/PS in the eboot through the recompiler's native mode (see "Shaders: converted on the player's machine") | 1726 of 1728 |
| `tools/native/aot/shader_aot.cpp` | `shader-aot [--native] EBOOT OUT_DIR`, the converter from the command line (`ninja -C out/static shader-aot`) | works |
| `tools/native/check.sh [n] [s]` | **the standard check**: builds, restarts, captures the title, starts New Game, captures n pairs every s seconds, writes the grid, prints new log lines | works |
| `tools/native/{build,go,dump}.sh`, `pairs.py` | build; restart to the title menu; capture native + reference at the same moment; reference/native grid | work |
| `tools/native/analysis/names.py HASH…` | a game shader's reflection names (uniforms, blocks, inputs) from the eboot | works |
| `tools/native/analysis/ref.py HASH` | the old recompiler's GLSL for a shader, constants as `cN[i]` | works |
| `tools/native/analysis/sig.py HASH…` | pixel shader feature signature (textures, overlay, x2, gradient lookups) | works |
| `tools/native/analysis/slots.py DUMP HASH…` | constant/texture slots bound at that shader's draws | works |

### Renderer design (as built)

- Two frames in flight (fences), one command buffer each; no queue wait per frame.
- Per frame, persistently mapped rings: vertex data, 32-bit indices (bound once per frame),
  constants, **draw records** and indirect commands.
- **One record per draw** (`Record` in `native_vk.c`, `Info::NativeLayout` in the recompiler,
  544 bytes): per texture slot the bindless texture and sampler index; per stage and game
  constant slot the block's GPU address; per vertex input the stream's address, stride and format. The draw's `firstInstance` is the
  record's index; the only push constant is the records' address (set once per frame).
- **Vertex pulling**: the converted vertex shaders fetch their own inputs in their prologue.
  Pipelines have no vertex input state; the pipeline key is shaders, blend, mask, topology.
  Compiled pipelines are kept in `<user>/native_shaders/pipelines.bin`.
- **Batched draws**: each draw writes a `VkDrawIndexedIndirectCommand`; consecutive draws with
  the same pipeline, viewport, scissor and target go out as one `vkCmdDrawIndexedIndirect`.
- Hash maps for targets, textures and pipelines (by address / key), the last pipeline kept.
- Images stay in `GENERAL`; one global barrier after every pass, clear and upload.
- **Texture content check**: the game reuses memory for new content. Each frame, before
  replay, textures are re-sampled round robin (64 bytes per 16 KiB, 64 MiB of texture per
  frame) and a changed one is uploaded again into the same image.
- Prefers the first discrete GPU (`VW_NATIVE_GPU=<index>` overrides).
- Fans and quad lists (0x13) become triangle lists; rect lists (0x11) are not drawn yet.
- When a frame logs something new (unknown shader, unbound constant slot, unhandled primitive),
  it is dumped automatically as `run_uo/drive/native/report_<frame>.txt`.

Debugging switches: `VW_NATIVE_WHITE=<addr>` (that texture reads white),
`VW_NATIVE_WATCH=<addr>` (log draws into that target), `VW_NATIVE_SHOT_TARGET=<addr>` with
`VW_NATIVE_SHOT_ALPHA=1` (write a target's alpha), `VW_NATIVE_PEEK=<addr>` (is game memory
there non-zero).

### Facts learned while matching pictures

- Tile mode 13 (Thin_1dThin) stores each 8x8 micro tile in **Morton order** (x0 y0 x1 y1 x2 y2)
  for every element size; micro tiles are in rows of `pitch / 8`.
- The vertex input → buffer slot mapping is **per draw**, not per shader: the engine pairs the
  same VS with different fetch shaders. The fetch shader is the 4th argument of the VS hook;
  `gnm_fetch()` decodes it (`s_load_dwordx4` offset / 4 = slot, `buffer_load_format` vdata =
  input order).
- The final image is a quad list (0x13) into the tile-mode-10 display buffer.
- Constant slots seen: 0 common (matrix; floats 16.. material colour or g_Rate, 20..23 g_K,
  28.. uv scale), 1 material colour, 2 fog colour (PS) or multi XYWH (some VS), 3/4 godray and
  glare XYWH or multi XYWH, 6 shading colours (4 corners + pos/min/rev at floats 32..35),
  8 gradient map (x amount, y row), 9 gradient map parts, 13 unknown 48-byte block.
- Texture slots: 0 main, 2 multi texture, 4 gradient map (512x2048, one palette per row) or
  godray, 5 glare.
- Pixel input locations in our GLSL follow the **semantic**, not the game's input index:
  0 colour, 1 uv, 2 multi uv, then shading. Shaders that skip one still line up.
- Feature order in the sprite pixel shaders: texture x colour, fill (shading rgb replaces),
  parts gradient, multi (x2 multiply, or mix by alpha), shading overlay (by its alpha), gradient
  map, fog.

### Status (2026-11-12, session 3)

- The New Game opening matches the reference in every captured pair: title, logos, intro, the
  cavalry street (field "Curve"/"Post" shaders, rain, godray/glare composite with Post grade),
  the castle, the garden, the location emblem (drawn once into a 256x256 target, read by
  `bc96aaf8` with `MULTI_RECT`).
- Replay costs ~0.5-0.8 µs per draw (2.3-3.0 ms for ~4,000 draws); recording adds ~0.6 µs per
  draw on the game thread.
- **`VW_NATIVE=own` works** (Step 4, first part): the hooks record and no longer run the
  game's graphics code; shadPS4 only processes what is left (synchronisation, labels, flips).
  The native renderer shares the presenter's `VkDevice` and queue (`vwgpu_vulkan_handles`,
  submits under `Scheduler::submit_mutex`), copies its display image at each flip into one
  of four present images, and `Presenter::PrepareFrame` samples that image
  (`native_present_image`, by display-buffer address) instead of its own. FSR, Anime4K, the
  overlay and the settings menu keep working, since they run after that point.
- **Result** (`tools/native/bench.sh`, RX 560X laptop, New Game opening, 60 s, vsync off,
  60 Hz vblank): shadPS4 renderer 36-55 FPS (most windows 51-54); native owning the frame
  **60.0 FPS** in every window after loading. The game limits itself to 60: with
  `VW_VBLANK_HZ=240` both show 60, so headroom has to be read from frame costs, not FPS.
- **Shaders**: hand-written and hand-converted shaders (programs.txt, ref2native.py, match.py)
  are retired; every shader is converted at start, see "Shaders: converted on the player's
  machine" below.
- Not yet checked in own mode: battles beyond the tutorial map,
  the world map, text entry. CPU reads of GPU-rendered memory other than through
  detileSurfaceRegion (the save thumbnail path) still see nothing.

## 5. Working loop

```sh
cd ~/unicorn_overlord_pc_native
bash tools/native/check.sh 5 5           # build, run, grid in run_uo/drive/native/pairs.png
NATIVE_MODE=own bash tools/native/check.sh 5 5   # the same with the native renderer owning the frame
bash tools/native/bench.sh own 60        # FPS of the opening (0: shadPS4 alone, 1: both)
python3 -I tools/native/analysis/names.py <hash>     # what the shader is
python3 -I tools/native/analysis/sig.py <hash>       # pixel shader features
python3 -I tools/native/analysis/ref.py <hash>       # the maths, if the above is not enough
python3 -I tools/native/analysis/slots.py run_uo/drive/native/report_N.txt <hash>   # its slots
rm -rf run_uo/user/native_shaders        # or bump CONVERTER_VERSION: reconvert after emitter changes
pkill -x vwport                          # never pkill -f
```

- Do not pipe `go.sh` or `check.sh` into another command: the game inherits the pipe and the
  command never ends. Redirect to a file or `/dev/null`.
- `VW_NATIVE_SHOT_TARGET=<hex address>` writes one render target instead of the display image.
- Reference GLSL comes from the old path's shader cache (`run_uo/user/cache/CUSA36381`), a
  development aid only.
- Build environment: the shell exports a foreign clang toolchain; `tools/native/build.sh`
  removes it. Use `/usr/bin/python3` for analysis scripts.

## 6a. Performance rules (apply from step 1, measure from step 2)

The point of going native is to remove work, so the renderer must not grow the costs the
emulator had. Keep these from the start rather than optimising later:

- **Record cheaply.** Hooks append fixed-size records to a per-context array; no allocation,
  hashing or Vulkan calls on the game's thread per draw.
- **No GPU wait per frame.** Two or three frames in flight with fences; never `vkQueueWaitIdle`
  or `vkDeviceWaitIdle` outside resize/shutdown.
- **One upload ring per frame**: a persistently mapped host-visible buffer for vertex, index
  and constant data, sub-allocated linearly; constants bound by dynamic offset, not one buffer
  or descriptor set per draw.
- **Descriptors**: one bindless texture/sampler array (descriptor indexing) indexed by a push
  constant, or push descriptors; no `vkAllocateDescriptorSets` per draw.
- **Pipelines**: cached by a small packed key (shader pair, blend, primitive, target format,
  depth state); dynamic state for viewport, scissor, cull, depth test/write
  (`VK_EXT_extended_dynamic_state`), so the cache stays small. Build them at load or on a worker,
  never stall a frame on compilation when avoidable.
- **Merge consecutive draws** with the same pipeline and textures where the game issues many
  small quads (sprites); this is the main win over a PM4 translator, which cannot see intent.
- **Render passes**: one dynamic-rendering scope per target run, not per draw; skip
  load/store of targets that are fully cleared or never read.
- **Replay on a render thread** (step 4) so the game thread only records.
- **Measure** with a `Native: frame` statistics line (record µs, replay µs, draws, merged
  draws, pipelines, upload KiB) and compare against the baseline above at each step.

## 6. Steps, in order

Each step ends with a check. Do not move on while a check fails.

### Step 1. Title screen matches (done 2026-11-12)

1. Run `go.sh 1`, `dump.sh`, and read the native PNG next to the reference PNG.
2. Expect problems in this order, and fix them one at a time:
   - **Nothing or black**: check `Native:` lines in `run_uo/drive/game.log`; dump single targets
     with `VW_NATIVE_SHOT_TARGET` following the target order in the frame dump.
   - **Scrambled textures**: the untile routine (`untile` in `native_vk.c`) assumes tile mode 13
     is 8×8-element tiles in rows; for BC formats an element is a 4×4 block. Confirm against
     one texture.
   - **Upside down or mirrored**: viewport y direction (`draw` uses a negative height).
   - **Wrong colours**: channel order of RGBA8 targets, the BC4 swizzle (`4001`), blend bits.
   - **Missing sprites**: slot mapping of `sprite_lerp.vert` (2, 3, 4, 5, 8 is inferred from
     the engine code, not yet confirmed), fan-to-list conversion, skipped draws.
3. Depth is ignored so far: draws with a zero colour mask are skipped and no depth test runs.
   If the title looks right without it, leave depth for step 3.

**Check**: the native title image matches the reference by eye, and `skipped` is 0 except for
the depth-clear draw.

### Step 2. Cover the game's shaders (done 2026-11-12)

Replaced by conversion at start (the recompiler's native mode). A shader missing at run time
prints `no converted … shader <hash>` once.

### Step 3. Complete the state

- Depth: depth targets (`NC_DEPTH_TARGET`, format from the z-info word), depth test and write
  from `NC_DEPTH_CONTROL`, the depth clear draw, `NC_DEPTH_CLEAR`.
- Primitive setup (culling), clip control, colour buffer control modes other than normal,
  second texture slot users, compute dispatches other than the fill.
- Texture lifetime: textures are cached by address and never invalidated. Hook the engine's
  texture creation and release (`0x44690`, `0x44910` and the frees they call) to drop cache
  entries; find how dynamic textures (font cache, movie frames `g_TextureY`/`g_TextureV`) are
  updated and re-upload on that event.
- Render target release and resize (the heap is reused when the render size changes).

**Check**: movies, text entry, a save/load round trip and a render-resolution change all look
right.

### Step 4. Own the frame

1. Add a mode where the hooks **do not call the game's Gnmx code**. Keep calling
   `0x215560` (allocation) and `0x21be50` (reset) as they are.
2. Hook the GPU sync entries (`0x215d40`, `0x215f60`, `0x215df0`, `0x215410`, `0x215b70`,
   `0x215c50`) and perform their memory effects ourselves when a list is submitted: the engine
   waits on labels the GPU would have written.
3. Replace `sceGnmSubmit*`, `sceGnmSubmitDone`, flip and vblank events, and the VideoOut calls
   with small native versions (`src/runtime_*.c` style). The engine registers its display
   buffers through `sceVideoOutRegisterBuffers` in `0x3cb60`.
4. Give the native renderer the window: SDL3 surface, swapchain, present on flip, vsync and
   frame pacing, then move the overlay and the settings menu (`gpu/shim/vw_overlay.cpp`) onto
   it.
5. Move rendering off the game's thread (a render thread that takes whole frames) and replace
   the per-draw descriptor sets and `vkQueueWaitIdle` with something that scales.

**Check**: the game runs with `gpu/shadps4` not initialised at all, and the New Game opening
runs uncapped (`VW_VSYNC=0`) **faster than the 51-52 FPS baseline** on the RX 560X, ideally
well above 60. The light scene must also be faster than the shadPS4 build's uncapped figure
(measure that baseline on `unicorn/main` first and record it here).

### Step 5. Remove shadPS4

Delete `gpu/shadps4`, `gpu/third_party/{sirit,gcn,xbyak,…}` that only it used, the pipeline
cache and shader cache code, and the settings that no longer mean anything (async shaders,
readback options). Update `README.md`, `packaging/notices.py` and the licence notes. Port the
remaining features that lived in the old renderer: render scale, ultrawide, FSR1/RCAS,
Anime4K, frame dumps used by the drive scripts, GPU profiling.

**Check**: Linux and Windows CI build and package without `gpu/shadps4`; a full prologue run
matches the last reference images.

## 7. Risks and open questions

- **Shader coverage**: normal play had used only 137 of the 1,731 distinct shaders. Shaders
  nobody has triggered cannot be compared against a reference picture; they need a careful read
  of their code or a scene that uses them before step 5.
- **Reading the originals** needs a GCN disassembly. Today that comes from the old recompiler's
  output. The owner should decide whether keeping that as an offline development tool is
  acceptable once the code is gone from the port.
- **Hong Kong 1.05 and Windows**: untested. Windows needs nothing new for the hooks (they are
  `sysv_abi` functions like the existing ones), but the renderer has only been built on Linux.
- **Performance is unmeasured.** Recording costs about 0.02 ms per frame on the title screen;
  the renderer currently waits for the GPU every frame and is not representative.
- **Per-draw vertex copies** are sized from the highest index. Large batches have not been
  seen yet; check the `KiB copied` statistic in battles.
- **Texture memory**: BC7 4096² textures are uploaded whole (16 MiB each) and never freed.

## 8. Rules for whoever continues

- Verify by picture, not by "it runs". Every claim in a status report needs the image pair or
  the log line behind it.
- One change, one comparison. The frame dump tells you which draw produced which target.
- Do not commit or push without the owner asking; `main` is protected and work goes through
  pull requests. Commit with the noreply author address noted in the project memory.
- Kill the game with `pkill -x vwport`. Never press Cross during narration in battle scenes.

## Shaders: converted on the player's machine (2026-11-12)

No game-derived shader ships. At start, `native_render_init` → `shaders_prepare` converts every
vertex and pixel shader in the dump's `eboot.bin` into `<user>/native_shaders/<hash>.spv` with
`vwgpu_native_convert` (gpu/shim/native_convert.cpp, about 2 s, 1726 of 1728; e041714c and
546af9ea have no container found). A `stamp` file (converter version, eboot size and mtime) skips
it on later starts; bump `CONVERTER_VERSION` in native_vk.c when the emitter output changes.
`VW_NATIVE_SHADERS=<dir>` uses a folder as it is. `shader-aot [--native] EBOOT OUT_DIR` runs the
same converter from the command line (without --native: the old bindings, to compare with the
runtime cache; identical apart from the SPIR-V version, 1.6).

The recompiler's native mode (`Profile::native_renderer`, `Info::NativeLayout`):
- record index: VS `gl_InstanceIndex` (firstInstance), passed flat at location 15; made
  wave-uniform with subgroupBroadcastFirst (a wave never mixes draws), so record, constant and
  descriptor accesses are scalar;
- constant buffers: physical storage buffer pointers from the record, by game slot
  (`block[stage][slot]`), aligned loads; the unused clip-planes buffer is dropped;
- textures: `texture2D[]` / `sampler[]` (set 0, bindings 0/1) indexed by the record's slot word;
- vertex inputs: pulled from `stream[n]` in the prologue (formats 1-4 floats, 5 unorm8x4);
- outputs at semantic - 15 on both sides.
Survey of all 1726: no push-constant reads, only sampled 2D float images, no buffer writes.
All pass `spirv-val --target-env vulkan1.3`.

The record is 544 bytes: `slot[8]` @0, `block[2][16]` @32, `stream[16]` @288 (Record in
native_vk.c, `Info::NativeLayout`). The retired hand-shader files were moved out of the tree.

Verified: title menu and the opening rain scene match the reference by picture. 2026-11-13
`tools/uo_story.sh` in own mode on converted shaders: prologue to the chapter 1 title card, no
missing shader or warning, 59.2 FPS average (shadPS4 alone 54.5), lowest 5 s window 38.2; all
checkpoints match the reference apart from timing.

Save thumbnails (2026-11-13): the game makes them on the CPU, `FnThumbCapture` (0x357cc →
0x34dc0) detiles a 280x180 render target with the statically linked
`GpuAddress::detileSurfaceRegion` (0x20df20) and JPEG-encodes it. Own mode hooks 0x20df20: a
source that is a native target is read back from its image (`native_target_read`, once per
target and frame, in the target's comp_swap byte order); anything else runs the game's code.
Thumbnails now show in own mode; in shadPS4 mode they are black (its deferred/stale readback
options), also before this work. `VW_NATIVE_LOG_TARGETS=1` logs each new render target.

Intro and boot sequence (crest, painted scenes, title) render in own mode; no video decoder use
seen there, so movies are not a separate problem so far.

Vega 8 (`VW_GPU_ID=0`, 2026-11-13, opening, vsync off): native 46-52 FPS in the first 25 s, then
59-60 in the rain scene; shadPS4 38-46, then 51-59. Pictures match.
Open: on 2026-11-12 from about 23:00 every mode (shadPS4 alone too) ran at 19.7 FPS with the dGPU
100% busy even at the title, PCIe link stuck at 2.5 GT/s x8; at 22:31 the same build of mode 0 ran
60. Environmental; benchmarks are meaningless until that is fixed (reboot or force the PCIe DPM
level as root).

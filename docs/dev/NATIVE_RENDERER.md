# The renderer (experimental since v1.6.0)

Read this before any graphics work. It explains how the port draws, where each part lives, how
to change and check it, and what is left. The tools are listed in
[tools/native/README.md](../../tools/native/README.md).

Up to v1.5.2 the port drew through shadPS4's video core: it decoded the PS4 command buffers the
game built and recompiled its shaders. Since v1.6.0 that code is gone (about 246,000 lines). The
port hooks the game's own graphics library, records what the game asks for, and draws it with
Vulkan. There is no GPU emulation left. The renderer ships marked **experimental** (README,
release notes, the menu's About section, the `Native: renderer (experimental)` log line).

## 1. How a frame is drawn

```
game render passes -> engine device layer -> Gnmx entry (hooked, src/native_rec.c)
                                               '-> recorder: one command list per graphics context
Gnmx submit / submitAndFlip (hooked) -> the lists join a frame -> src/native_vk.c replays it
                                                                     '-> display image -> presenter
sceGnmSubmit* (gpu/platform/gnm.cpp) -> command thread: labels, end-of-pipe events, flip marker
sceVideoOut* (gpu/platform/video_out.cpp) -> flip thread: vblank, flip events, presenter
```

1. **Start.** `src/probe.c` checks the build (`unicorn_known_build`: v01.04 or the Hong Kong 1.05
   package), then `native_gfx_install` writes a 5-byte jump at each of 29 entry points of the
   game's statically linked Gnmx library. Addresses are v01.04 image offsets, moved for other
   builds by `unicorn_address()` (`src/unicorn_build.h`). Each hook checks the function's
   prologue (`push rbp; mov rbp, rsp; push reg`) before patching; a mismatch stops the start
   with a `Native:` line naming the entry.
2. **Shaders.** `native_render_init` translates every vertex and pixel shader in the player's
   `eboot.bin` into `<user>/native_shaders/<hash>.spv` (about 1.5 s for 1726 shaders) with
   `ns_convert_all`. A `stamp` file (translator version, eboot size and time) skips it on
   later starts. Converted shaders are game data: **never commit or ship them.**
3. **Recording** (`src/native_rec.c`, game thread). The hooks append fixed-size commands to the
   calling context's list: state (blend, mask, viewport, scissor, primitive type), resources
   (T#, V#, S# descriptors, constant buffers, vertex buffers; LCUE calls take
   `context + 0x1c8`), shaders (the hash from the shader's footer), draws and dispatches. They
   do **not** run the game's Gnmx code, so the PS4 command buffers the game submits carry only
   synchronisation (labels, end-of-pipe events) and the flip marker.
4. **Replay** (`src/native_vk.c`). At submit-and-flip the frame's lists (usually 5: four display
   lists and the main context, which flips) are replayed: one 544-byte **record** per draw
   (texture slots, constant buffer addresses, vertex streams), vertex pulling in the shaders,
   bindless textures, consecutive draws with the same pipeline and target batched into one
   `vkCmdDrawIndexedIndirect`. Two frames in flight. The display image is copied into one of
   four images the presenter samples.
5. **Command buffers** (`gpu/platform/gnm.cpp`). `sceGnmSubmit*` copies the submitted buffers
   and a command thread walks their PM4 packets for what the game waits on: `WRITE_DATA`,
   `EVENT_WRITE_EOP`/`RELEASE_MEM` (label writes, end-of-pipe interrupts into the event queues),
   `DMA_DATA` memory fills, `INDIRECT_BUFFER` (nested at most 4 deep), and the flip marker
   64 dwords before the end of a submit-and-flip buffer. Everything else is skipped:
   drawing already happened through the hooks.
6. **Flips and present** (`gpu/platform/video_out.cpp`, `presenter.cpp`). The flip thread paces
   flips to the vblank, signals VideoOut events, and the presenter scales the image into the
   window (FSR 1, CAS sharpening, Anime4K, supersampling) and draws the ImGui menu on top.
7. **Save thumbnails.** The game detiles a render target on the CPU
   (`GpuAddress::detileSurfaceRegion`, 0x20df20) to make the JPEG. That hook reads the native
   image back instead (`native_target_read`).

## 2. Code map

| File | What it is |
|---|---|
| `src/native_rec.c` | The 29 hooks, per-context lists, frame assembly, frame text dumps |
| `src/native_gfx.h` | The command list format and the recorder/renderer interface |
| `src/native_gnm.h` | Decoders: T# (texture), V# (buffer), render target, shader header, fetch shader |
| `src/native_vk.c` | Vulkan replay: targets, textures (detiling), pipelines, records, batching, readback |
| `src/native_shader.{h,c}` | Shader containers in the eboot (`ns_find_shaders`), GCN decoder, disassembler |
| `src/native_shader_spirv.c` | GCN → SPIR-V (`ns_translate`, `ns_convert_all`) |
| `src/unicorn_build.h` | Known builds and the v01.04 → HK 1.05 address map |
| `gpu/platform/vulkan.cpp` | Instance, GPU choice (`VW_GPU_ID`, else the first discrete), device |
| `gpu/platform/presenter.cpp` | Swapchain, scaling, FSR 1 / CAS / Anime4K passes, frame dumps |
| `gpu/platform/gnm.cpp` | GnmDriver: submits, the command thread, GPU events |
| `gpu/platform/video_out.cpp` | VideoOut: buffers, flips, vblank, the flip thread, `VW_FRAME_STATS` |
| `gpu/platform/equeue.cpp` | Kernel event queues (user, VideoOut, graphics events) |
| `gpu/platform/window.cpp`, `vw_overlay.cpp`, `vw_settings.cpp` | SDL3 window, the ImGui menu, `vw.ini` |
| `gpu/platform/vwgpu.cpp`, `gpu/vwgpu.h` | The C interface the runtime calls (`vwgpu_*`) |
| `tools/native/` | Build, checks, picture comparisons, shader dumps ([README](../../tools/native/README.md)) |
| `tests/test_native_shader.c` | Container scan, translator (+ `spirv-val`), disassembler |
| `tests/test_gpu_platform.cpp` | Command thread (indirect buffers, labels, end-of-pipe) and event queues |
| `tests/test_build.c` | The HK address map, including every hook |

`gpu/VENDOR.txt` lists what still follows shadPS4 (layouts and values only, no code).

## 3. The shader translator

The game has 1743 shader blobs (1731 unique; 620 VS, 1120 PS, 3 CS); `ns_find_shaders` finds
1726 VS/PS containers. `e041714c` and `546af9ea` have no container the scan recognises and are
not translated; nothing seen so far draws with them (the renderer logs a missing shader once).

The model (`src/native_shader_spirv.c` top comment has the details):

- One invocation per GCN lane. Each VGPR is a `u32` function variable; scalar registers that hold
  descriptors or table pointers are tracked while translating, so every resource is known by its
  game slot; 64-bit lane masks become one bool per invocation.
- Branches that skip when no lane is active fall through; while and do-while loops become
  structured loops; anything else fails the shader by name.
- No float execution modes are declared, and `NoContraction` sits only on the few operations
  that need exact results. (Issue #74: strict float modes made AMD's Windows driver run the
  title's blur 8x slower under v1.5.2.)
- The interface with `native_vk.c`: push constant = the records' address; the record index is
  `gl_InstanceIndex` (the draw's `firstInstance`), passed flat at location 15; record layout
  `slot[8]` @0, `block[2][16]` @32, `stream[16]` @288 (544 bytes). **This layout is written in
  both files; change them together.**

**Adding an instruction** (a new game, a later chapter, or `not handled` in the log):

1. `cc -std=c11 -O2 -Isrc tools/native/ns_dump.c src/native_shader*.c -o out/ns_dump`, then
   `out/ns_dump <eboot> <hash>` prints the shader's header and disassembly.
2. If the decoder prints `?`, add the opcode to the table in `src/native_shader.h` (encoding and
   number from the GCN3/Sea Islands ISA manual).
3. Implement it in the `switch (n->operation)` of `native_shader_spirv.c`.
4. Add a small hand-assembled case to `tests/test_native_shader.c` (see `white_ps`), then run
   `bash tools/native/check.sh <eboot>`: every shader must still pass `spirv-val`.
5. Bump `CONVERTER_VERSION` in `src/native_vk.c` so players' converted shaders are redone.
6. Compare the scene with `tools/native/parity.sh`.

## 4. Supported builds, and adding one

v01.04 (the 1.04 update, also with the fan English patch, whose eboot is the official one) and
the Hong Kong 1.05 package. The HK build has the same code moved; the hooks map like this:

| v01.04 range | HK shift | What |
|---|---|---|
| `0x3ede0` | +400 | engine: fill memory with a compute shader (target clears) |
| `0x20df20` | +944 | detileSurfaceRegion (save thumbnails) |
| `0x2146d0`-`0x215b10` | +944 | Gnmx context state, draw, dispatch |
| `0x218ae0`-`0x21c1f0` | +960 | Gnmx LCUE resources, shaders, submit, viewport |

They were found with `tools/re/map_addresses.py <v0104.elf> <hk.elf> <addresses...>` on the
Ghidra ELFs (`tools/re/make_ghidra_elf.py`): every hook matched 0.94-1.00 of its surrounding
bytes. For another build: do the same, add ranges to `unicorn_hk0105_ranges` (or a table of
its own), add asserts to `tests/test_build.c`, and check the title and a save by picture.

## 5. Debugging

| Switch | What it does |
|---|---|
| `VW_FRAME_STATS=1` | FPS and the worst frame every 5 s; the renderer adds `Native:` lines every 600 frames (lists, draws, recording ms, replay ms, GPU wait) |
| `VW_VSYNC=0` | Uncapped. **Use it for every measurement**: KWin throttles a window it considers hidden to 20 FPS with VSync (vkcube too) |
| `VW_NATIVE_DUMP_TRIGGER=<file>` + `VW_NATIVE_DUMP_DIR` | Touch the file: the next frame's lists as text (every command, target and shader) |
| `VW_NATIVE_SHOT_TRIGGER=<file>` | Touch the file: the display image as PNG |
| `VW_NATIVE_SHOT_TARGET=<hex>` (`VW_NATIVE_SHOT_ALPHA=1`) | That render target instead (its alpha as grey) |
| `VW_NATIVE_WATCH=<hex>` | Log the first draws into that target, and why any was skipped |
| `VW_NATIVE_WHITE=<hex>` | That texture reads as white |
| `VW_NATIVE_LOG_TARGETS=1` | Log each new render target |
| `VW_NATIVE_SHADERS=<dir>` | Use converted shaders from that folder as they are |
| `VW_GNM_LOG=1` | Each PM4 packet type the command thread sees, once |
| `VW_VK_VALIDATION=1` | Vulkan validation layers |
| `VW_GPU_ID=<n>` | Which GPU (index in the log's list) |
| `VW_FRAME_DUMP_TRIGGER=<file>` | The presented frame as raw BGRA (`tools/uo_pad.sh dump` turns it into PNG) |

When a frame logs something new (an unknown shader, an unbound constant slot, an unhandled
primitive), it is written as `run_uo/drive/native/report_<frame>.txt` by itself.

Symptoms and where to look first:

- **Black or missing**: the `Native:` lines; then the frame dump, target by target, with
  `VW_NATIVE_SHOT_TARGET`.
- **Scrambled texture**: detiling (`untile` in `native_vk.c`). Tile mode 13 stores each 8x8
  micro tile in Morton order (x0 y0 x1 y1 x2 y2) for every element size; for BC formats an
  element is a 4x4 block.
- **Wrong colours**: RGBA8 target channel order, the BC4 swizzle, blend bits.
- **A sprite missing in one scene**: the vertex input → buffer slot mapping is per draw (the
  fetch shader, 4th argument of the VS hook, `gnm_fetch()`), not per shader.
- **Hang at start or in a loading screen**: a label the game waits for was not written. Run
  with `VW_GNM_LOG=1` and look for a new packet type; `VW_HANG_DUMP=5` shows where every guest
  thread waits.
- **Crash at boot in the command thread**: resolve the address with `addr2line -e out/vwport`
  (static build, load base 0x400000).
- **A GPU crash** prints `GPU crash: the graphics device was lost` and exits with 23.

## 6. How to verify a change

Verify by picture, not by "it runs". Every claim needs the image pair or the log line behind it.

1. `bash tools/native/check.sh <eboot>`: build, all tests, every shader through `spirv-val`.
2. `VW_GAME_DIR=<dump> bash tools/native/parity.sh <out> title|opening|options|load:N`: the same
   scenario with this build and a reference build, side by side. Build the reference from the
   commit before your change (or `0b09c1a` for the last shadPS4 renderer) with
   `tools/native/build.sh` and copy `out/vwport` to `out/ref/vwport_shadps4` (or set
   `VW_PARITY_REF`). Animated scenes never match exactly: look at the pictures.
3. For speed: `tools/native/bench.sh` before and after, same machine, `VW_VSYNC=0`.

What v1.6.0 was checked with (RX 560X and Vega 8, Linux, against the shadPS4 renderer):
title, the New Game opening to the chapter 1 title card, issue #34's two saves (Ravaged Swamp
quest, battle map, stage menu), the English patch's dialogue, French from the Switch text,
the Options screen with Xbox icons, 720p + FSR, 4K, 21:9, 32:9, Anime4K, save thumbnails, and
the HK 1.05 title. Performance: the opening at 60 FPS (v1.5.2: 36-54) on the RX 560X.

## 7. What is left, most useful first

1. **Later chapters.** Nothing past chapter 1 has been seen. Load late saves with both builds
   (`parity.sh load:N`) and watch for `Native:` lines (`no converted`, `not handled`, `skipped`).
2. **Windows, NVIDIA, Intel, Steam Deck.** CI builds Windows; nobody has run it there yet.
3. **Depth and culling** are not applied (nothing seen needs them); **rect lists** (primitive
   0x11) are not drawn; compute shaders other than the fill are not run; render targets are
   assumed RGBA8.
4. **CPU reads of rendered memory** other than the thumbnail path return nothing.
5. **Long sessions**: target, texture and pipeline caches are not bounded; textures are checked
   for new content round robin (64 MiB per frame) and never freed.
6. **Recording** runs on the game thread (~0.6 µs per draw); replay could move to its own thread.
7. Small cleanups found in review: the presenter's `ChooseFormat` falls back to R8G8B8A8 even
   when the surface does not list it; the record layout is defined in two files (a shared
   header with `offsetof` asserts would catch drift); `video_out.cpp` reads `VW_FPS_LIMIT` and
   `VW_VBLANK_HZ` twice; `src/runtime_memory.c` still has the shadPS4-era GPU interface (page
   tracking, write faults, `VW_TOGGLE_FILE`) that nothing uses; `RADV_DEBUG=nohiz` is still set
   for AMD on Linux although the renderer uses no depth buffer.

## 8. Rules

- Never commit or ship converted game shaders, frame dumps of game content in the repository,
  or anything from the game.
- Kill the game with `pkill -x vwport`, never `pkill -f`; a copied reference binary has its own
  name (`pkill -x vwport_shadps4`).
- Don't pipe `go.sh` or `parity.sh` into another command: the game inherits the pipe and the
  command never ends. Redirect to a file.
- Never press Cross during narration in battle scenes when driving the game by script.
- One change, one comparison.
- `main` is protected: every change goes through a pull request with green Linux and Windows CI.

## 9. Reference: the game's executable (v01.04, from Ghidra)

| Layer | Where | Notes |
|---|---|---|
| Render passes | `0x3220`, `0x3390`, `0x3564` (sprites), `0x470c0`, `0x232140`-`0x252090` (post) | run from display-list packets by a `RenderDLThread` |
| Engine device layer | `0x22d00`-`0x4df80`, about 45 functions | class `FnPS4_Device`; device = `FUN_00000700()->+0x40` |
| **Gnmx (Sony's library, statically linked)** | `0x214000`-`0x21d000` | **hooked here** |
| `sceGnm*` imports | 97 imported, 23 called | one draw call, `sceGnmDrawIndex` |

Hooked Gnmx entry points (first argument is the graphics context unless noted; LCUE functions
take `context + 0x1c8`):

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
| `0x218df0` | LCUE setBuffers(stage, slot, V#) |
| `0x218fe0` | LCUE setSamplers(stage, slot, S#) |
| `0x2191b0` | LCUE setConstantBuffers(stage, slot, V#) |
| `0x2193a0` | LCUE setRwBuffers |
| `0x219580` | LCUE setVertexBuffers(stage, first slot, count, V#) |
| `0x2199b0` / `0x219b50` / `0x219d00` | LCUE set VS / PS / CS |
| `0x21be90` | submit(context) |
| `0x21c050` | submitAndFlip(workload, handle, buffer, mode, arg, context) |
| engine `0x3ede0` | fill memory with a compute shader: how the game clears targets |
| `0x20df20` | `GpuAddress::detileSurfaceRegion` (save thumbnails) |

Not hooked (the game's own code runs): `0x21be50` reset context, `0x215560` command buffer
allocation, `0x215b70`/`0x215c50` dmaData/fill and the label/wait helpers `0x215d40`,
`0x215f60`, `0x215df0`, `0x215410` (their packets reach `gnm.cpp`). Engine `0x3ebc0` clears a
render target (calls `0x3ede0` or draws a quad); `0x44690` / `0x44910` / `0x44a90` make a
texture from GNF data / an empty writable texture / a render target object. Shader stages are
numbered 0 CS, 1 PS, 2 VS.

Facts about the data:

- **Shader identity**: the Gnmx shader object's word 2 `<< 8` is the code address; the footer
  `OrbShdr` is at `(code[1] + 1) * 8` bytes; the hash is the u32 at footer + 0x10. A table of
  24-byte `{pointer, size, 0}` entries at `0x1403080`-`0x1600c40` points at the containers. PSSL
  reflection names follow each blob (`tools/native/analysis/names.py`). By name signature: 358 VS
  and 447 PS families, mostly permutations of one sprite shader (fog, gradient map, reflection,
  multi-texture, mask, dissolve) plus about 15 effects (aurora, mist, cloud, rain, snow, god
  rays, glare, heat shimmer, blur, post).
- **Constants**: the engine binds the same data to VS and PS at a slot: 0 common (matrix; floats
  16.. material colour or `g_Rate`, 20..23 `g_K`, 28.. uv scale), 1 material colour, 2 fog colour
  (PS) or multi XYWH (some VS), 3/4 god ray and glare XYWH, 6 shading colours, 8 gradient map,
  9 gradient map parts.
- **Vertex inputs**: every input has its own buffer slot. Plain sprites use 0, 1, 2 (position,
  colour, uv); the two-key-frame sprite 2, 3, 4, 5 and uv on 8; the four-key-frame sprite 0..7
  and uv on 8.
- **Textures**: BC7 and BC4 (fonts) in tile mode 13; render targets RGBA8 in tile mode 14; the
  display buffer tile mode 10 (the last draw is a quad list, 0x13, into it). No mip levels seen.
  Slots: 0 main, 2 multi texture, 4 gradient map (512x2048, one palette per row) or god ray,
  5 glare.
- **Title screen frame**: 5 lists, ~240 draws, 7 VS + 5 PS + 1 CS, about 150 render targets
  (many 256² and 512²), primitive types 5 (fan) and 6 (strip).
- **Movies** are decoded by `sceVideodec2` (`src/runtime_videodec.c`) and drawn by the game with
  its `g_TextureY`/`g_TextureV` shaders: no separate path in the renderer.

Ghidra: `tools/native/ghidra/gh.sh` drives a headless project (`UO_GHIDRA_PROJECT`, `GHIDRA_HOME`);
the analysis scripts read the linked image from `UO_ELF` (`eboot_ghidra.elf`).

## 10. History

- 2026-11-12: hooks and recorder; replay beside shadPS4 for comparison; then "own" mode (the
  hooks stop calling Gnmx) shared shadPS4's device and beat it (60 vs 36-54 FPS in the opening).
- 2026-11-13: own GCN → SPIR-V translator (decoder opcode-identical to shadPS4's over 422,968
  instructions, all 1726 shaders through `spirv-val`); own Vulkan device, presenter and overlay;
  own GnmDriver, VideoOut and event queues without PM4 emulation; shadPS4 deleted; HK 1.05
  mapped; feature parity checked against v1.5.2; released in v1.6.0 as experimental.
- Notes of the shadPS4 era (the threaded draw pipeline, readback options) are in git history
  before v1.6.0 (`docs/dev/parallel_gpu.md`).

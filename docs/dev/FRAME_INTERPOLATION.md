# Frame interpolation (experimental setting, off by default)

Goal: show more than 60 frames per second while the game still runs at 60. Running the game
faster is not an option: its movement is tied to frames (the uncapped presets cap at 120 for that
reason, see `gpu/platform/video_out.cpp`). The extra frames are made by the renderer, by blending
the last two frames the game recorded, the way the N64 PC ports do it.

## 1. What the N64 ports do

**Ship of Harkinian** (`soh/soh/frame_interpolation.cpp`, Emill's patch):

- Every matrix operation the game performs while drawing is logged into a tree. The game's
  display-list macros open a child keyed by `{pointer, int}` (usually the actor), which gives a
  path.
- At display time, frame N-1's tree and frame N's tree are walked together. Children are matched
  by key and occurrence index, and ops by type and ordinal.
- Each matched op's inputs or final matrix are blended with `w*old + step*new`. The result is a
  map from the game's own `Mtx*` buffers to replacement matrices.
- A new child (no old match) is drawn as it is. Angles take the short way round, and s16 angles
  snap when they turn more than 90°.
- Opt-outs: `DontInterpolateCamera` (a camera epoch for cuts) and `DontInterpolateChild(key)` for
  one gameplay frame (teleports).
- Matrices are blended relative to the actor's root matrix, to avoid the "paper" artifact when an
  actor turns.

**RT64 / Zelda64Recomp:**

- RT64 records the frame's commands, and a second renderer replays them at a different rate.
- Draws are matched between frames: by matrix-group IDs the game sets (`gEXMatrixGroup`), or
  otherwise by heuristics (rendering parameters, textures, position).
- Matched transforms are decomposed into translation, rotation and scale. The parts are blended
  separately and recomposed. Vertices can be blended too, using velocities.
- Each group has per-component modes (skip, interpolate, auto). `gEXMatrixGroupNoInterpolate`
  marks things that must snap.
- Zelda64Recomp patches the game to give each actor limb a stable ID
  (`ACTOR_TRANSFORM_ID_START + limb`, `patches/transform_ids.h`), and marks actors as "skip"
  on teleport frames.
- Known tradeoff: effects that accumulate over frames (the motion blur) had to be capped.

Shared lessons:

- Game logic is untouched.
- The display runs one game frame behind and blends from N-1 to N.
- Matching is the hard part. An unmatched draw is shown as it is, and anything in doubt snaps.

## 2. What our frames look like (measured)

With `VW_INTERP_PROBE=1` (`src/native_interp_probe.h`), each draw is matched to the previous
frame's draw by key (render target, VS, PS, texture 0, index count) and its ordinal within that
key. Averages per frame over 120 frames, v01.04, RX 560X:

| Scene | Draws | Matched | Same | Matrix only | Positions only | Both | Other constants only |
|---|---|---|---|---|---|---|---|
| Title | 253 | 244 (96%) | 19 | 129 | 12 | 42 | 42 |
| Battle map, still | 2582 | 2578 (99.8%) | 192 | 405 | 1 | 31 | 1948 |
| Battle map, camera pan | 1865 | 1853 (99.4%) | 13 | 1283 | 14 | 77 | 466 |

- **Vertex count of a matched draw never changed.**
- **Positions are local quads.** A sprite is 6 vertices of `float2` local coordinates.
- **The transform is in constant slot 0.** Floats 0..15 hold the full clip matrix (object and
  camera together), and floats 16.. hold `g_Rate`, the key-frame blend of the two- and
  four-key-frame sprites. A camera pan changes the matrix and nothing else, which is the N64
  situation exactly: blend the matrix.
- **Other constants** (colours, UV, `g_K`) change on most draws in a still battle scene. That is
  animation. It can stay at 60 steps.
- **Mismatches cluster in particles** (the title's embers). Same key, different sizes: an ordinal
  shifts when one particle dies.
- **No render target carries state between frames.** Not one target was sampled before it was
  drawn in the same frame (title and battle map). Every offscreen target is rebuilt each frame,
  so a frame can be replayed again at another blend without breaking anything.
- **Cost.** Replay takes 2.1-2.3 ms of CPU per battle-map frame (`Native:` line). GPU time per
  replay is not measured yet.
- **The game stays at 60 with `VW_VSYNC=0`**, because the guest vblank paces it.

## 3. Design

The game side does not change: the vblank stays at 60 Hz, and flips and their events stay at 60.

**Keep two frames.** The renderer keeps frame N-1's lists when frame N arrives, instead of
freeing them after replay.

**Match** (once per game frame, O(n log n)):

- Key every draw as the probe does, and pair draws by key and ordinal.
- If a key has a different number of draws in the two frames, that whole key snaps (the particle
  case).
- If most matched matrices jump (a camera cut or scene change), the whole frame snaps.

**Blend** each matched draw, with alpha in [0, 1):

| Data | Rule |
|---|---|
| Slot 0, floats 0..15 (the clip matrix) | Element-wise lerp. Snap if the translation row moves more than a threshold in clip space (a teleport or a mismatched pair) |
| Slot 0, `g_Rate` | Lerp only when the key-frame vertex streams are the same in both frames; otherwise the new value (the key frame advanced) |
| Position stream (location 0) | Lerp when it changed and the delta is small; otherwise new |
| Everything else (textures, colours, UV, other slots) | New frame, stepping at 60 |
| Draw with no partner | Drawn as in the new frame |
| Draw only in the old frame | Not drawn |

**Replay an in-between frame.** Copy frame N's lists, patch the blended payloads in place (only
matched draws that changed), and replay all passes into the display image. The probe showed that
offscreen passes are safe to repeat.

**Pacing.**

- In interpolation mode, a present thread runs at the display's refresh rate. The flip thread
  keeps signalling the game at 60 and hands it real frames.
- A present at time t uses alpha = (t - T_N) / 16.67 ms, clamped to [0, 1], and blends N-1 → N.
- This adds one game frame of latency, the same tradeoff as RT64 and SoH.
- If no new frame arrives (loading, the game waiting), alpha holds at 1. No extrapolation.
- Save thumbnails (`native_target_read`) must see a real frame, so run an alpha = 1 replay before
  a readback.

**Setting.** Add Off / Display rate / 120 / 144 to the Insert menu and `vw.ini`. Off is today's
path, unchanged.

## 4. Steps, each checked by picture or measurement

1. **Still frames.**
   - Add `VW_INTERP_STILL=<alpha>`: present frame N-1 → N blended at a fixed alpha.
   - Check: frames at 0, 0.5 and 1 during a camera pan; 0.5 must sit between the other two.
   - Check: character walks, the title's particles, menus, fades, and a movie.
2. **Present thread and pacing at 120 Hz.**
   - `VW_FRAME_STATS` must show 120 presents and 60 game flips.
   - Measure GPU time per replay with timestamp queries on the RX 560X and Vega 8.
   - Capture a slow-motion recording of a pan.
3. **Cost, if needed.**
   - On in-between replays, skip offscreen passes whose draws are all unchanged and reuse their
     targets.
   - Move replay to its own thread (item 6 in NATIVE_RENDERER.md §7).
4. **Tagging, only if heuristics fail somewhere visible.**
   - Do it the Zelda64Recomp way: hook the engine's sprite pass (`0x3564`) to record the sprite
     object's pointer as a label before its draws, and key on that instead of the ordinal.
5. **Release.**
   - Add the menu setting, Windows CI and the HK 1.05 build.
   - Ship it as experimental, off by default.

## Status (2026-11-14, RX 560X, v01.04)

**How to use it.** Turn on "Frame interpolation" in the Insert menu (Image tab, under VSync).
- It is saved as `frame_interpolation=` in `vw.ini`; `VW_INTERP=1` or `0` overrides it.
- It switches live. When it is off, the old path runs unchanged.

**Code**
- `src/native_interp.h`: matching and blending.
  - `native_interp_plan` runs once per game frame, `native_interp_apply` once per present.
  - Tests: `tests/test_native_interp.c`.
- `src/native_vk.c`:
  - Interpolation mode: `interp_accept` on the game thread (it only queues), and
    `native_interp_next` for the present thread (plans, then replays at alpha).
  - `VW_INTERP_SHOT=<file>` takes picture checks when the mode is off.
  - `VW_INTERP_TRACE=<n>` logs pacing.
- `gpu/platform/video_out.cpp`: `PresentThread`, which idles while the mode is off. `Present`
  is behind a mutex, since both threads may present at a switch.

**Matching changes since the design**
- Draws in a key pair with the closest old draw by origin, not by ordinal. A sprite that enters
  or leaves the view no longer snaps its group. During a battle-map pan this cut snaps from 321
  to 6-44 per frame.
- A draw keeps its own-order partner without a search when the group is the same size and the
  partner is within 0.05.

**Checked by picture**, with frame pairs at alpha 0, 1/4, 1/2, 3/4 and 1:

| Scene | Movement at those alphas | Notes |
|---|---|---|
| Battle-map pan | 0 / 5 / 11 / 16 / 22 px | Units, badges, houses and the knight move together; the cursor stays; a 347-patch scan found nothing that jumps |
| World map, walking | 0 / 0 / -1 / -1 / -2 px | |
| World map, running | 0 / 2 / 4 / 6 / 8 px | The far field moves less (perspective); the minimap stays |

**Measured**, live, with `VW_VSYNC=0 VW_FRAME_STATS=1`:

| Scene | Game | Presents | Blended per frame | Snapped per frame |
|---|---|---|---|---|
| Title | 60.0 | ~116-133 | | |
| Battle map | 60.0 | 72-85 | | |
| World map, running | 60.0 | 82-84 | ~970 | 3 |

- The game stays at 60.0 in every scene.
- The live switch worked four times in a row: off goes back to plain 60, on goes back to blended
  presents.
- Planning costs about 1.4 ms per game frame; applying a blend under 0.1 ms per present.

**Step 3 was tried and removed.** It marked the targets no blended draw reaches (directly or
through sampling, to a fixpoint) and skipped their draws and fills when a frame was replayed
again.
- A battle frame draws into only about 10 targets, and the 7 that matter (the scene,
  post-processing, the display) must be redrawn anyway.
- Battle map 83 → 85 FPS, title 98 → 116, for about 100 lines and +1.3 ms of planning per frame.
- On the RX 560X a replay costs about 10-12 ms of GPU, almost all of it the game's own scene.
  More frames there need a faster GPU, not less replay work.

**Not done**
- The setting does not switch itself off on 60 Hz displays; the menu text says it needs a faster
  display.
- `native_target_read` (save thumbnails) reads the last replay, possibly a blend.
- Not checked: fades, movies, effects positioned through other constant slots (they step at
  60), later chapters, Windows, the HK 1.05 build, a real >60 Hz display.

## 5. Open questions

- Effects positioned through other constant slots (slot 2 "multi XYWH", slots 3/4 god ray and
  glare XYWH) will step at 60 unless they get their own rules.
- Later chapters (rain, snow, aurora) are not measured. Run the probe on late saves.
- Screen-space effects that read the display image are not interpolated. None seen so far.

Sources:
- [SoH frame_interpolation.cpp](https://raw.githubusercontent.com/HarbourMasters/Shipwright/develop/soh/soh/frame_interpolation.cpp)
- [SoH frame_interpolation.h](https://raw.githubusercontent.com/HarbourMasters/Shipwright/develop/soh/soh/frame_interpolation.h)
- [RT64 extended GBI](https://raw.githubusercontent.com/rt64/rt64/main/include/rt64_extended_gbi.h)
- [RT64 README](https://raw.githubusercontent.com/rt64/rt64/main/README.md)
- [Zelda64Recomp transform_ids.h](https://raw.githubusercontent.com/Zelda64Recomp/Zelda64Recomp/dev/patches/transform_ids.h)
- [Zelda64Recomp actor_transform_tagging.c](https://raw.githubusercontent.com/Zelda64Recomp/Zelda64Recomp/dev/patches/actor_transform_tagging.c)

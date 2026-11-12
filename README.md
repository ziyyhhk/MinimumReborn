# Minimum Reborn

A rendering-focused optimization mod for Geometry Dash 2.2081, built with C++20 and the
Geode SDK. It is a successor to [Minimum](https://github.com/ziyyhhk/Minimum), with the
render path as the main subject rather than a settings collection.

**Status: v2.3.2.** The measured runtime evidence in this
README and in `DEVELOPMENT_REPORT.md` - the Engine vs Optimized A/B table and the skip counts -
comes from earlier in-game runs on a Windows laptop, not from estimates. Those runs predate the
per-quad batch filter, so they do not measure the current build. The custom panel was
**removed in v2.3.0**: every setting is edited on the native Geode settings page, which the
pause menu's MR button opens directly. v2.3.1 shipped the per-quad batch filter, the
custom-shader filter and the GPU compute batch filter as the first release since v2.3.0; it
also shipped a crash in the GPU compute batch filter (DEP violation, fixed in v2.3.2 - do not
use v2.3.1 with GPU Batch Filter enabled). v2.3.2 is a crash hotfix on top of v2.3.1. The game
has **not been launched or played** with the GPU Batch Filter; the FPS impact of the batch
filters and of the GPU frame-time line is **not measured**.

## What it does

Minimum Reborn changes execution paths in the draw traversal. Each feature can be turned off;
when it is off, the original path is taken unchanged. There are two renderer modes:
**Optimized** (the mod's draw hooks) and **Engine** (every per-node hook calls straight through
to the stock cocos2d function; only frame-level measurement runs). The Default profile
selects Engine.

### 1. Sprite draw fast-out

`CCSprite::draw()` is not universal in GD: level objects are submitted through
`CCSpriteBatchNode`, which draws its whole texture atlas in one call and never calls the
per-sprite hook. The fast-out therefore applies to the unbatched sprites that do reach
`CCSprite::draw()` — HUD elements, effects and content from other mods. Measured on Stereo
Madness: **11,891–18,939 sprite visits per 10 s run, about 20–32 per frame**. That is where
this hook can act, not thousands of level blocks.

For those sprites, opacity 0, scale 0 on one axis, or an empty content size cannot produce a
pixel; the mod returns before the draw call for exactly those cases. Children are drawn by
`visit()` independently of the parent's `draw()`, so the image is unchanged.

### 2. Scene object culling (opt-in, off by default)

A sprite whose whole quad lies outside the viewport plus a margin cannot contribute a pixel,
so its draw call is skipped. The test uses the parent's world basis, cached once per parent per
frame, and a rotation-safe circle (content diagonal × scale). The viewport is expanded by
160 units in Conservative mode and 16 in Aggressive. Any doubt — no parent, non-finite
transform, empty size — answers "draw". Only `draw()` is skipped; `visit()` still runs, so
updates, animations, triggers, children, collision and physics are untouched.

**Runtime result:** in Stereo Madness this test examined **18,939 sprites over 10 s and skipped
0**. GD already avoids drawing objects that are far outside the view, and the objects it does
draw are batched. The cull is useful for unbatched sprites (UI, effects, modded scenes) and is
kept as an opt-in in Performance/Extreme, but it is **not** a level-object culler. Keeping it
on by default would only add checks with no measured saving, so Balanced now leaves it off.

### 3. Batched level objects: per-quad filter (on by default)

This is the path GD actually uses for level objects. `CCSpriteBatchNode::draw` submits every
child in one GL call, so the per-sprite hook can never see them. GD's visibility pass hides an
object by collapsing its four vertices to `(0,0,0)`, but the batch still uploads and shades
every hidden quad.

The mod now rebuilds only the index list for that one draw. Zero-area quads (hidden objects,
collapsed sprites) and quads whose four projected corners lie fully outside the clip volume plus
a small margin are left out; every kept quad stays in its original atlas position and its
vertices are byte-identical to the normal draw. The batch keeps its single GL draw, its blend
state and its normal vertex upload, so overlapping objects blend in the same order.

The filter is fail-open by construction. It runs for the stock `PositionTextureColor` program,
or for a custom program whose vertex stage is byte-identical to the stock pass-through (the
shape of fragment-only effects GD's shader library uses; `custom-shader-filter`, default on). A
custom vertex stage that could displace geometry, an active grid, a non-screen-space projection
or any doubt falls back to the untouched draw. The child `updateTransform` pass runs first,
exactly where the original runs it, and the quad count is re-read afterwards because that pass
can grow the atlas. More than 16,383 quads, a non-finite coordinate, a pending child sort or an
exhausted per-frame budget (65,536 quads) falls back to the untouched draw. If no quad
survives, the GL submission is skipped and the atlas dirty flag stays set for the next normal
upload. Only `draw()` is affected; `visit()`,
updates, triggers, physics and object management are untouched.

An optional GPU path (`gpu-batch-cull`, Windows, GL 4.3, off by default) runs the same scan on
the GPU instead of the CPU for batches larger than 1024 quads; see section 16.

**Status: not measured in-game.** The counter to watch on the first run is `quads culled /
scanned` on the Detailed overlay; the level must look identical with the filter on and off.

### 4. Particle system fast paths

* **Idle systems.** `CCParticleSystemQuad::draw` binds a texture and submits a draw even with
  zero live particles. Idle systems are skipped. Simulation is not touched.
* **Off-screen systems.** A live system whose particle AABB does not touch the expanded
  viewport is not drawn. The box is measured every frame from the actual particle positions
  (`m_pParticles`), expanded by each particle's size; the local center is mapped to the world as
  a rotation-invariant circle through the emitter's basis. Relative and Grouped systems
  (both live in emitter-local space) are covered. It still updates, so emission timing and
  motion are unchanged when it returns. Free systems are drawn as before - their absolute frame
  has no transform safe to assume. If the emitter itself is on screen the particle data is not
  scanned at all: one circle test decides. (v2.0.0 rebuilt this from the cocos2d-x 2.2
  headers; the previous worst-case reach cone could rarely cull anything.)
* **Measured:** Stereo Madness exposes **one particle system per frame** to the hook and it
  stayed on screen for all 601 frames, so 0 draws were skipped. The hook and its counters are
  exercised; this level simply has no off-screen effects. Effect-heavy levels are where the
  skip can fire, and the Detailed overlay counters report it so it is not taken on faith.
* **Pool cap.** The requested pool size is clamped *before* the original
  `initWithTotalParticles` runs, so the particle, quad and index buffers are allocated for the
  capped size. Nothing can index past its buffer. Affects systems created after the change.
* **Adaptive cap (off by default).** While drawn frames stay slower than the Target FPS, the
  pool is stepped down (never below 24) and logged; it relaxes back when frames recover.
* **Spawn budget (off by default).** After N systems are created in one frame, further
  systems get a pool of 4: bursts look thinner instead of stalling the frame.

### 5. Render gate (experimental, off by default)

A `CCDirector::drawScene` hook that can skip *rendering* while the scheduler keeps its normal
rate. A skipped frame runs one scheduler update with the delta the main loop already computed;
it does not call `calculateDeltaTime()`. Physics, collision, input processing and the FPS cap
are untouched. It only makes sense above the display refresh rate (uncapped FPS), and it
delays what you see by up to one render interval, which is why it is off by default.

While the window is unfocused the same mechanism draws at Background FPS (default 20).

### 6. Windows system tuning

1 ms timer resolution, opt-out of Windows power throttling (EcoQoS), optional process
priority. Applied only when the wanted state differs; undone when switched off.

### 7. Performance overlay and diagnostics

FPS (drawn frames, wall clock), average and p95 frame time, 1% low, scheduler updates per
second, CPU time inside the draw hook, and culling telemetry side by side with what it was
measured against: scene draws skipped *of* tests performed, particle draws skipped *of*
systems examined, invisible-sprite skips, capped pools, logic-only frames, batch quads culled
*of* quads scanned. A clearly-labelled bottleneck *hint* says whether the draw path itself is
spending most of the frame. One label, updated four times a second, only when the text changed.
No per-frame nodes. The GPU line is a `GL_TIME_ELAPSED` query read back one frame late with a
non-blocking availability check, so it never stalls the frame; it is available on Windows.
The default corner is Top Right because GD's attempt counter occupies the top left.

### 8. In-game benchmark (removed in v2.3.0)

> **Removed:** the benchmark button lived on the custom panel, which was deleted in v2.3.0.
> `benchmark-seconds` is inert and kept only so existing settings files stay valid. Frame-time
> stats shown on the overlay come from `frameStats()` directly.

The pause panel's Bench button records **unpaused drawn frames** for the configured duration
(default 10 s) and reports average FPS, average/p50/p95 frame time, 1% low, variability, draw
CPU time, culling deltas, invisible-sprite skips and the real batch path (draw count and quad
count). The same line is written to the Geode log. Recording uses a fixed 16384-sample buffer
(no allocation in the frame path); sorting happens once at stop.

Starting the benchmark from the pause panel does not record the pause screen: the window
starts on the first frame after you resume. Use it to compare Engine vs Optimized, or
Conservative vs Aggressive, on the same level and position.

## Measured performance

Environment: Windows laptop, Geometry Dash 2.2081, Geode 5.11.2, window 1176 × 664, level
Stereo Madness (Normal), an auto-player mod active, in-level FPS cap 60. Each run was started
from the pause panel and resumed immediately. Runs A–D are v1.2.0 and counted about 1 s of
pause screen at the start; run E is v1.2.1 with paused frames excluded.

| Run | Version | Frames | Avg | p50 | p95 | 1% low | Draw CPU | Scene culled | Particles skipped | Sprite skips | Batches (quads) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A | 1.2.0 | 601 | 16.67 ms | 16.34 ms | 17.36 ms | 39 fps | 3.10 ms | 0 / 18,939 | 0 / 601 | — | 24,671 (231,543) |
| B | 1.2.0 | 601 | 16.67 ms | 16.33 ms | 17.36 ms | 46 fps | 2.79 ms | — | — | — | — |
| C | 1.2.0 | 601 | 16.67 ms | 16.33 ms | 17.36 ms | 44 fps | 2.88 ms | — | — | — | — |
| D | 1.2.0 | 601 | 16.67 ms | 16.33 ms | 17.35 ms | 42 fps | 2.78 ms | 0 / 11,891 | 0 / 601 | — | 20,898 (211,759) |
| E | 1.2.1 | 602 | 16.67 ms | 16.33 ms | 17.36 ms | 38 fps | 2.66 ms | 0 / 0 (off) | 0 / 598 | 1,516 | 21,330 (210,943) |

What this does and does not show:

* **The FPS number cannot improve here.** GD caps in-level rendering at 60 FPS and this scene
  holds that cap in every mode. Average frame time is identical (16.67 ms) because it is the
  cap, not the renderer.
* **Draw CPU is not measurably better or worse.** `drawScene` cost 2.78–3.10 ms with the
  optimized path and 2.79–2.88 ms with the engine path. The ranges overlap and the runs cover
  slightly different parts of the level (the auto-player restarts attempts), so this is a
  wash, not a win. It is also why the README does not claim an FPS gain.
* **GD batches level objects.** 20,898–24,671 batch draws (35–41 per frame) and 211,759–
  231,543 quads (352–385 per frame) per 10 s are the actual level path. The per-sprite hook
  saw about 3% as many sprites. Any future level-object work has to happen inside the batch
  node, not in `CCSprite::draw` - which is exactly what v2.4.0's per-quad filter does.
* **v2.4.0 is not in this table.** The per-quad filter, the shader-trace GD timings and the GPU
  frame-time line have no measured run yet. Do not read these rows as evidence for them.
* **Culling fired zero times.** GD's own object management plus batching leaves nothing for
  the per-sprite test. The default now reflects that. Particle culling has no off-screen
  targets on this level either; the counters are there so the claim can be checked on levels
  that do.
* **The sprite fast-out did fire.** Run E skipped 1,516 draws in 10 s (~2.5 per frame) for
  sprites with opacity 0, zero scale or empty size. That is the one skip path with a measured,
  non-zero effect here; it is small in absolute terms and is not presented as an FPS win.
* Where the mod can still help: uncapped/high-refresh rendering, batching-heavy custom
  content seen through the sprite path, particle spam, and background throttling. Those are
  not demonstrated by the table above.

## What is NOT implemented

These were considered and deliberately left out rather than faked:

* **Replacing GD's batching.** GD already draws level objects through `CCSpriteBatchNode`
  layers (measured above). Merging those batches across textures would require reordering
  translucent objects and rebuilding texture atlases; a partial version that silently changes
  draw order was judged more dangerous than valuable. v2.4.0 filters **individual quads**
  inside each batch instead (section 3): it drops only quads with zero area or fully outside
  the clip volume, keeps atlas order, blending and the single GL draw, and never rewrites the
  atlas. GPL-licensed third-party renderers were studied only as a design reference; **no code,
  shaders or assets from them are used here.**
* **Dynamic resolution / render scale.** Cocos2d-x 2.2 in this game exposes no safe
  render-target resize path through the SDK; changing window or viewport size is not an
  equivalent and was not attempted. Documented as future work.
* **A GPU-heavy renderer rewrite.** No compute-shader or multi-pass renderer replaces the
  engine. The mod changes which existing quads are submitted, not how the engine shades them.
  v2.3.1 adds an optional GPU implementation of the existing per-quad batch filter
  (`gpu-batch-cull`, off by default, section 16). That is the only part of the GPU
  direction present in this build; there is no replacement renderer, no sprite-mesh
  rendering, no group-state transform engine and no custom z-order sorting. The GPL-licensed
  third-party renderers in this space were evaluated as a design reference only; none of
  their code, shaders or data files are included.
* **Automatic quality presets by frame time.** The only automatic adjustment is the opt-in
  particle cap (off by default), which changes pool allocation, never simulation.
* **A "disable glow" switch.** GD's glow is part of the batched level path, so neither the
  sprite fast-out nor the scene cull can skip it, and a toggle would remove visuals without
  removing the batch submission. Not added.

## Settings and profiles

Profiles (`Custom`, `Default`, `Balanced`, `Performance`, `Extreme`) write their values into
the individual settings once; editing any setting switches the profile to Custom. `Balanced`
is the default. A profile also selects the renderer mode and both cull modes:

| Profile | Renderer | Scene cull | Particle cull | Batch filter | Particle cap | Background FPS |
|---|---|---|---|---|---|---|
| Default | Engine | Off | Off | Off | 256 | 20 |
| Balanced | Optimized | Off | Conservative | On | 128 | 20 |
| Performance | Optimized | Conservative | Conservative | On | 64 | 15 |
| Extreme | Optimized | Aggressive | Aggressive | On | 32 | 10 |

The full list is in `mod.json` and appears in Geode's native settings page, which also
provides reset. The pause menu's MR button closes the pause and opens that page directly.
The custom panel and its Benchmark button were removed in v2.3.0.

Settings are cached in a snapshot refreshed twice a second and immediately after a UI change.
Hot paths never look up setting keys.

## Input responsiveness

Minimum Reborn **adds no input hook, queue, buffer, lock, thread or sleep**. The game reads
input on the main thread exactly as before. The render gate never postpones input processing:
on a skipped render frame the scheduler still runs. No physics or timing values are modified.
Two opt-in switches change *when* things happen, not the game's input code: **No Additional
Input Delay** (this mod's own design - see section 13) moves all mod bookkeeping behind the
game's update and behind the scene draw, and **Low Latency (drain GPU queue)** calls `glFinish()`
after each submitted frame to shorten the driver's queue. The latter trades average frame rate
for lower input-to-screen delay and is not measured here.

What the numbers mean:

* *Input processing overhead*: work between a click and the game acting on it. Unchanged by
  this mod (no code in that path).
* *Render frame time*: time to produce a frame. Measured, and reduced where draws are skipped.
* *Display latency*: time from the finished frame to photons. Not measured here.
* *Total hardware-to-photon latency*: not measured. This mod does not promise "zero latency";
  it promises no intentional added delay.

## Build

Requirements: CMake ≥ 3.21, a C++20 compiler, and the Geode SDK 5.11.2 with `GEODE_SDK`
pointing at it (`mod.json` pins `"geode": "5.11.2"`; an older SDK refuses to configure).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

or with the Geode CLI: `geode build`.

GitHub Actions builds automatically on push, pull request and manual dispatch. The Windows
`.geode` is uploaded as the artifact **MinimumReborn-Windows** on the run page
(Actions → run → Artifacts).

## Install

Copy the `.geode` file into Geometry Dash's `geode/mods` folder (or use the Geode mods
folder shortcut) and start the game.

## Verification

Build evidence:

* Workflow runs for v1.2.0 ([41011043806](https://github.com/ziyyhhk/MinimumReborn/actions/runs/41011043806))
  and v1.2.1 ([41012157311](https://github.com/ziyyhhk/MinimumReborn/actions/runs/41012157311))
  completed green on Windows and uploaded **MinimumReborn-Windows**. The v1.2.1 package was
  downloaded and inspected: `mod.json` v1.2.1, `about.md`, `changelog.md`, `.geode_cache`,
  and `ziyyhhk.minimum-reborn.dll` 208,384 bytes (Release, LTO). The archive measured
  86,352 bytes packed / 241,101 bytes unpacked; doc-only rebuilds change the text by a few
  hundred bytes, the DLL does not. The small size is the point: one DLL plus text; cocos2d,
  Geode and the C++ runtime are provided by the game install, not bundled. Every source file
  in `CMakeLists.txt` is compiled; nothing is inflated to look substantial.
* Earlier runs are visible in Actions; run 41008194965 failed to compile
  (`CCSprite::getBoundingBox` is not exposed by the bindings) and was fixed in 41008425934.

Runtime evidence (this laptop, details and numbers above):

* The mod loaded with all hooks enabled in the Geode log, the pause panel opened, the overlay
  drew, the Benchmark completed five times, and its numbers came from the same frame timing
  the overlay uses. v1.2.1 was installed over v1.2.0 and relaunched; Reset was tested (overlay
  moved to Top Right, profile Balanced, scene cull Off) and run E confirmed that paused frames
  are excluded. The panel and overlay were checked visually; the log lines quoted in this
  README are from the run.
* **Not measured:** other levels, other hardware, uncapped FPS, GPU time, long sessions,
  visual A/B against screenshots, input-to-photon latency. The culling's visual correctness is
  argued from the fail-open test and the fact that it skipped zero draws in the measured
  level; it was not exercised with a non-zero cull count in GD.
* The skip paths are written to fail open. If the viewport snapshot is missing or
  non-finite, every visibility test answers "draw". A sprite is rejected only when its whole
  quad is outside the expanded viewport; a particle system only when it is Relative or Grouped
  and its live-particle AABB misses the expanded viewport; a batch quad only when it has zero
  projected area or its whole projected box is outside the clip volume, and only under the
  stock program, or a custom program whose vertex stage is byte-identical to the stock
  pass-through (`custom-shader-filter`, default on), with no grid, no pending sort and a
  screen-space projection. Idle-system,
  zero-pixel and batch-quad skips can only fire where the original draw would submit geometry
  covering no pixels.

### Trace procedure (v2.3.1)

The old benchmark UI is gone. The replacement is the `shader-trace` setting plus the Detailed
overlay:

1. Enable `shader-trace`, start the level, and reproduce the slow section for at least 15 s
   (the log flushes every 5 s).
2. Read the `shader-trace` lines for total/redundant program binds and the top programs, and
   the `gd-trace` line for average/max CPU time in `PlayLayer::updateVisibility`,
   `updateShaderLayer`, `ShaderLayer::visit` and the batch scan.
3. Compare against the GPU line on the Detailed overlay. If `gpu` is near the frame time while
   the `gd-trace` numbers are small, the frame is GPU-bound and no CPU-side cull will fix it.
   If visibility or `updateShaderLayer` dominates, that is where the next change belongs.
4. Watch `quads culled`, `batch scans` and `fully culled`: a non-zero, plausible fraction
   means the filter is doing
   work. The level must look identical with `batch-culling` on and off.
5. For longer sessions use the overlay in Detailed mode. There is no automatic result line;
   the log is the record.

## Compatibility

* Windows, Geometry Dash 2.2081, Geode 5.10+.
* Do not combine the Render Gate with `mat.draw-divide`; both rewrite the draw cadence.
* Other mods that hook `CCSprite::draw` or particle systems will run alongside this one, but
  stacked draw-skipping has not been tested.
* Scene culling only sees nodes drawn through `CCSprite::draw`. It does not, and cannot,
  cull `CCSpriteBatchNode` content, which is how GD draws level objects. It is disabled
  automatically if the viewport snapshot is unusable.
* The sprite bound assumes a plain position/rotation/scale transform. A sprite with skew set
  could paint slightly outside the bound; such sprites are not expected in levels, and
  `renderer-mode = Engine` or `scene-cull = Off` disables the test entirely.
* Feature toggles exist so conflicts can be isolated without uninstalling.

## License

MIT. Original code by ziyyhhk. Minimum (MIT) was used as a behavioral reference for the
particle techniques, the render-gate scheduling approach, and the `low-latency` (`glFinish`)
idea; all are reimplemented for this project and no Minimum code is copied. The No Additional
Input Delay setting is Minimum Reborn's own design, not inherited from Minimum (see section 13).
No GPL-licensed third-party code is included.


## 9. v1.4.0 - the control panel rebuilt, and what the draw path really saves

### Layout

The panel is 400x240 design units inside GD's own popup frame and is laid out from one set of
constants: `kMargin`, `kHeaderH`, `kNavH`, `kFooterH`, `kRowPitch`, `kButtonHeight`. Header,
tabs, content rows and footer all derive their positions from those, so nothing is placed with
an unrelated hard-coded coordinate. Six tabs share the content width equally; the selected tab
is brighter, one step larger and carries a green accent bar on the shared baseline.

Controls are drawn from a 16x16 solid white PNG the mod owns (`resources/reborn-pixel.png`),
scaled to the row size and tinted. Three alternatives were tried and are now documented as
rejected because they were screenshotted failing:

* stretching GD's `square02b_001.png` into a 20-unit row drags its trimmed atlas margin into
  a sliver at one end of every pill;
* a `CCScale9Sprite` with hard-coded cap insets samples the wrong sub-rect of the trimmed
  frame, producing a rounded left end and a square right end;
* `CCLayerColor` builds its quad from the layer position in a way that does not follow the
  anchor point here, so bars and bodies landed mirrored away from where they were placed
  (and `setIgnoreAnchorPointForPosition` is not in this binding at all).

### Measured A/B, same level, same machine, 10 s each, 60 FPS cap

Stereo Madness, recorded with the panel's own benchmark button (the log lines are quoted
verbatim in `DEVELOPMENT_REPORT.md`):

| | drawn frames | avg frame | p95 | draw ms (CPU) | variability | invisible sprite draws skipped |
|---|---|---|---|---|---|---|
| Optimized | 610 in 10.0 s | 16.67 ms | 17.36 ms | 2.42 ms | 1.03 ms | 1,830 |
| Engine | 601 in 10.0 s | 16.67 ms | 17.35 ms | 2.35 ms | 1.02 ms | 0 |

What that does and does not say:

* The frame rate is identical because the game is capped at 60 FPS in this level; no FPS gain
  is claimed and none was measured.
* The optimized path costs **+0.07 ms of CPU time inside `drawScene` per frame** (about 0.4%
  of a 16.7 ms frame) while removing 1,830 zero-pixel draw submissions per 10 s. That is the
  honest size of the optimization's own overhead, measured rather than assumed.
* The 1% low came out worse in the optimized run (25 vs 53 fps). One 10 s run is not enough to
  attribute that; it is reported as-is, and it is the reason no "smoother" claim is made.
* Scene culling culled 0 sprites and particle culling 0 systems in this level, consistent with
  the batching finding: the per-node path only sees unbatched sprites.

### Shader-related work

See [`SHADER_CULLING.md`](SHADER_CULLING.md) for the full accounting: which operations are
avoided, where each decision is taken, how visibility is bounded, which object types are
covered, what stays untouched, how correctness follows, and what has been measured.


The skips return before `CCSprite::draw` / `CCParticleSystemQuad::draw`, which is where
cocos2d activates the `CCGLProgram`, sets the vertex attribute pointers, binds the texture and
issues the draw. So the work actually avoided per skipped node is program activation and
uniform setup, the attribute pointers, the texture bind, the draw call, and the fragment work
those triangles would have produced. That is the defensible reading of "avoid shader setup for
objects that will not be rendered".

What is deliberately not claimed: no shader is compiled, replaced or cached; no draw is
reordered, merged or re-batched; no individual quad is dropped out of a
`CCSpriteBatchNode` (its buffer is rebuilt only for dirty children, so a mod that rewrote it
would corrupt the next frame); and nothing is culled on its center alone. Batched level
objects therefore stay untouched, and the panel says so.

### Overhead of the instrumentation itself

The draw hooks used to perform a locked atomic increment per object. In a level with tens of
thousands of sprite draws per frame that made the counters one of the more expensive things in
the path, so they now accumulate into plain integers and are published once per frame by
`renderFrameEnd()`. The sprite visibility test also dropped a `hypot` per sprite by comparing
squared distance against squared radius.

### Platform

Windows only. `mod.json` declares `"gd": {"win": "2.2081"}` and CI builds Windows. The
Windows-specific features (1 ms timer resolution, background draw throttle, process priority,
render gate) sit behind `GEODE_IS_WINDOWS` in the source and are hidden in the panel on other
platforms, so the rest of the mod is written to compile elsewhere - but no Android or iOS
binary has been built or run here, so this package does not claim mobile support.


## 10. What this mod cannot fix (and why "Performance" still stutters)

Steady frame rate and spikes are different problems, and only some of them live in the draw
path this mod touches.

* **The FPS cap.** Geometry Dash caps the loop (60/90/144/240 by setting). While the cap
  holds, no renderer can report a higher FPS; the only honest numbers are frame time, CPU
  time inside `drawScene`, and how much draw work is not submitted. Section 9 shows exactly
  that measurement: identical FPS, +0.07 ms CPU per frame, 1,830 zero-pixel draws removed.
* **Level streaming and object creation.** GD builds objects, textures and particle systems as
  a level loads and as it scrolls. Those allocations, plus shader program creation on first
  use of a texture, are the usual source of a one-frame hitch. This mod does not run during
  them and cannot shorten them; the pool cap only makes *later* particle systems smaller.
* **Other mods.** 51 mods are installed on this machine. One of them (BetterEdit
  v6.10.0-beta.6) crashes the game when the editor pause menu opens, and its stack trace
  contains no frame from this mod. A mod that hooks per-frame drawing can also add cost that
  shows up as spikes; the Detailed overlay's frame-time lines are how you tell the two apart: if
  draw time stays low while the 1% low drops, the stall is outside the draw path.
* **GPU and driver work.** `gpu-frame-time` can report the GPU time of the submitted frame, but
  that is measurement only. A fragment-bound frame (large glow, full-screen effects, high
  resolution) is not something this mod can fix.
* **What it does help with, concretely**: hidden and off-screen unbatched sprites and particle
  systems stop paying shader/attribute/texture setup and a draw call; idle particle systems
  stop uploading an empty quad buffer; particle pools are bounded so a burst allocates less;
  and the mod's own per-frame bookkeeping runs after the game's update and after the scene
  draw when No Additional Input Delay is on.


## 11. v1.5.0 - reaching the batched path

> **Superseded twice:** the whole-batch culling this section describes was added in v1.5.0 and
> **removed in v1.5.1** after review found two ways it could hide an entire batch permanently
> (section 12 and `SHADER_CULLING.md` §8). v2.0.0 then re-implemented it with both failure
> modes closed by construction, behind the `batch-culling` setting; see section 14. Read this
> section as the history of the first attempt.

The measured history of this mod kept hitting the same wall: GD submits level objects through
`CCSpriteBatchNode`, which draws all of its children in one call and never runs
`CCSprite::draw` for them, so every per-sprite optimization was blind to the bulk of a level.
The Culling page now has a control that addresses that directly.

`Batch culling` (off by default) measures the batch's own extent at draw time: the
axis-aligned box of the vertex buffer that `visit()` filled for this frame, its four corners
transformed through the node's world basis, tested against the viewport plus a margin floor of
64 world units. If the result is entirely outside, the batch's single draw call is skipped -
program activation, attribute pointers, texture bind, the draw and everything it would have
rasterized. No quad is rewritten, nothing is reordered, and simulation is untouched.

The bounds are deliberately conservative. The buffer is scanned in full or not at all under a
fixed per-frame budget of 24,000 quads, so the cost of the feature is capped by that number
rather than by the size of the level, and any batch that cannot be measured exactly is drawn.
All four vertices of every quad are folded into the box, so a rotated child cannot escape it,
and a missing atlas, an empty buffer, a non-finite vertex or a degenerate box all answer
"draw".

Honest status: this is new, it is off by default, and its gain has **not** been measured in a
live level yet - the user asked for code work rather than more test launches. The Culling page
reports `culled / measured / drawn` so the first person to enable it can see exactly how much
of a given level the batch path can remove, instead of taking a claim for it.


## 12. v1.5.1 and v1.5.2 - the batched path removed, one skip brought back safely

Both of these came from a code review rather than from playing the game.

**v1.5.1 removed the whole-batch visibility culling** that v1.5.0 had added (section 11). A
review found two failure modes that could hide an entire batch permanently, and neither can be
ruled out from inside a mod:

* the scan measured `TextureAtlas::getTotalQuads()` slots, but the engine can submit more
  entries than that - children added via `addSpriteWithoutQuad`, and stale slots past the count
  from removed objects - so a visible batch can fall outside the measured box;
* in cocos2d-x 3 and later the batch node assembles its vertex buffer inside `draw()`, so
  skipping that `draw()` also skips the rebuild and the next frame measures a frozen buffer - a
  batch whose children have since scrolled into view can stay culled indefinitely.

Settling it needs an in-game test on a level with off-screen batches, which has not been done,
so the setting, its counters and its panel control are gone. **The empty-batch skip remains** -
a batch with zero quads cannot hide anything. v1.5.1 also **hardened the transform-basis cache
against pointer reuse**: a node freed and reallocated at the same address within one frame would
previously have been handed the old occupant's world basis, so each cache entry now also carries
the parent pointer and the node position, and the one-entry sibling memo carries the parent.
Finally it **narrowed the sprite zero-opacity skip** back to the node's own opacity.

**v1.5.2 reintroduced the inherited-opacity skip, safely.** A sprite whose `getDisplayedOpacity()`
is 0 is skipped only when `getBlendFunc().src == GL_SRC_ALPHA` - the case where the blend equation
itself multiplies the source by its alpha, so a fully transparent node provably cannot paint.
Premultiplied blends (`src == GL_ONE`) are drawn, because a program that ignores vertex alpha
could still paint there. See `SHADER_CULLING.md` section 6. The node's-own-opacity, zero-scale and
empty-size skips are unchanged.

## 13. v1.6.0 - Low Latency (drain GPU queue), and a correction about Minimum

**The feature.** A new `low-latency` setting (bool, default off, Windows) calls `glFinish()` once
the frame has been submitted, so the driver cannot keep several frames queued ahead of the display
and a click is not stuck behind them. It is counted in `lowLatencyDrains`, shown on the Input page
("Frame order: deferred / scheduler / **drains**") and in the Overview pipeline row ("low
latency"), and `CMakeLists.txt` now links `opengl32` on Windows. In plain terms it **LOWERS
input-to-screen delay but can REDUCE average frame rate**, because the CPU now waits for the GPU
every single frame. It does not touch physics, timing or the game's FPS cap, and no FPS or latency
improvement has been measured for it - v1.6.0 was built and installed, not launched here.

**The correction.** The original [Minimum](https://github.com/ziyyhhk/Minimum) (public, MIT,
v3.2.2, geode 5.11.2, GD 2.2081, win/mac/android/ios) has **no "No Additional Input Delay" setting
and no input hook at all.** That setting - moving mod bookkeeping after the game's scheduler update
and after the scene draw - is **Minimum Reborn's own design**, and it cannot be validated by
reading Minimum; earlier text here implied otherwise and is now fixed. What Minimum *actually*
does for latency is a `low-latency` setting that calls `glFinish()` once per submitted frame to
drain the driver queue ("Input-to-screen latency is mostly queueing ... Calling `glFinish()` once
the frame has been submitted makes the CPU wait until the GPU has caught up"). So this v1.6.0
feature is the **Reborn equivalent of Minimum's real approach** - same author, MIT-licensed,
reimplemented, no code copied. Minimum also touches timing/priority directly (`timeBeginPeriod(1)`,
`SetPriorityClass`, an EcoQoS opt-out, a sleep+spin frame wait, an animation-interval unlock);
Minimum Reborn deliberately does **not** change the game's FPS cap or animation interval, so those
statements about Reborn remain accurate.

**Still unverified for these docs.** v1.5.x and v1.6.0 were built by CI and installed, but not
launched or played here (the owner asked for code work and will test). No measured FPS or latency
number exists for the new `glFinish` mode. The batched level-object path was untouched until
v2.0.0 (see section 14); v2.0.0 has likewise not been launched yet.
And the project remains Windows-only in its metadata (`mod.json` declares `win` only) even though
the original Minimum ships for mac/android/ios. As always, GPL-licensed third-party renderers are
a design reference only - **no code, shaders or assets from them are used here.**

## 14. v2.0.0 - the batched path, done safely, and a particle cull that sees real data

> **Superseded in v2.4.0:** the whole-batch skip described here was replaced by the per-quad
> filter in section 3. A batch is no longer dropped as a whole; individual quads that cannot
> paint are left out while the single GL draw, atlas order and blending stay intact. Read this
> section as the history of the batch-cull attempt.

The two things this project kept circling are now in the build, both fail-open and both
measured by construction.

**The whole-batch cull** (`batch-culling`, bool, **off by default**, on in Performance and
Extreme). GD submits the level through `CCSpriteBatchNode::draw` — one GL call per batch, ~35
batch draws per frame in Stereo Madness — and never runs `CCSprite::draw` for those objects.
When a whole batch's vertex buffer lies outside the viewport plus 64 world units, the hook
returns before the original draw: the program activation, the texture bind and the draw call
itself are all saved. The two failure modes that killed v1.5.0's version are closed with the
engine's own truth instead of assumptions:

1. "The scan measures less than `draw()` submits" — it cannot. The atlas draws
   `getTotalQuads()` quads and nothing else; children registered with `addSpriteWithoutQuad`
   never got an atlas slot and go through their own `CCSprite::draw` path, which this does not
   touch. The scan reads every corner of `[0, total)` and any missing/non-finite data answers
   "draw".
2. "Skipping freezes the VBO and the cull sustains itself" — it cannot. The hook never skips a
   batch whose `CCTextureAtlas::isDirty()` is set (the skipped draw is exactly where the VBO
   upload happens) nor one whose `m_bManualSortAllChildrenDirty` is pending (a sort rebuilds the
   buffer inside the draw). Skipping only ever happens when the buffer that will be drawn is
   byte-identical to the one the previous frame used.

A per-frame scan budget of 24,000 quads caps the CPU cost of the feature — past the budget,
batches draw untested. The margin is 64 world units on top of the viewport. Only `draw()` is
skipped; `visit()`, updates, triggers and object add/remove are untouched. It is counted in its
own `batchCulls` counter; v2.4.0 shows `batch scans`, `batch culls` and `fully culled` on the
Detailed overlay.

**The particle cull** now measures *live particle data* instead of a worst-case reach cone.
Every frame it reads the emitter-local positions the draw will submit from `m_pParticles`,
expands them by particle size, and tests the resulting AABB — mapped to the world as a
rotation-invariant circle — against the viewport. Relative and Grouped systems are both
covered. The old cone could only cull when every possible future position was off screen; the
new test culls exactly when no particle is visible now. Free systems (absolute frame) remain
always-drawn. The emitter-on-screen shortcut still costs one circle test.

**Profiles**: when this was written, Performance and Extreme turned `batch-culling` on and
Balanced kept it off. v2.4.0 turns it on in Balanced as well, and the setting can still be
turned off.

**Not measured**: v2.0.0 has not been launched. The design is argued from the cocos2d-x 2.2
headers this binary was built against — which is why the skip is bounded and fails open — but
nobody has watched a level render through it yet. The counter to check on the first run is
`batch scans` / `quads culled` on the Detailed overlay: they should move only when quads are
actually off screen, and the level must look intact. If it does not, turn `batch-culling` off;
the rest of the mod is unchanged. No FPS gain is claimed for it; the claim is that off-screen
level chunks stop paying their submission, plus the physics/inputs/cap claim that holds for
every version of this mod.

## 15. v2.1.0 - the suppression pass: program binds and off-screen particle simulation

> **v2.4.0 note:** the program-bind part below was retired. cocos2d already caches binds in
> `ccGLUseProgram`, so the extra query was redundant; `shader-state-cache` is now an inert
> legacy key. The particle-suppression feature is unchanged.

Two more opt-in systems, both default off, both counted:

**Skip Redundant Program Binds** (`shader-state-cache`, Windows). Every program bind in the game
goes through `CCGLProgram::use()`; the hook validates the *live driver state*
(`glGetIntegerv(GL_CURRENT_PROGRAM)`) before calling the original, and skips the `glUseProgram`
when the program is already bound. No cache is kept, so a bind that happens anywhere outside the
hook is picked up by the very next `use()` — the classic stale-cache corruption class cannot
occur here. `programBindsSkipped` counted the skips and showed on the Detailed overlay while the feature
shipped; it is gone in v2.4.0. Honest
frame: one driver query per candidate skip in exchange for not re-binding; whether the exchange
wins on your machine was never measured, which is why it shipped off by default.

**Suppress Off-Screen Particle Simulation** (`particle-suppression`). v2.0.0 stopped the GPU
work for off-screen particles but kept the CPU simulation running. This hook skips
`CCParticleSystem::update` for systems whose live particles are fully outside the expanded
viewport (the same live-particle AABB as the draw cull; Relative and Grouped only). Emission,
integration and the quad rebuild do not run. Particle state freezes off screen and resumes with
the next frame's normal dt when it scrolls back into view — no backlog burst, nothing deleted
mid-frame, no cocos2d state corruption path. `particleSimsSuppressed` counts the suppressed
updates; the Detailed overlay shows it.

**Evaluated and not done:**

* Skipping update for off-screen unbatched sprites — cocos2d has no per-node update worth
  gating (actions/animations run through the global `CCScheduler`/`CCActionManager`); freezing
  them desynchronizes animations everywhere. Not compatible with this mod's timing rule.
* Grouping/reordering draws by program or texture — z-order and blending make it unsafe;
  correctness beats batching.
* Static-content caching — no invalidation signal a mod can hook.
* Shader precompilation at load — `CCShaderCache` already preloads the standard programs.

**Settings map** (the owner's master prompt asked for `enableCulling`, `particleBudget`,
`particleCullMode`, `shaderStateCache`, `drawOptimizations` plus a master toggle; the existing
settings already fill those roles and renaming would break saved user settings):

| Requested | Lives here | Default |
|---|---|---|
| master toggle | `enabled` | on |
| enableCulling | `cull-offscreen-particles` + `particle-cull-mode`, `scene-cull`, `batch-culling`, `editor-culling` | on for particles/batch, off for scene |
| particleBudget | `cap-particles` / `particle-cap`, `spawn-budget` | cap 128, budget off |
| particleCullMode | `particle-cull-mode` (geometry mode) + `particle-suppression` (full suppression) | Conservative, off |
| shaderStateCache | `shader-state-cache` | retired in v2.4.0 (inert key) |
| drawOptimizations | `sprite-fast-path`, `skip-idle-particles`, `batch-culling` | on / on / on |
| customShaderCull | `custom-shader-filter` (fragment-only programs verified against the stock vertex stage) | on |

**Verification**: CI green (Windows, Geode 5.11.2). Not launched in-game; FPS impact: **not
measured**. The two new hooks are build-clean and their Windows import symbols are present in
`libcocos2d.dll` (`?use@CCGLProgram@cocos2d@@QEAAXXZ`,
`?update@CCParticleSystem@cocos2d@@UEAAXM@Z`), but hook *installation* happens at runtime and
has not been exercised here — treat them as unverified until the first launch shows them on the
Detailed overlay.

## 16. v2.3.1 - the GPU batch filter (experimental, off by default)

The per-quad batch filter (section 3) decides, on the CPU, which quads out of a batched level
draw cannot produce a pixel, and leaves them out of the index list. `gpu-batch-cull` runs that
exact same decision on the GPU with a GL 4.3 compute shader, for batches larger than 1024
quads, and submits the result with `glDrawElementsIndirect`. No code from any other renderer
was used; the shader and the index pipeline are written for this mod.

What changes and what does not:

* **No copy of the vertex data, no readback.** The atlas vertex VBO is bound directly as a
  read-only shader storage buffer - the same bytes the original draw would upload. Each compute
  invocation projects one quad (screen-space MVP, `w > 1e-6`, NaN fails open to keeping the
  quad), drops it when its four corners lie fully outside the clip volume plus the same 0.005
  NDC margin the CPU filter uses or when its area is below the same epsilon, and otherwise
  appends its six indices to an output buffer while atomically adding 6 to the draw count
  stored in the indirect command. `glDrawElementsIndirect` then reads the count from that
  command, so the CPU never learns how many quads survived - when everything is culled the
  command carries count 0 and the draw is a no-op. If you want exact culled/kept numbers,
  keep using the CPU path; the GPU path reports only its own dispatch and scanned-quad
  counters.
* **The batch keeps its single GL draw, blend state and atlas order.** Kept indices point at
  the same vertices in the same sequence (tl, bl, tr, br, tr, bl), and the atlas's own index
  buffer is never written. Draw order, blending, glow, clipping, z-order, shaders and the
  dirty upload are exactly as in the CPU path.
* **Every gate still runs on the CPU first.** The child `updateTransform` pass, the grid
  check, the stock-or-verified-passthrough program rule, the screen-space projection test, the
  16,383-quad cap and the 65,536-quad per-frame scan budget all apply before the GPU path is
  considered. The GPU path only replaces the scan and the index emission.
* **Fail-open everywhere.** GL older than 4.3, a missing entry point (cocos ships a GLEW
  limited to GL 4.2; the 4.3 functions are resolved at runtime via `wglGetProcAddress`, and the
  rest via `glew32.dll`'s `__glew*` exports), a shader that fails to compile or link, or a
  non-screen-space projection all return "unavailable" before anything is drawn; the CPU
  filter then runs instead. The first failure latches for the process lifetime, so a bad driver
  disables the path permanently rather than faulting every frame.
* **1024-quad switchover.** One compute dispatch plus one memory barrier costs more than a
  few hundred index-list passes ever do on the CPU, so small batches stay on the CPU filter.
  The threshold is a static constant, not a setting; it trades a small misprediction in either
  direction for never making small batches slower.

Honest status:

* **Not measured in-game.** Nobody has watched a level render through the compute path. The
  Detailed overlay reports `gpu batches` and `gpu quads` so first runs can confirm dispatches
  happen and quads get scanned; there is deliberately no culled-count readback, so the visual
  A/B (`gpu-batch-cull` on/off with everything else identical, `batch-culling` on) is the
  check that matters.
* **It is off by default and by any profile**, because it is unverified and because its target
  - very large batches in shader-heavy levels - is exactly the case this mod has never
  benchmarked. The CPU filter remains the default everywhere.
* **Nothing about it changes physics, input, the FPS cap, draw order or blending.** If it ever
  looks wrong, turn it off; the CPU path is the documented fallback and is unaffected.
* **Relationship to third-party renderers.** The compute-culling idea is the same
  architecture as the GPL-licensed renderers that exist for this game. They were used as a
  design reference only: no code, shaders or assets from any of them are included, and
  nothing here plugs into or patches any other mod.

**Version note.** v2.3.1 is the first release since v2.3.0 and ships everything that existed
on `main` as v2.4.0 and v2.4.1 (the per-quad batch filter, the custom-shader filter, the
tighter live-particle world-rectangle cull, the 65,536-quad scan budget) plus this GPU path.
The 2.4.x line was never published, so the release sequence continues from 2.3.0. The version
2.4.1 was set to 2.3.1 deliberately.

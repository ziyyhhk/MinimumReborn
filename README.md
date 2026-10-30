# Minimum Reborn

A rendering-focused optimization mod for Geometry Dash 2.2081, built with C++20 and the
Geode SDK. It is a successor to [Minimum](https://github.com/ziyyhhk/Minimum), with the
render path as the main subject rather than a settings collection.

**Status: v1.2.1, built by GitHub Actions and run in Geometry Dash on a Windows laptop.**
The runtime evidence in this README comes from that machine, not from estimates. It also
changed the design: GD batches its level objects, so the per-sprite scene cull cannot see
them. Scene culling is now opt-in and off in the default profile, and this file says so.

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

### 3. Particle system fast paths

* **Idle systems.** `CCParticleSystemQuad::draw` binds a texture and submits a draw even with
  zero live particles. Idle systems are skipped. Simulation is not touched.
* **Off-screen systems.** A relative-position system whose worst-case reach (speed × life,
  acceleration, start/end radius, position variance, particle size, scaled to world units)
  does not touch the expanded viewport is not drawn. It still updates, so emission timing and
  motion are unchanged when it returns. Free and grouped systems are never culled because no
  safe bound exists for them. If the emitter itself is on screen the reach bound is not
  computed at all: one circle test decides.
* **Measured:** Stereo Madness exposes **one particle system per frame** to the hook and it
  stayed on screen for all 601 frames, so 0 draws were skipped. The hook and its counters are
  exercised; this level simply has no off-screen effects. Effect-heavy levels are where the
  skip can fire, and the benchmark reports it per run so it is not taken on faith.
* **Pool cap.** The requested pool size is clamped *before* the original
  `initWithTotalParticles` runs, so the particle, quad and index buffers are allocated for the
  capped size. Nothing can index past its buffer. Affects systems created after the change.
* **Adaptive cap (off by default).** While drawn frames stay slower than the Target FPS, the
  pool is stepped down (never below 24) and logged; it relaxes back when frames recover.
* **Spawn budget (off by default).** After N systems are created in one frame, further
  systems get a pool of 4: bursts look thinner instead of stalling the frame.

### 4. Render gate (experimental, off by default)

A `CCDirector::drawScene` hook that can skip *rendering* while the scheduler keeps its normal
rate. A skipped frame runs one scheduler update with the delta the main loop already computed;
it does not call `calculateDeltaTime()`. Physics, collision, input processing and the FPS cap
are untouched. It only makes sense above the display refresh rate (uncapped FPS), and it
delays what you see by up to one render interval, which is why it is off by default.

While the window is unfocused the same mechanism draws at Background FPS (default 20).

### 5. Windows system tuning

1 ms timer resolution, opt-out of Windows power throttling (EcoQoS), optional process
priority. Applied only when the wanted state differs; undone when switched off.

### 6. Performance overlay and diagnostics

FPS (drawn frames, wall clock), average and p95 frame time, 1% low, scheduler updates per
second, CPU time inside the draw hook, and culling telemetry side by side with what it was
measured against: scene draws skipped *of* tests performed, particle draws skipped *of*
systems examined, invisible-sprite skips, capped pools, logic-only frames. A clearly-labelled
bottleneck *hint* says whether the draw path itself is spending most of the frame. One label,
updated four times a second, only when the text changed. No per-frame nodes. No GPU timer: a
blocking GPU query would itself cost frames, so GPU timing is not claimed. The default corner
is Top Right because GD's attempt counter occupies the top left.

### 7. In-game benchmark

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
Stereo Madness (Normal), an auto-player mod active, in-level FPS cap 60. Four 10 s runs, each
started from the pause panel and resumed immediately; v1.2.0 counted about 1 s of pause screen
at the start, v1.2.1 excludes paused frames.

| Run | Renderer | Frames | Avg | p50 | p95 | 1% low | Draw CPU | Scene culled | Particles skipped | Batches (quads) |
|---|---|---|---|---|---|---|---|---|---|---|
| A | Optimized | 601 | 16.67 ms | 16.34 ms | 17.36 ms | 39 fps | 3.10 ms | 0 / 18,939 | 0 / 601 | 24,671 (231,543) |
| B | Engine | 601 | 16.67 ms | 16.33 ms | 17.36 ms | 46 fps | 2.79 ms | — | — | — |
| C | Engine | 601 | 16.67 ms | 16.33 ms | 17.36 ms | 44 fps | 2.88 ms | — | — | — |
| D | Optimized | 601 | 16.67 ms | 16.33 ms | 17.35 ms | 42 fps | 2.78 ms | 0 / 11,891 | 0 / 601 | 20,898 (211,759) |

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
  node, not in `CCSprite::draw`.
* **Culling fired zero times.** GD's own object management plus batching leaves nothing for
  the per-sprite test. The default now reflects that. Particle culling has no off-screen
  targets on this level either; the counters are there so the claim can be checked on levels
  that do.
* Where the mod can still help: uncapped/high-refresh rendering, batching-heavy custom
  content seen through the sprite path, particle spam, and background throttling. Those are
  not demonstrated by the table above.

## What is NOT implemented

These were considered and deliberately left out rather than faked:

* **Replacing GD's batching.** GD already draws level objects through `CCSpriteBatchNode`
  layers (measured above). Merging those batches across textures would require reordering
  translucent objects and rebuilding texture atlases; a partial version that silently changes
  draw order was judged more dangerous than valuable. Bismuth (GPL-3.0-or-later, with an
  additional restriction on generative-AI level generation/modification) was studied only as
  a design reference; **no Bismuth code, shaders or assets are used here.**
* **Dynamic resolution / render scale.** Cocos2d-x 2.2 in this game exposes no safe
  render-target resize path through the SDK; changing window or viewport size is not an
  equivalent and was not attempted. Documented as future work.
* **GPU timers.** No reliable non-blocking timer is exposed; omitted rather than faked.
* **Hard GPU sync (`glFinish`) low-latency mode.** It trades throughput for latency and was
  not part of this build. Minimum Reborn adds no input-path code at all instead.
* **Automatic quality presets by frame time.** The only automatic adjustment is the opt-in
  particle cap (off by default), which changes pool allocation, never simulation.
* **A "disable glow" switch.** GD's glow is part of the batched level path, so neither the
  sprite fast-out nor the scene cull can skip it, and a toggle would remove visuals without
  removing the batch submission. Not added.

## Settings and profiles

Profiles (`Custom`, `Default`, `Balanced`, `Performance`, `Extreme`) write their values into
the individual settings once; editing any setting switches the profile to Custom. `Balanced`
is the default. A profile also selects the renderer mode and both cull modes:

| Profile | Renderer | Scene cull | Particle cull | Particle cap | Background FPS |
|---|---|---|---|---|---|
| Default | Engine | Off | Off | 256 | 20 |
| Balanced | Optimized | Off | Conservative | 128 | 20 |
| Performance | Optimized | Conservative | Conservative | 64 | 15 |
| Extreme | Optimized | Aggressive | Aggressive | 32 | 10 |

The full list is in `mod.json` and appears in Geode's native settings page, which also
provides reset. The pause menu has a compact panel with the same features, live numbers,
Render/Scene/Particles mode buttons that cycle in place, Reset, Benchmark, and a shortcut to
the native page.

Settings are cached in a snapshot refreshed twice a second and immediately after a UI change.
Hot paths never look up setting keys.

## Input responsiveness

Minimum Reborn **adds no input hook, queue, buffer, lock, thread or sleep**. The game reads
input on the main thread exactly as before. The render gate never postpones input processing:
on a skipped render frame the scheduler still runs. No physics or timing values are modified.

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

* Workflow runs for v1.2.0 (41011043806) and v1.2.1 are green on Windows and upload
  **MinimumReborn-Windows**. The v1.2.0 package was downloaded and inspected: `mod.json`,
  `about.md`, `changelog.md`, `.geode_cache`, and `ziyyhhk.minimum-reborn.dll` (208,384 bytes,
  Release, LTO). Archive size 84,318 bytes packed / 236,025 bytes unpacked. The small size is
  the point: one DLL plus text; cocos2d, Geode and the C++ runtime are provided by the game
  install, not bundled. Every source file in `CMakeLists.txt` is compiled; nothing is inflated
  to look substantial.
* Earlier runs are visible in Actions; run 41008194965 failed to compile
  (`CCSprite::getBoundingBox` is not exposed by the bindings) and was fixed in 41008425934.

Runtime evidence (this laptop, details and numbers above):

* The mod loaded with all hooks enabled in the Geode log, the pause panel opened, the overlay
  drew, the Benchmark completed four times, and its numbers came from the same frame timing
  the overlay uses. The panel and overlay were checked visually; the log lines quoted in this
  README are from the run.
* **Not measured:** other levels, other hardware, uncapped FPS, GPU time, long sessions,
  visual A/B against screenshots, input-to-photon latency. The culling's visual correctness is
  argued from the fail-open test and the fact that it skipped zero draws in the measured
  level; it was not exercised with a non-zero cull count in GD.
* The skip paths are written to fail open. If the viewport snapshot is missing or
  non-finite, every visibility test answers "draw". A sprite is rejected only when its whole
  quad is outside the expanded viewport; a particle system only when it is relative-positioned
  and its worst-case reach misses the expanded viewport. Idle-system and zero-pixel skips can
  only fire where the original draw would submit geometry covering no pixels.

### Benchmark procedure

1. Same level, game version, resolution, graphics settings, FPS cap, background workload.
2. Pause the level, open the Minimum Reborn panel (MR button), press **Bench**, close the
   panel and resume. Recording starts on the first unpaused frame; it ends after the
   configured duration (10 s by default, 3–60 s).
3. Do at least three runs per configuration and report the range. Runs cover different
   attempts as the auto-player progresses; that is visible in the batch/quad counts.
4. Compare `Engine` vs `Optimized`, then `Conservative` vs `Aggressive` culling. Watch
   average FPS, average frame time, p95, 1% low, draw CPU time, and the scene/particle
   counters together: a mode that culls more draws but does not move frame time is not
   helping on that level.
5. For longer sessions use the overlay in Detailed mode. The benchmark summary is also
   written to the Geode log.

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
particle techniques and the render-gate scheduling approach, both reimplemented for this
project. No GPL code from Bismuth is included.

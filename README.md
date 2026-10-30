# Minimum Reborn

A rendering-focused optimization mod for Geometry Dash 2.2081, built with C++20 and the
Geode SDK. It is a successor to [Minimum](https://github.com/ziyyhhk/Minimum), with the
render path as the main subject rather than a settings collection.

**Status: v1.1.0 source and Windows build verified on GitHub Actions** (run 41008735369).
The `.geode` is built by the workflow in this repository. See "Verification" below for exactly
what has and has not been proven — in particular, **no in-game performance numbers have been
measured yet**, and the build has not been loaded into Geometry Dash from this machine.

## What it does

Minimum Reborn changes real execution paths in the draw traversal. Each feature can be turned
off; when it is off, the original path is taken unchanged. There are two renderer modes:
**Optimized** (the mod's draw path) and **Engine** (every per-node hook calls straight through
to the stock cocos2d function; only frame-level measurement runs). The Default profile
selects Engine.

### 1. Sprite draw fast-out

`CCSprite::draw()` is called for every visible sprite in a level — thousands of times per
frame. Some of those sprites cannot produce a single pixel: opacity 0, scale 0 on one axis,
or an empty content size. The mod returns before the draw call for exactly those cases.

* Original path: `CCNode::visit` → `CCSprite::draw` → program/texture setup → GL draw.
* Replacement: three field reads and a return.
* Visual result: identical under the game's standard alpha and additive blending (a zero-alpha
  quad contributes nothing); children are drawn by `visit()` independently of the parent's
  `draw()`.
* Cost: one branch per sprite when enabled.

### 2. Scene object culling

A sprite whose whole quad lies outside the viewport plus a margin cannot contribute a pixel,
so its draw call is skipped. This covers decorations, glow sprites and player trails, because
all of them are drawn through `CCSprite::draw`.

* Once per frame the viewport is snapshotted from `CCDirector`; the parent's world basis
  (origin and two unit vectors) is computed once per parent and cached for the rest of the
  frame. In a level thousands of objects share one parent, so the expensive
  `convertToWorldSpace` walk is paid a handful of times per frame.
* The sprite is reduced to a circle around its node position with radius = full content
  diagonal × scale. That covers the quad for any anchor point, any rotation and any scale,
  so the test cannot reject a partly visible sprite. It is deliberately looser than a
  bounding-box test.
* The circle is tested against the viewport expanded by the margin (160 units conservative,
  16 aggressive), which absorbs one frame of camera movement.
* Any doubt — no parent, non-finite transform, empty content size — answers "draw".
* Only `draw()` is skipped. `visit()` still runs, so updates, animations, triggers, children,
  collision and physics are untouched.

### 3. Particle system fast paths

* **Idle systems.** `CCParticleSystemQuad::draw` binds a texture and submits a draw even with
  zero live particles. Idle systems are skipped.
* **Off-screen systems.** A relative-position system whose worst-case reach (speed × life,
  acceleration, start/end radius, position variance, particle size, scaled to world units)
  does not touch the expanded viewport is not drawn. It still updates, so emission timing and
  motion are unchanged when it returns to the screen. Free and grouped systems are never
  culled because no safe bound exists for them. If the emitter itself is on screen the reach
  bound is not computed at all: one circle test decides.
* **Pool cap.** The requested pool size is clamped *before* the original
  `initWithTotalParticles` runs, so the particle, quad and index buffers are allocated for the
  capped size. Nothing can index past its buffer. Affects systems created after the change.
* **Adaptive cap (off by default).** While drawn frames stay slower than the Target FPS, the
  pool is stepped down (never below 24) and logged; it relaxes back when frames recover.
* **Spawn budget (off by default).** After N systems are created in one frame, further
  systems get a pool of 4: bursts look thinner instead of stalling the frame.

### 4. Render gate (experimental, off by default)

A `CCDirector::drawScene` hook that can skip *rendering* while the scheduler keeps its normal
rate. A skipped frame runs one scheduler update with the delta the main loop already
computed; it does not call `calculateDeltaTime()`. Physics, collision, input processing and
the FPS cap are untouched. It only makes sense above the display refresh rate (uncapped FPS),
and it delays what you see by up to one render interval, which is why it is off by default.

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
blocking GPU query would itself cost frames, so GPU timing is not claimed.

### 7. In-game benchmark

The pause panel's Bench button records drawn frames for the configured duration (default
10 s) and reports average FPS, average/p50/p95 frame time, 1% low, variability, and the
culling deltas over the same window; the same line goes to the Geode log. Recording uses a
fixed 16384-sample buffer (no allocation in the frame path); sorting happens once at stop.
Use it to compare Engine vs Optimized, or Conservative vs Aggressive, on the same level.

## What is NOT implemented

These were considered and deliberately left out rather than faked:

* **Full object batching / replacement renderer.** Geometry Dash draws each object as its own
  draw call. A correct batcher must preserve z-order, additive glow, blending, clipping and
  sprite-frame state. Bismuth (GPL-3.0-or-later, with an additional restriction on
  generative-AI level generation/modification) was studied only as a design reference; **no
  Bismuth code, shaders or assets are used here.** A partial batcher that silently reorders
  translucent objects was judged more dangerous than valuable within this build's scope.
  "Optimized" therefore means a cheaper draw *path*, not a different renderer.
* **Dynamic resolution / render scale.** Cocos2d-x 2.2 in this game exposes no safe
  render-target resize path through the SDK; changing window or viewport size is not an
  equivalent and was not attempted. Documented as future work.
* **GPU timers.** No reliable non-blocking timer is exposed; omitted rather than faked.
* **Hard GPU sync (`glFinish`) low-latency mode.** It trades throughput for latency and was
  not part of this build. Minimum Reborn adds no input-path code at all instead.
* **Automatic quality presets by frame time.** The only automatic adjustment is the opt-in
  particle cap (off by default), which changes pool allocation, never simulation.
* **A "disable glow" switch.** GD's glow objects are ordinary additive sprites, so they are
  already covered by the sprite fast-out and the scene cull — exactly like every other sprite.
  A dedicated glow toggle would only *remove visuals* without skipping work the cull cannot
  already skip, so it was not added.

## Settings and profiles

Profiles (`Custom`, `Default`, `Balanced`, `Performance`, `Extreme`) write their values into
the individual settings once; editing any setting switches the profile to Custom. `Balanced`
is the default. A profile now also selects the renderer mode and both cull modes:

| Profile | Renderer | Scene cull | Particle cull | Particle cap | Background FPS |
|---|---|---|---|---|---|
| Default | Engine | Off | Off | 256 | 20 |
| Balanced | Optimized | Conservative | Conservative | 128 | 20 |
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

Build evidence, nothing more:

* Workflow run [41008735369](https://github.com/ziyyhhk/MinimumReborn/actions/runs/41008735369)
  (commit `104d69d`) completed green on Windows and uploaded **MinimumReborn-Windows**. The
  package was downloaded and inspected: `mod.json` (version `v1.1.0`), `about.md`,
  `changelog.md`, `.geode_cache`, and `ziyyhhk.minimum-reborn.dll` (200,192 bytes, Release,
  LTO). The previous runs are also visible in Actions; run 41008194965 failed to compile
  (`CCSprite::getBoundingBox` is not exposed by the bindings) and was fixed in 41008425934.
* **No in-game run has been performed for this project.** The mod has not been loaded into
  Geometry Dash from here, so neither startup stability nor any FPS or frame-time improvement
  is claimed. The overlay and the in-game benchmark exist so you can measure them on your
  hardware; the numbers they produce are real draw-frame measurements, not estimates.
* The skip paths are written to fail open. If the viewport snapshot is missing or
  non-finite, every visibility test answers "draw". A sprite is rejected only when its whole
  quad is outside the expanded viewport; a particle system only when it is relative-positioned
  and its worst-case reach misses the expanded viewport. Idle-system and zero-pixel skips can
  only fire where the original draw would submit geometry covering no pixels.

### Benchmark procedure

1. Same level, game version, resolution, graphics settings, FPS cap, background workload.
2. Open the pause panel and use **Bench** (10 s by default, configurable 3–60 s). Do at least
   three runs per configuration and report the range.
3. Compare `Engine` vs `Optimized`, then `Conservative` vs `Aggressive` culling. Watch
   average FPS, average frame time, p95, 1% low, and the `scene N of M/s` / `particles N of
   M/s` counters together: a mode that culls more draws but does not move frame time is not
   helping on that level.
4. For longer sessions use the overlay in Detailed mode. The benchmark summary is also
   written to the Geode log.

## Compatibility

* Windows, Geometry Dash 2.2081, Geode 5.10+.
* Do not combine the Render Gate with `mat.draw-divide`; both rewrite the draw cadence.
* Other mods that hook `CCSprite::draw` or particle systems will run alongside this one, but
  stacked draw-skipping has not been tested.
* Scene culling is relative to `CCDirector`'s visible size and origin. It is disabled
  automatically if that snapshot is unusable, and it never touches nodes that are not drawn
  through `CCSprite::draw` (for example batched custom nodes).
* The sprite bound assumes a plain position/rotation/scale transform, which is what GD uses
  for level objects. A sprite with skew set could paint slightly outside the bound; such
  sprites are not expected in levels, and `renderer-mode = Engine` or `scene-cull = Off`
  disables the test entirely.
* Feature toggles exist so conflicts can be isolated without uninstalling.

## License

MIT. Original code by ziyyhhk. Minimum (MIT) was used as a behavioral reference for the
particle techniques and the render-gate scheduling approach, both reimplemented for this
project. No GPL code from Bismuth is included.

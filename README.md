# Minimum Reborn

A rendering-focused optimization mod for Geometry Dash 2.2081, built with C++20 and the
Geode SDK. It is a successor to [Minimum](https://github.com/ziyyhhk/Minimum), with the
render path as the main subject rather than a settings collection.

**Status: source complete, build verification in progress.** See "Verification" below for
exactly what has and has not been proven.

## What it does

Minimum Reborn changes a small number of real execution paths. Each feature can be turned
off; when it is off, the original path is taken unchanged.

### 1. Sprite draw fast-out

`CCSprite::draw()` is called for every visible sprite in a level — thousands of times per
frame. Some of those sprites cannot produce a single pixel: opacity 0, scale 0 on one axis,
or an empty content size. The mod returns before the draw call for exactly those cases.

* Original path: `CCNode::visit` → `CCSprite::draw` → program/texture setup → GL draw.
* Replacement: three field reads and a return.
* Visual result: identical. The quad covers zero pixels; children are drawn by `visit()`
  independently of their parent's `draw()`.
* Cost: one branch per sprite when enabled.

### 2. Particle system fast paths

* **Idle systems.** `CCParticleSystemQuad::draw` binds a texture and submits a draw even with
  zero live particles. Idle systems are skipped.
* **Off-screen systems.** A relative-position system whose worst-case reach (speed × life,
  acceleration, start/end radius, position variance, particle size, scaled to world units)
  does not touch the visible rectangle is not drawn. It still updates, so emission timing and
  motion are unchanged when it returns to the screen. Free and grouped systems are never
  culled because no safe bound exists for them.
* **Pool cap.** The requested pool size is clamped *before* the original
  `initWithTotalParticles` runs, so the particle, quad and index buffers are allocated for the
  capped size. Nothing can index past its buffer. Affects systems created after the change.
* **Spawn budget (off by default).** After N systems are created in one frame, further
  systems get a pool of 4: bursts look thinner instead of stalling the frame.

### 3. Render gate (experimental, off by default)

A `CCDirector::drawScene` hook that can skip *rendering* while the scheduler keeps its normal
rate. A skipped frame runs one scheduler update with the delta the main loop already
computed; it does not call `calculateDeltaTime()`. Physics, collision, input processing and
the FPS cap are untouched. It only makes sense above the display refresh rate (uncapped FPS),
and it delays what you see by up to one render interval, which is why it is off by default.

While the window is unfocused the same mechanism draws at Background FPS (default 20).

### 4. Windows system tuning

1 ms timer resolution, opt-out of Windows power throttling (EcoQoS), optional process
priority. Applied only when the wanted state differs; undone when switched off.

### 5. Performance overlay

FPS (drawn frames, wall clock), average and p95 frame time, 1% low, frame-time variability,
scheduler updates per second, CPU time inside the draw hook, skipped-draw and capped-pool
counters, and a clearly-labeled bottleneck *hint*. One label, updated four times a second,
only when the text changed. No per-frame nodes. No GPU timer: a blocking GPU query would
itself cost frames, so GPU timing is not claimed.

## What is NOT implemented

These were considered and deliberately left out rather than faked:

* **Full object batching / custom renderer.** Geometry Dash draws each object as its own
  draw call. A correct batcher must preserve z-order, additive glow, blending, clipping and
  sprite-frame state. Bismuth (GPL-3.0-or-later, with an additional restriction on
  generative-AI level generation/modification) was studied only as a design reference; **no
  Bismuth code, shaders or assets are used here.** A partial batcher that silently reorders
  translucent objects was judged more dangerous than valuable within this build's scope.
* **Dynamic resolution / render scale.** Cocos2d-x 2.2 in this game exposes no safe
  render-target resize path through the SDK; changing window or viewport size is not an
  equivalent and was not attempted. Documented as future work.
* **GPU timers.** No reliable non-blocking timer is exposed; omitted rather than faked.
* **Hard GPU sync (`glFinish`) low-latency mode.** It trades throughput for latency and was
  not part of this build. Minimum Reborn adds no input-path code at all instead.
* **Adaptive quality switching.** No automatic changes to visual quality based on frame time.

## Settings and profiles

Profiles (`Custom`, `Default`, `Balanced`, `Performance`, `Extreme`) write their values into
the individual settings once; editing any setting switches the profile to Custom. `Balanced`
is the default. The full list is in `mod.json` and appears in Geode's native settings page,
which also provides reset. The pause menu has a compact panel with the same features, live
numbers, Reset, and a shortcut to the native page.

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

Requirements: CMake ≥ 3.21, a C++20 compiler, and the Geode SDK (5.10 or newer) with
`GEODE_SDK` pointing at it.

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

* Source is complete and compiles on GitHub Actions (Windows). Check the latest workflow run
  for the actual result — do not rely on this sentence.
* **No in-game benchmark has been run for this repository yet.** No FPS or frame-time
  improvement figures are claimed. The overlay exists to measure them on real hardware.
* The skip paths are guarded so a mis-detection cannot crash the game: off-screen particle
  culling answers "draw" for degenerate transforms, NaN values, free/grouped systems; sprite
  fast-out only fires for zero-pixel cases.

### Benchmark procedure

1. Same level, game version, resolution, graphics settings, FPS cap, background workload.
2. Record 60 s with the overlay in Detailed mode, first with the mod disabled (master switch),
   then enabled, then with individual features toggled.
3. Compare average FPS, average frame time, p95 frame time and 1% low — not just average FPS.
4. Repeat at least twice; report the range.

## Compatibility

* Windows, Geometry Dash 2.2081, Geode 5.10+.
* Do not combine the Render Gate with `mat.draw-divide`; both rewrite the draw cadence.
* Other mods that hook `CCSprite::draw` or particle systems will run alongside this one, but
  stacked draw-skipping has not been tested.
* Feature toggles exist so conflicts can be isolated without uninstalling.

## License

MIT. Original code by ziyyhhk. Minimum (MIT) was used as a behavioral reference for the
particle techniques and the render-gate scheduling approach, both reimplemented for this
project. No GPL code from Bismuth is included.

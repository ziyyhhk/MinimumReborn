# Changelog

## v1.2.1 (2026-10-30)

Phase 3: first in-game test, and the changes the measurements forced.

* **Benchmark excludes paused frames.** The window now starts on the first unpaused frame, so
  starting from the pause panel no longer records a static pause screen. The log line adds
  invisible-sprite skips.
* **Balanced profile leaves the scene cull off.** Measured 0 culls in 18,939 sprite visits on
  Stereo Madness: GD batches level objects through `CCSpriteBatchNode`, so the per-sprite path
  cannot see them. The cull stays available in Performance/Extreme and in the settings.
* Setting descriptions corrected: renderer mode no longer claims level objects are culled;
  scene culling says what it actually covers and quotes the measurement; benchmark text says
  pause screens are not counted.
* Runtime evidence (Stereo Madness, 10 s): 602 frames in 10.0 s after resume, 1,516 invisible
  sprite draws skipped, 0 scene culls with the cull off, 0/598 off-screen particles, 21,330
  batch draws. 60 FPS cap held; no FPS gain is claimed.

## v1.2.0 (2026-10-30)

Phase 3 instrumentation and UI fixes made before the first run.

* **Batch-path telemetry**: a `CCSpriteBatchNode::draw` hook records batch draw calls and
  quads per frame. It only counts; it never changes what the batch submits.
* **Draw CPU time** in the benchmark (average `drawScene` time) and the batch counts, so a
  run shows where the frame actually goes.
* Pause panel fits the 320-unit design height again (title and footer visible).
* Overlay default corner is Top Right so it no longer sits under GD's close button; Reset
  writes the same corner.

## v1.1.0 (2026-10-30)

Phase 2: real visibility culling and diagnostics.

* **Renderer Mode setting** (`Engine` / `Optimized`). Engine takes the game's draw path
  unchanged for every node; Optimized installs the mod's draw path. Default profile now uses
  Engine, so the stock renderer is always one click away.
* **Scene object culling** (`Off` / `Conservative` / `Aggressive`). Skips the draw call of
  sprites whose whole quad is outside the viewport plus a margin (160 / 16 world units). Uses
  a per-frame parent world-basis cache and a rotation-safe radius; every uncertain case draws.
  Covers decorations, glow sprites and trails because they share the sprite path.
* **Particle culling modes** (`Off` / `Conservative` / `Aggressive`) with a 128 / 8 unit
  margin, plus a center-inside shortcut so visible systems cost one circle test. Simulation is
  never skipped.
* **Frame viewport snapshot and transform cache** rebuilt once per `drawScene`; nothing is
  kept across frames.
* **Telemetry**: examined vs culled counts for scene and particle culling, per second and as
  totals, in the overlay and the pause panel. Counts describe tests and skipped draws, not GPU
  work.
* **In-game benchmark** (pause panel, Bench button): records drawn frames for the configured
  duration and reports average FPS, average/p50/p95 frame time, 1% low, variability, and the
  culling deltas over the same window; also written to the Geode log. Fixed 16384-sample
  buffer, no allocation while recording.
* **Adaptive particle cap** (opt-in, off by default): steps the pool down while drawn frames
  stay slow and relaxes it when they recover. Never touches timing, physics or input.
* Pause panel rebuilt: larger layout, Renderer/Scene/Particles cycle buttons showing the state
  actually in effect, live culling rates, benchmark result line.

## v1.0.0 (2026-10-30)

First build.

* Sprite draw fast-out for sprites that cannot produce a pixel (opacity 0, zero scale,
  empty content size).
* Particle fast paths: idle-system draw skip, off-screen culling with a conservative reach
  bound, pool-size cap applied before allocation, optional per-frame spawn budget.
* Optional render gate: draws at Render FPS / Background FPS while the scheduler keeps the
  game's normal rate. Off by default; experimental.
* Performance overlay with FPS, average/p95 frame time, 1% low, jitter, logic rate, draw CPU
  time, skipped-draw counters and a bottleneck hint. One label, four updates per second.
* Pause menu panel with profile selection, quick toggles, reset and a link to the native
  settings page.
* Windows system tuning: 1 ms timer resolution, power-throttling opt-out, optional priority.
* Settings cached in a snapshot; no setting lookups in draw paths. No input-path code.

# Changelog

## v1.6.1 (2026-10-30)

Follow-ups from a read-only review of the new low-latency hook.

* **The GPU wait no longer leaks into the render gate.** The gate accumulates wall time between
  hooks, so the drain's cost was being charged to the next frame, quietly pulling the effective
  render rate below the requested one and making pacing uneven when both features are on. The
  sample point is now advanced by the measured drain duration. Frame-to-frame time still shows
  the cost, which is correct - it is real.
* **The switch is Windows-only in the panel too.** The drain is compiled into the Windows draw
  path, so the toggle is hidden elsewhere instead of offering a control that cannot act.
* **Low Latency no longer flips the profile to Custom.** It is a per-user preference that no
  profile owns, like the overlay and timer switches.
* **Honest trade-off in the description:** with vsync on, the buffer swap already blocks, so
  `glFinish()` mostly adds a driver flush and wait. The gain is clearest when the game is not
  vsync-bound, and it can cost average frame rate - noticeably on hybrid/Optus laptops.


## v1.6.0 (2026-10-30)

A new opt-in latency mode, and the correction of a premise about the original Minimum that the
earlier docs carried.

* **Low Latency (drain GPU queue)** (`low-latency`, bool, default false, Windows): calls
  `glFinish()` once the frame has been submitted, so the driver cannot keep several frames queued
  ahead of the display and a click is not stuck behind them. This **lowers input-to-screen delay
  but can reduce average frame rate**, because the CPU now waits for the GPU every frame. It does
  not touch physics, timing or the game's FPS cap. It is counted in `lowLatencyDrains`, shown on
  the Input page and in the Overview pipeline row, and `CMakeLists.txt` now links `opengl32` on
  Windows. No FPS or latency number has been measured for it here (built and installed, not
  launched). This is the Reborn equivalent of the original Minimum's real approach to latency -
  its own `low-latency`/`glFinish` setting (MIT, same author), reimplemented, no code copied.
* **Premise corrected**: inspecting Minimum showed it has **no** "No Additional Input Delay"
  setting and no input hook. That setting - moving mod bookkeeping after the game's scheduler
  update and after the scene draw - is Minimum Reborn's own design, not something inherited from
  or validated by Minimum. Earlier text that implied otherwise is fixed in the README and the
  development report.

## v1.5.2 (2026-10-30)

Reintroduces one skip that v1.5.1 had narrowed away, this time with a guard that makes it
provably correct. A review change, not a measured one.

* **Inherited-opacity skip, safely.** A sprite whose `getDisplayedOpacity()` is 0 is now skipped
  - but only when `getBlendFunc().src == GL_SRC_ALPHA`, the case where the blend equation itself
  multiplies the source by its alpha, so a fully transparent node cannot contribute a pixel.
  Premultiplied blends (`src == GL_ONE`) are still drawn, since a program that ignores vertex
  alpha could paint there. The node's-own-opacity and zero-scale/empty-size skips from v1.5.1 are
  unchanged. See `SHADER_CULLING.md` section 6.

## v1.5.1 (2026-10-30)

A feature removed and one skip narrowed, both from reviewing the code rather than playing the
game. `SHADER_CULLING.md` section 8 has the reasoning.

* **Whole-batch visibility culling removed** (added in v1.5.0, off by default). Review found
  two failure modes that hide entire batches: the scan covers `getTotalQuads()` slots while the
  engine can submit more entries than that (children added without a quad, stale slots past the
  count), and if the fork assembles the vertex buffer inside `draw()` then skipping that draw
  also skips the rebuild, so a culled batch can stay culled forever. Settling it needs an
  in-game test on a level with off-screen batches; an unmeasured gain did not justify shipping
  a possible invisible-level bug, so the setting, counters and panel control are gone. The
  empty-batch skip stays - it cannot hide anything.
* **Sprite zero-opacity skip narrowed back to the node's own opacity.** The
  `getDisplayedOpacity()` variant added in v1.4.0 is unsafe for a program that reads only the
  vertex color's rgb, or for additive blending on a non-premultiplied texture, which can still
  paint with vertex alpha 0. The narrower condition is the one observed in-game.
* **Transform cache hardened against pointer reuse**: a node freed and reallocated at the same
  address inside one frame would have been handed the previous occupant's world basis; entries
  now also carry the parent pointer and node position, and the sibling memo carries the parent.
* **Panel**: the Culling page keeps the spawn-budget cycle (a real setting that had no control
  before v1.5.0) and the batch row reports empty-skipped versus drawn again.


## v1.4.0 (2026-10-30)

Phase 5: the control panel rebuilt on one layout system, and an honest account of what the
draw path actually skips.

* **One spacing system.** The panel is laid out from a single set of constants (margin,
  header, nav, row pitch, footer). Header, tabs, content rows and footer share the same
  left/right edges and baselines instead of each control carrying its own coordinate.
* **Header**: the MK icon at 26 px, the title, a "Performance and rendering" subtitle, the
  active profile and GD's own close button. The profile block stops short of the close
  button, which is why the previous build clipped it.
* **Navigation**: six equal-width tabs with no background plates. The selected tab is
  brighter, slightly larger and carries a green accent bar on the shared baseline, so the
  selected state is obvious without the blue rectangle that made one tab look like a
  different widget from the other five.
* **Controls**: consistent pill buttons (same height, same cap insets), native togglers with
  their label on the same baseline, and full-width cycle buttons that step the real setting.
* **Diagnostics page** is now a labelled table: `Frame`, `CPU draw time`, `State`, per-second
  culling rates, each with its own value column and unit. Values with no samples answer `--`.
* **Nine-slice fix**: cap insets are read from the texture the engine actually loads instead
  of being hard-coded. A hard-coded inset produced a stretched right edge and a thin bar
  sticking out of every pill, because the texture size at runtime is not the size the source
  file suggests.
* **Popup background** stays on GD's own popup frame. Geode's default (`GJ_square01.png`)
  turned out to be the brown level-info panel, which is why the previous build looked like a
  different UI language.
* **Hot-path counters**: the draw hooks no longer perform a locked atomic increment per
  sprite. They accumulate into plain integers and publish once per frame, so the cost of
  measuring the optimization does not scale with the object count.
* **Squared visibility test**: the sprite cull compares squared distance against squared
  radius, removing a `hypot` from the hottest path in the mod.
* **Particle fast-out** for zero opacity and zero scale, counted separately from the idle and
  off-screen skips.
* **Rendering page** now exposes the adaptive particle cap toggle that already existed in the
  settings but had no control in the panel.
* **Flat shapes come from a mod-owned 16x16 solid sprite.** Stretching GD's trimmed square and
  `CCLayerColor` were both tried and both produced artifacts; see the report for the evidence.
* **Second entry point**: the panel button is installed on `EditorPauseLayer` as well, so the
  editor counters are reachable while the editor is the active scene.
* **Verified live**: all six pages, tab switching, profile chips (Default and Balanced
  applied), toggles, renderer cycle, benchmark start/stop, the Reset confirmation and the
  All Settings hand-off. Engine vs Optimized A/B numbers are in `DEVELOPMENT_REPORT.md`
  section 27 - no FPS gain at the 60 cap, +0.07 ms CPU per frame, 1,830 zero-pixel draws
  skipped per 10 s.

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

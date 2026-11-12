# Changelog

## v2.3.1

First release since v2.3.0. Ships the v2.4.0 and v2.4.1 work (which was pushed to `main` but
never released or tested) plus a new experimental GPU path. Not verified in-game; no FPS claim
is made. The version jump from 2.4.1 to 2.3.1 is deliberate.

* **GPU Batch Filter (experimental)** (`gpu-batch-cull`, bool, Windows, **off by default**,
  requires `batch-culling`). The per-quad batch filter decision moved to the GPU: a GL 4.3
  compute shader reads the atlas vertex VBO as a read-only shader storage buffer (the same
  bytes the draw would upload, never a copy), projects each quad with the screen-space MVP,
  and appends the surviving indices to an output buffer while atomically incrementing the
  count stored in a `glDrawElementsIndirect` command. No GPU result is ever read back by the
  CPU; when every quad is culled the command carries count 0 and the draw is a no-op.
  Batches larger than 1024 quads use the GPU path; smaller ones use the CPU filter, and any
  failure - pre-4.3 context, missing entry points, shader build error, non-screen-space
  projection - falls back to the CPU filter with nothing drawn twice. All existing gates
  (updateTransform pass, grid, stock-or-verified-passthrough program, screen-space
  projection, 16,383-quad cap, 65,536-quad per-frame budget) still apply. No code or shaders
  from any third-party renderer are included; the shader and index pipeline are written for
  this mod. Off by default and in every profile because it is unverified and because small
  batches would be slower (one dispatch outweighs the CPU scan). Detailed overlay gains
  `gpu batches` / `gpu quads` counters.
* **Everything from v2.4.0, v2.4.1, unreleased at the time:** per-quad filtering inside
  `CCSpriteBatchNode::draw` (hidden objects collapsed to zero area and fully off-screen quads
  are left out of the index list; single GL draw, blend state and atlas order preserved; all
  gates fail open), Custom Shader Filter (fragment-only programs verified byte-identical
  vertex stage to stock), tighter live-particle cull (world-rectangle AABB mapped through the
  emitter basis), scan budget raised to 65,536 quads per frame, `shader-trace` GD-side CPU
  timings, and retirement of the redundant `shader-state-cache` rebind dedup.
* `benchmark-seconds` stays inert; GPU 4.3 entry points are resolved at runtime (cocos's
  GLEW stops at GL 4.2); a failed GPU path latches off for the session.

## v2.4.1 (unreleased; shipped as part of v2.3.1)

An iteration of the shader path, aimed at sessions where shaders, deco and triggers push the
frame hard. It is not verified in-game; no FPS claim is made.

* **Custom Shader Filter** (`custom-shader-filter`, bool, **on by default**). Fragment-only
  shader effects - the shape GD's shader library uses - keep the vertex pass-through, so their
  vertices are still where the stock draw would put them. The batch filter now also accepts a
  batch program whose vertex stage is verified byte-identical to the stock PositionTextureColor
  program, instead of bailing to the original draw. Verification happens per frame on Windows
  and is fail-open: any mismatch, ambiguity or doubt draws untouched. A custom vertex stage
  that can displace geometry (waves, shockwaves, lens distortions) is never filtered. The new
  key is additive; existing settings stay stable.
* **Tighter live-particle cull.** The particle draw cull no longer bounds the live particle AABB
  by a bounding circle; the world-space rectangle is computed from the four corners and tested
  against the expanded viewport - the same test the sprite fast-out uses, and strictly tighter
  than the circle, so more off-screen systems are skipped with no visual change.
* **Scan budget raised from 24,000 to 65,536 quads per frame.** A dense shader level can exceed
  the old cap, which used to silently draw untested; the new cap covers the GD object limit.
* This build has not been released or tested in-game.

## v2.4.0 (unreleased; shipped as part of v2.3.1)

This release makes the optimized renderer reach the path Geometry Dash actually uses for level
objects. Until now the per-sprite cull examined ~19k sprites and skipped none, because GD
submits level objects through `CCSpriteBatchNode`. v2.4.0 filters inside that batch draw
instead.

* **Per-quad batch filtering** (`batch-culling`, bool, **on by default**, also in Balanced).
  GD's visibility pass hides an object by collapsing its four vertices to `(0,0,0)`, but the
  batch still uploads and shades every hidden quad. The mod now rebuilds only the index list
  for `CCSpriteBatchNode::draw`: zero-area quads and quads whose four projected corners lie
  fully outside the clip volume (plus a small margin) are left out. The batch keeps its single
  GL draw, its blend state, its atlas order and its normal vertex upload, so kept quads are
  byte-identical and overlapping objects blend in the same sequence.
* **Safety gates, all fail-open.** Only the stock `PositionTextureColor` program is filtered
  (a custom shader can move vertices); an active grid, a pending child sort, a non-screen-space
  projection, non-finite coordinates, more than 16,383 quads or an exhausted per-frame budget
  all fall back to the original draw. The child `updateTransform` pass still runs first and the
  quad count is re-read afterwards because that pass can grow the atlas. The filtered list is
  the mod's own client-side index array; the atlas index buffer is never rewritten, and the
  vertex VBO receives only the same full-range dirty upload the original draw performs.
  When no quad survives, the GL submission is skipped and the dirty flag is left set for the
  next normal upload.
* **Balanced now turns the batch filter on.** Scene culling stays off because the batched path
  is where level objects live; the filter is bounded by the 24,000-quad scan budget, so it is a
  safe default rather than an opt-in extra.
* **`shader-state-cache` retired.** cocos2d already routes `CCGLProgram::use` through
  `ccGLUseProgram`'s state cache (`CC_ENABLE_GL_STATE_CACHE`), so the extra check - and its
  per-bind `glGetIntegerv` - was redundant. The setting key remains in `mod.json` as an inert
  legacy key; the counters it used to drive are gone.
* **Shader trace now reports GD-side CPU time.** In addition to program-bind totals, the
  `gd-trace` log line reports average/max wall time for `PlayLayer::updateVisibility`,
  `LevelEditorLayer::updateVisibility`, `GJBaseGameLayer::updateShaderLayer`,
  `ShaderLayer::visit` and the batch scan. This is how the next measurement separates
  visibility CPU, shader-layer CPU and GPU cost on a level like The Jawbreaker.
* **Counters and overlay**: new `batchCulls` (batches with at least one quad removed),
  `batchFullyCulled`, `batchScans` and `batchQuadsCulled`. The Detailed overlay shows quads
  culled, batch scans and fully culled batches, and still reports trace binds.
* **Not measured.** No in-game launch, FPS or 1% low number exists for v2.4.0 yet, and the
  Jawbreaker `shader-trace` log has not been captured. The release claims only what the code
  does; performance claims stay pending until the owner runs it.

## v2.3.0

The custom panel is deleted. This release is the answer to "the UI is unusable": every
setting lives on the native Geode settings page, reached from the pause menu's MR button
(the button opens it directly; no popup of ours exists anymore).

* **Custom control panel removed** (`src/ui.cpp` rebuilt): `RebornPopup`, the tab pages,
  the segmented profile control, the checkbox component, the footer buttons, the header
  strip, the ledger rows and `DESIGN.md` are gone. The remaining UI surface is the corner
  performance overlay (`StatsHud`) and the MR button.
* **MR button now opens the native settings page** (`openSettingsPopup`), so every setting -
  profile, culling, batch cull, shader cache, suppression, trace, GPU frame time - is edited
  on the same page Geode renders, with its own reset.
* **GPU frame time stays** (`gpu-frame-time`, bool, default on, Windows): `GL_TIME_ELAPSED`
  around the engine draw, read back one frame late non-blocking; shown as `gpu` on the
  Detailed overlay. **Not measured in-game yet.**
* **Benchmark removed** with the panel: `src/benchmark.cpp`, `src/benchmark.hpp` and the
  per-frame `benchmarkNoteFrame` call are gone. `frameStats()` still feeds the overlay's
  rolling averages. `benchmark-seconds` is inert and stays so existing settings files load.
* Versioning: v2.3.0 is the release line; the v2.2.0 candidate never shipped and is
  superseded by this PR.
* Not measured: the Jawbreaker lag probe (`shader-trace`) has still not been run by the
  owner; hook installation is confirmed in the 14:13 log (all 10 hooks, including
  `CCGLProgram::use` and `CCParticleSystem::update`). FPS / 1% low unchanged claims: none.

## v2.2.0 (2026-11-11)

Two workstreams. The rendering work depends on an in-game trace the owner captures; the UI is
a full rebuilt as part of this version. See the README for the honest status of both.

* **Shader trace (diagnostic, `shader-trace`, bool, default off, Windows).** Per-frame
  accounting of every `CCGLProgram::use` call: total binds, redundant binds (validated against
  live `GL_CURRENT_PROGRAM`), program switches per batch, and the top 5 GL program objects by
  bind count, flushed to the Geode log every 5 s. No behavior change, no allocations, fixed-size
  state. This exists because v2.1.0's systems were shipped without confirming which GL path the
  GD 2.2 shader gameplay objects actually take - the measurement comes first, then the fix.
* **GPU frame time (`gpu-frame-time`, bool, default on, Windows).** Measures the GPU time of the
  submitted frame with `GL_TIME_ELAPSED` and shows it as `gpu` on the Detailed overlay. The
  result is read back one frame late with a non-blocking availability check, so the frame path
  never waits on the GPU. Measurement only: no GL state is changed beyond the query lifecycle.
  This is the number that answers whether the Jawbreaker shader lag is GPU-bound.
* **Detailed overlay** gains `gpu`, `trace binds` and the two v2.1.0 counters.
* **UI rebuilt** against a declared design system (violet/indigo mark, game-native panel
  language): one accent, semantic colors only, sentence case, spacing/radius scales, button
  roles. Panel is 480x284 with 31px rows and 40px buttons; nothing clips; text scales raised
  ~15-25%. The GD native close button overlapped the header content in the last build - header
  content now starts after it. Verification loop pending user screenshots - not marked done.
* **Escape** closes the panel via `geode::KeyboardInputEvent` (no multi-inheritance
  warnings); the key is consumed before the game's keyboard dispatch, so a native close
  can never double-fire.

## v2.1.0 (2026-11-11)

Two more opt-in systems, both implemented fail-open and both report through their own counters.
Physics, timing, input handling, draw order, blending and glow are unchanged.

* **Skip Redundant Program Binds** (`shader-state-cache`, bool, default off, Windows). Retired
  in v2.4.0; see the v2.4.0 entry. The original GL dance per node happens inside
  `CCGLProgram::use`; the hook there checks the live
  driver state (`glGetIntegerv(GL_CURRENT_PROGRAM)`) and skips the `glUseProgram` call when the
  program is already bound. There is no cached assumption anywhere, so a bind done by anyone
  outside the hook is picked up by the next `use()` - it cannot go stale. Counted in the new
  `programBindsSkipped` counter, shown on the Detailed overlay. Whether it pays on your machine
  is a measured question (per-call query vs per-call bind); it is off by default because the
  trade has not been measured.
* **Suppress Off-Screen Particle Simulation** (`particle-suppression`, bool, default off). The
  draw cull from v2.0.0 removed the submission but left the simulation running. This hooks
  `CCParticleSystem::update` and skips it for systems whose live particles are fully outside
  the expanded viewport (same live-particle AABB test, Relative/Grouped only). Emission,
  integration and the quad rebuild do not run; particle state freezes off screen and resumes
  with the next frame's normal dt when it scrolls back into view. Nothing is deleted
  mid-frame, so there is no cocos2d state corruption path. Counted in the new
  `particleSimsSuppressed` counter, shown on the Detailed overlay.
* Overlay Detailed mode is now four lines with both counters.

Evaluated and deliberately NOT done, with reasons:

* Skipping update for off-screen unbatched sprites: cocos2d runs node updates through
  `CCScheduler`/`CCActionManager` globally, not through a per-node `update()` worth gating, and
  freezing action updates desynchronizes animations everywhere - the mod's own rule (no
  timing/physics change) forbids it.
* Reordering/grouping draws by program or texture: transparency and z-order in the level path
  make this unsafe; correctness beats batching, so this was left for a batch the engine batches
  itself.
* Static-content caching ("nodes unchanged never redrawn"): cocos2d-x has no invalidation
  signal a mod can hook; anything there would be a guess.
* Shader precompilation at load: cocos2d's `CCShaderCache` already preloads the standard
  programs at engine init.

Not measured: this build has not been launched in-game. No FPS gain is claimed.

## v2.0.0 (2026-11-11)

The two features this project had been circling since v1.5.0, both implemented fail-open and
measured by construction. Nothing here changes physics, timing, input handling, draw order,
blending, shaders or glow.

* **Whole-batch visibility culling** (`batch-culling`, bool, default off; on in Performance and
  Extreme). A `CCSpriteBatchNode` whose full vertex buffer lies outside the viewport plus 64
  world units no longer pays its single GL draw. The two failure modes that got this feature
  removed in v1.5.1 are closed by construction: the scan covers exactly `[0, getTotalQuads())`
  (the atlas cannot submit more), and the skip never fires while `CCTextureAtlas` reports its
  buffer dirty (`isDirty()`) or while a child sort is pending
  (`m_bManualSortAllChildrenDirty`) - both rebuild inside the skipped `draw()`, so both answer
  "draw". The local AABB of every quad is converted to a rotation-invariant circle mapped
  through the batch's world basis. A per-frame scan budget of 24,000 quads caps the CPU cost
  of the feature; once spent, batches draw untested. Counted in the new `batchCulls` counter,
  shown on the Detailed overlay, on the Culling page ("empty / culled / drawn"), in the "Work
  avoided" row and in the benchmark line. This is the one feature that can remove draw
  submissions for the bulk of a level.
* **Particle culling now measures live particle data.** The old reach-cone bound could only
  reject a system when every *possible* future position was off screen, which is rarely true.
  The cull now computes the AABB of the particles this frame actually holds (from
  `m_pParticles`, expanded by particle size), converts the center to a rotation-invariant
  circle through the emitter's world basis, and compares against the expanded viewport.
  Relative and Grouped systems are both covered - both live in emitter-local space. Emitter
  on-screen still costs one circle test. Free systems are always drawn (absolute frame, no safe
  transform). The test is exact to the data the draw will submit a few calls later, so there is
  no "worst-case" margin to guess.
* **Counters and UI**: new `batchCulls` counter (total); the Culling page batch row shows
  `empty / culled / drawn`; the Detailed overlay prints the batch-cull total; "Work avoided"
  counts batch culls; the benchmark result line reports `batches N (M quads, K culled)` and
  records the mode with `batch cull on/off`; Performance and Extreme set `batch-culling`.
* **Versioning**: v2.0.0 because it is a new line of work, and because a GitHub release of
  "v1.0.0" would sort below every release this repo has produced.
* **Not done, honestly**: no in-game measurement of v2.0.0 exists. The geometry is argued from
  the cocos2d-x 2.2 headers (which is why the skip is bounded and fail-open), but this build
  has not been launched; the owner runs the first test. No FPS gain is claimed.

## v1.6.2 (2026-10-30)

Found by a read-only audit of the panel, not by playing. Two controls were dead.

* **Cull-mode cycle rows wrote an empty setting name.** `cycleRow` moved its key into the row
  record and then passed the moved-from string to the action, so `getSettingValue("")` was
  called for the Particles and Editor mode rows (the Scene sprites row only worked because the
  name is short enough to survive small-string optimization). The action now reads the key back
  from the row.
* **The Spawn budget row did nothing.** `makeCycleAction` had no `SpawnBudget` branch, so the
  button re-read the value and never wrote it. It now steps off / 8 / 16 / 32 / off.
* **`cull-offscreen-particles` had no control anywhere**, although it gates the whole particle
  off-screen test. It is on the Culling page now.
* **The finished benchmark result is shown in the panel** (`Last benchmark` on the Diagnostics
  page) instead of only in the Geode log.
* **One panel at a time.** Opening the badge twice created two popups with two live schedules;
  the open panel now registers itself and a second open is ignored.
* **Reset confirmation lifetime**: the popup is retained for the life of the confirmation alert
  and released when it is answered, instead of the alert holding a raw pointer.
* **Small-window fit uses the tighter of width and height**, and scales only the content layer,
  so a short window no longer clips the footer and nothing can be double-scaled.
* **The pause-menu badge is clamped inside the visible rect**, so a narrow window cannot push it
  off-screen.
* Switches no profile owns (master enable, render gate, background throttle, particle culling
  master, overlay, low latency, timer resolution, adaptive cap, editor culling, no-input-delay)
  no longer rewrite the profile to Custom.


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

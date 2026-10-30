# Minimum Reborn — Development Report

Date: 2026-10-30 (phase 1 and phase 2 reports below)
Target: Geometry Dash 2.2081 (Windows), Geode 5.11.2, C++20, CMake
Repository: https://github.com/ziyyhhk/MinimumReborn
Release package: `ziyyhhk.minimum-reborn.geode` (v1.1.0; v1.0.0 was the phase-1 build)

## 1. Scope actually delivered

Minimum Reborn v1.0.0 is a real, compiling Geode mod that changes specific execution paths in
rendering. It is not a settings-only mod and not an FPS-cap tweak.

Implemented:

1. **Sprite draw fast-out** — `CCSprite::draw` hook. Returns before the original draw when the
   sprite cannot produce a pixel (opacity 0, zero scale on an axis, empty content size).
2. **Particle fast paths** — `CCParticleSystemQuad` hooks.
   * `draw`: skip idle systems (zero live particles) and relative-position systems whose
     worst-case reach is entirely outside the visible rectangle. Update still runs.
   * `initWithTotalParticles` / `setTotalParticles`: clamp the pool before the original
     allocates, so buffers are allocated for the capped size and cannot be overrun.
   * Optional spawn budget: after N systems in one frame, further systems get pool 4.
3. **Render gate** (opt-in, off by default) — `CCDirector::drawScene` hook. Draws at a fixed
   rate while the scheduler keeps its own schedule; unfocused window draws at Background FPS.
   Identical mechanism to Minimum's Draw Divide, reimplemented.
4. **Windows system tuning** — 1 ms timer resolution (`timeBeginPeriod` + `winmm`),
   `ProcessPowerThrottling` opt-out (EcoQoS) resolved via `GetProcAddress`, optional process
   priority. State is applied only when it differs and undone when switched off.
5. **Performance overlay** — one `CCLabelBMFont` in Geode's OverlayManager, 4 Hz, text updated
   only when it changes; rolling 240-frame window: FPS, average/p95 frame time, 1% low,
   scheduler updates/s, CPU time of the draw call, per-second skip/cap counters, and a clearly
   labeled bottleneck hint. No GPU timer.
6. **Settings, profiles, UI** — 26 settings in `mod.json`, five profiles (Custom, Default,
   Balanced, Performance, Extreme) that write through to individual settings, reset-to-default,
   plus an in-game panel on the pause menu with live numbers, profile buttons, quick toggles,
   Reset and a link to Geode's native settings page.

Deliberately not implemented (documented, not faked): object batching / custom renderer,
dynamic resolution, GPU timing, `glFinish` low-latency mode, adaptive quality.

## 2. Rendering changes: original path vs. replacement

| Area | Original path | Minimum Reborn path | Visual risk | Recovery |
|---|---|---|---|---|
| Sprites | `draw()` always sets up quad, binds program/texture, submits | Early return for zero-pixel cases only | None: quad covers zero pixels | Toggle `sprite-fast-path` |
| Idle particles | `draw()` binds texture, uploads buffers, submits | Early return at particle count 0 | None: nothing to rasterize | Toggle `skip-idle-particles` |
| Off-screen particles | Always drawn | Culled when a conservative reach bound misses the viewport | Bound is conservative; free/grouped systems never culled; degenerate/NaN transforms answer "draw" | Toggle `cull-offscreen-particles` |
| Particle pools | Requested size allocated | Clamped before allocation | Loses particles in large bursts only | Toggle `cap-particles` |
| Frame draw cadence | Every main-loop frame | Optional gate; skipped frame still runs one scheduler update | Reduced smoothness above refresh rate | Toggle `render-gate` |

No global GL state, no render target, no shader and no texture is touched, so blending,
additive glow, clipping, z-order and atlases are exactly the original ones.

## 3. Input responsiveness

* Zero input-path code: no input hook, no queue, no buffer, no lock, no thread, no sleep, no
  disk access and no network call anywhere in the mod.
* Render gate never delays input processing: skipped frames still call the scheduler with the
  delta the main loop already computed, and never call `calculateDeltaTime()`.
* No physics, collision, timing or FPS-cap value is modified.
* Overlay and pause panel run on their own timers (0.25 s) and never inside the input path.

## 4. Build and verification

* Toolchain: GitHub Actions `windows-latest`, official `geode-sdk/build-geode-mod` action
  (pinned SHA), SDK 5.11.2, target Win64, Release, LTO.
* Runs: 41005991432 (failed: mod.json SDK version mismatch), 41006173398 (failed: one stray
  brace and one unqualified constant), **41006565096 (success, 2m21s)**.
* Artifact `MinimumReborn-Windows` downloaded and inspected:
  `mod.json`, `about.md`, `changelog.md`, `ziyyhhk.minimum-reborn.dll` (181,248 bytes),
  package 69,697 bytes.
* Tests performed: compile + link on MSVC/clang-cl via CI, mod.json schema validation by the
  Geode build action, package structure inspection. **No runtime test in Geometry Dash has
  been performed in this environment** (the game is not installed here).

## 5. Benchmarks

None. No FPS or frame-time numbers are claimed. The overlay is the instrument for measuring on
real hardware; the procedure is in README ("Benchmark procedure"). Any future figures must
come from the same level, resolution, FPS cap and background workload, reported as a range over
repeated runs, comparing average and p95 frame time and 1% low — not average FPS alone.

## 6. Known limitations and risks

* Not yet loaded in-game; startup and interaction bugs are possible and should be checked on
  the first launch (see checklist below).
* Off-screen particle culling uses a conservative analytic bound. Exotic systems with unusual
  custom motion could be culled while still visible; the toggle exists for that case.
* Particle pool cap applies to systems created after the change; existing systems keep their
  pool until recreated.
* Render gate conflicts with other draw-cadence mods (`mat.draw-divide`).
* Overlay uses `chatFont.fnt`; a custom font/replacement could alter its look.
* No dynamic resolution, no batching, no GPU timings — future work.

## 7. First-launch checklist

1. Install the `.geode`, start the game, confirm the mod loads without a crash dialog.
2. Open a dense level (glow, particles, many objects) with the overlay on; confirm FPS and
   "particle skips/s" are non-zero.
3. Check glow/particles/portals/coins/trails look unchanged with all fast paths on.
4. Toggle each feature off and confirm no visual change compared to Geode-only.
5. Enable the render gate above 60 FPS and confirm movement, input and physics feel unchanged.
6. Restart a level and confirm no rendering corruption; alt-tab and confirm background throttle.

## 8. Files

Project: `MinimumReborn/` — `CMakeLists.txt`, `mod.json`, `about.md`, `changelog.md`,
`README.md`, `LICENSE`, `.github/workflows/build.yml`, `src/{config,rendering,frame_gate,
frame_stats,system_tuning,ui,main}.cpp` and headers.

Deliverables in the workspace: `MinimumReborn.geode`, `MinimumReborn-Source.zip`.

## 9. Licensing

MIT. Bismuth (GPL-3.0-or-later plus a restriction on generative-AI level generation and
modification) was read as a design reference only; no code, shader or asset from it is used.
Minimum (MIT, same author) supplied behavioral reference for particle handling and the
render-gate scheduling idea; the code here was written for this project.

# Phase 2 (v1.1.0) — rendering path work

The second session extended the existing project rather than rebuilding it. Everything below
is in the source tree and in the v1.1.0 CI artifact; it has still not been run inside the game.

## 10. What changed

1. **Renderer mode.** `renderer-mode` is `Engine` or `Optimized`. Engine takes every original
   code path and exists as a control for measurement; Optimized enables the mod's draw path.
   All culling and fast-outs are gated on it, so an A/B test is one setting, not a reinstall.
2. **Scene object culling** (new, the main phase-2 feature). Every `CCSprite::draw` in
   Optimized mode is tested against the viewport. A sprite whose whole quad is outside is not
   drawn. This is the first culling of ordinary level objects (blocks, decorations, slope
   textures, glow sprites, trails) in this project.
3. **Particle cull modes.** Off / Conservative / Aggressive, independent from the scene cull;
   the old boolean remains as the on/off master for particle culling.
4. **Emitter-inside shortcut.** If the emitter position is inside the view, the system is drawn
   without computing the reach bound, so visible systems cost one circle test.
5. **Opt-in adaptive particle cap** (`adaptive-cap`, default off): 25% steps down after two
   slow one-second windows, back up after six fast ones, floor 24, never above the configured
   cap. Only allocation size changes; the simulation is not touched.
6. **Honest telemetry.** `scene N of M/s` and `particles N of M/s` separate *tests performed*
   from *draws skipped*, plus invisible-sprite skips, capped pools, logic-only frames. A
   benchmark can therefore show whether culling removed work at all.
7. **In-game benchmark** (`src/benchmark.cpp`): fixed 16384-slot buffer, no allocation in the
   frame path, one sort at stop, reports average FPS, average/p50/p95 frame time, 1% low,
   variability and culling deltas; writes a one-line summary and a detailed log entry.
8. **Pause panel rebuilt** for the new modes: renderer/scene/particle cycle buttons, live
   2–3 line readout, Reset, Bench (3–60 s), native settings shortcut.

## 11. Scene culling: how the test works

Per frame (`renderFrameBegin`, called from the drawScene hook before anything is drawn):

* viewport snapshot from `CCDirector::getVisibleSize()` / `getVisibleOrigin()` in scene
  coordinates; if it is non-finite or empty, culling answers "draw" for the whole frame;
* the parent of each sprite is mapped once per frame: world basis `{origin, ux, uy}` from
  `convertToWorldSpace(0,0)`, `(1,0)`, `(0,1)`, cached in a fixed 256-slot direct-mapped table
  keyed by node pointer and frame id. Blocks share parents, so this is paid a few times per
  frame, not thousands.

Per sprite:

* `r = hypot(contentW, contentH) * max(|scaleX|, |scaleY|) * max(|ux|, |uy|)`;
* center `= origin + pos.x*ux + pos.y*uy`;
* reject only if `center + r` and `center - r` miss the viewport expanded by the margin.

The circle is centred on the node position and sized by the whole diagonal, so it covers the
quad for any anchor point, any rotation (cocos2d rotates about the anchor inside the quad) and
any uniform or non-uniform scale. The test is intentionally looser than a bounding box: it
keeps some sprites that a precise test would reject, but it cannot reject a visible one. A
rotated parent only makes `r` larger, never smaller. Margins: scene 160 (Conservative) / 16
(Aggressive) world units; particles 128 / 8. Missing parent, empty content size, non-finite
values or a degenerate basis answer "draw".

Only `draw()` is skipped. `visit()` still runs, so updates, animations, triggers, children,
collision and physics never see the culling code. Rendering never feeds back into gameplay.

## 12. What was deliberately not done

* **Batching / a replacement renderer.** Z-order, additive glow, clipping and blending make a
  partial batcher a visual-correctness risk, and the SDK exposes no safe replacement path.
  "Optimized" is documented as a cheaper draw path, not a new renderer. No Bismuth code.
* **Glow-specific toggle.** GD glow is an ordinary additive `CCSprite`; the sprite cull and
  fast-out already cover it. A separate switch would only remove visuals.
* **Dynamic resolution, GPU timers, `glFinish`.** Same reasoning as phase 1: no safe or
  non-blocking path through this SDK, and a blocking query would cost frames.
* **Skipping particle simulation for off-screen systems.** Explicitly out of scope: an
  off-screen system still runs `update()` every frame, so emission timing, lifetimes and
  physics are unchanged. Only its draw is skipped.

## 13. Build and verification (phase 2)

* Run 41008194965 **failed**: `CCSprite::getBoundingBox` is not exposed by the bindings.
  Fixed by deriving the radius from `getContentSize()` and scale, anchored at the node
  position (also safer for rotation). Two unqualified `CullMode` references were qualified.
* Run 41008425934 (commit before telemetry) succeeded.
* Run [41008735369](https://github.com/ziyyhhk/MinimumReborn/actions/runs/41008735369)
  (commit `104d69d`) succeeded, 2m. Artifact `MinimumReborn-Windows`:
  `mod.json` v1.1.0, `about.md`, `changelog.md`, `.geode_cache`,
  `ziyyhhk.minimum-reborn.dll` 200,192 bytes, package 79,385 bytes.
* Verified by inspection only: compile/link on the CI Windows toolchain, Geode package
  structure, and a manual cross-check that every settings key read by `config.cpp` exists in
  `mod.json`. **Not verified at runtime** — Geometry Dash is not installed in this
  environment, so no launch, no FPS number, no visual check is claimed. All performance
  statements remain hypotheses for the user's benchmark to test.

## 14. Risks introduced in phase 2

* If a sprite's visible extent exceeded `contentSize × scale` (for example a custom shader
  that expands the quad), the scene cull could reject it while it still paints. Stock GD
  sprites do not do this; the Aggressive/Off switch and `renderer-mode = Engine` isolate it.
* Scene culling keeps fewer draws on screen as the camera moves fast; the 160-unit margin
  covers normal scroll speeds, and any doubt keeps the draw.
* The transform cache is direct-mapped and rebuilt each frame; a hash collision only causes a
  recompute, never a stale answer, because the frame id is part of the key.
* Benchmark buffer is 16384 frames (~4.5 min at 60 FPS); longer runs stop early at the cap.

## 15. Files touched in phase 2

`src/rendering.cpp` (rewritten: viewport snapshot, basis cache, sprite/particle culling, modes),
`src/rendering.hpp`, `src/config.{hpp,cpp}` (CullMode, ProfileValues, margins, adaptive cap,
counters), `src/frame_gate.cpp` (per-frame begin, per-second telemetry, benchmark feeding),
`src/benchmark.{hpp,cpp}` (new), `src/ui.cpp` (panel modes, live readout, Bench),
`CMakeLists.txt`, `mod.json`, `CHANGELOG.md`, `README.md`, this report.

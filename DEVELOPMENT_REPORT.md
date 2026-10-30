# Minimum Reborn — Development Report

Date: 2026-10-30 (phase 1, phase 2 and phase 3 reports below)
Target: Geometry Dash 2.2081 (Windows), Geode 5.11.2, C++20, CMake
Repository: https://github.com/ziyyhhk/MinimumReborn
Release package: `ziyyhhk.minimum-reborn.geode` (v1.2.1; v1.1.0 was the phase-2 build,
v1.0.0 the phase-1 build)

Phase 3 (§16–24) is the first in-game test. It corrects claims made in phases 1–2 where the
measurements disproved them; earlier sections are kept as history, with the key correction
inline and §23 listing what is still unverified.

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
  Geode build action, package structure inspection. At the time of phase 1 no runtime test in
  Geometry Dash had been performed (the game was not installed then). **Phase 3 ran the mod
  in-game; see §16–24.**

## 5. Benchmarks

At the time of phase 1: none. Phase 3 measured four 10 s runs in Geometry Dash; the numbers and
their limits are in §18 and §23. They come from the same level, resolution, FPS cap and
background workload, reported as observed, and are not a claim of an FPS gain.

## 6. Known limitations and risks

* Phase 2: not yet loaded in-game. Phase 3 loaded it; startup, panel, overlay and benchmark
  were exercised. See §21 for the UI defects that were found and fixed, and §23 for what was
  not tested.
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
   drawn. Phase 2 described this as culling ordinary level objects; **phase 3 measured that
   claim and it is false for GD levels** — level objects are drawn through
   `CCSpriteBatchNode`, so 0 of 18,939 sprite visits were culled. See §16.
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
* **Glow-specific toggle.** GD glow is part of the batched level path (phase 3), so the
  per-sprite cull and fast-out do not see it. A separate switch would remove visuals without
  removing the batch submission.
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
* Run 41009157257 (documentation-only commit `21735f2`) also succeeded and produced the
  delivered package: same source, `about.md`/`changelog.md` regenerated from the final docs,
  DLL 200,192 bytes, package 82,232 bytes. CI builds from the two runs are not byte-identical
  (build metadata), so no binary hash equality is claimed; the source is identical.
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

# Phase 3 (v1.2.1) — first in-game test

The user provided the Geometry Dash install on this laptop
(`C:\Users\akira\Documents\Games\Geometry Dash`, GD 2.2081, Geode 5.11.2) and asked for a real
test. This section reports what was run, what was measured, what broke, and what changed as a
result. It supersedes any "not verified at runtime" statement above.

## 16. Procedure

1. Installed the v1.1.0 build into `geode/mods`, launched GD, confirmed the mod loaded. Moved
   the user's `minimum.optimization.geode` to `geode/mods_disabled_by_test/` so two
   optimization mods did not fight over the same hooks (restore instructions are in the final
   message; the file was not edited or deleted).
2. Built v1.2.0 through GitHub Actions (run 41011043806), installed it, relaunched. The log
   shows the new `CCSpriteBatchNode::draw` hook enabled for the mod.
3. Entered Stereo Madness (Normal). A user-installed auto-player mod was active, so the level
   played itself and restarted on death; no input was synthesized during a benchmark except
   Escape (pause), mouse clicks on the panel, and the resume button.
4. Opened the pause panel (MR button), selected a renderer, pressed Bench, closed the panel
   with Escape, resumed the level, waited for the 10 s window, and read the benchmark line from
   `geode/logs/Geode 2026-10-30 13.57.09.log`. Two runs per renderer, alternating.
5. Captured the screen with a window-scoped tool (`tools/gdwin.ps1`) that brings GD to the
   foreground and crops to its client area, so no other window contents were captured.

## 17. What the first run found: level objects are batched

The phase-2 model assumed every level object reaches `CCSprite::draw`. That is wrong. Game
level objects such as blocks, spikes, decorations and glow are submitted by
`CCSpriteBatchNode` layers (the bindings name them `m_gameLayerT4`, `m_gameGlowLayerT4`, ...).
`GameObject` does not override `draw`, and the batch node draws its whole atlas itself, so the
per-sprite hook never sees those objects.

Evidence from the v1.2.0 benchmark (Stereo Madness, 10 s, 601 frames):

```
scene culled 0/18939 | particles skipped 0/601 | batches 24671 (231543 quads)
scene culled 0/11891 | particles skipped 0/601 | batches 20898 (211759 quads)
```

That is 35–41 batch submissions and 352–385 quads per frame, versus 20–32 sprite visits per
frame — the per-sprite hook sees about 3% of the drawable content. GD's object manager also
keeps off-screen objects out of the draw, which is why the scene cull found nothing to skip.

Consequences, applied in v1.2.1:

* Scene culling defaulted **off** in Balanced. It remains available (Performance, Extreme,
  settings) for unbatched content, where its test is still valid, but the product no longer
  implies it culls level objects. `mod.json`, README and this report say what was measured.
* A batch telemetry hook was added in v1.2.0 to count batch draws/quads. It only reads the
  texture atlas and increments counters; it never edits atlas indices or reorders draws.
* Rebuilding batch indices per frame to cull individual quads was rejected: it would mean
  rewriting a GPU index buffer every frame and changing draw order for no measured gain on a
  60 FPS-capped scene. That would be a different renderer, not this mod.

## 18. Frame results and the 60 FPS cap

GD caps in-level rendering at 60 FPS on this setup. Four runs, 601 drawn frames each:

| Run | Version | Renderer | Avg | p50 | p95 | 1% low | Draw CPU | Scene | Particles | Sprite skips |
|---|---|---|---|---|---|---|---|---|---|---|
| A | 1.2.0 | Optimized | 16.67 ms | 16.34 | 17.36 | 39 fps | 3.10 ms | 0/18,939 | 0/601 | — |
| B | 1.2.0 | Engine | 16.67 ms | 16.33 | 17.36 | 46 fps | 2.79 ms | — | — | — |
| C | 1.2.0 | Engine | 16.67 ms | 16.33 | 17.36 | 44 fps | 2.88 ms | — | — | — |
| D | 1.2.0 | Optimized | 16.67 ms | 16.33 | 17.35 | 42 fps | 2.78 ms | 0/11,891 | 0/601 | — |
| E | 1.2.1 | Optimized | 16.67 ms | 16.33 | 17.36 | 38 fps | 2.66 ms | 0/0 (off) | 0/598 | 1,516 |

Average frame time is exactly the cap in every configuration, so **no FPS improvement is
possible or claimed on this scene**. Draw CPU (`CCDirector::drawScene`, traversal plus GL
submission) is 2.78–3.10 ms optimized vs 2.79–2.88 ms engine; the ranges overlap and the runs
cover different parts of the level, so this is a wash, not a win. The panel/overlay numbers
agree with the log because both read the same counters.

The 1% low column is a single-run statistic (601 frames, six slowest averaged) and varies
39–46 fps across runs; it is reported as observed, not as a stable property of either mode.
No claim of reduced stutter is made.

## 19. Particle and scene culling in practice

* Scene: the hook ran (18,939 examinations in run A) and skipped 0. This is expected after
  §17; run E had the scene cull off, hence 0/0. The fail-open test was never exercised with a
  non-zero cull in GD; that limitation is in the README.
* Sprite fast-out: run E skipped 1,516 sprite draws in 602 frames (~2.5 per frame) for
  zero-pixel/zero-scale/empty sprites. This is the only skip path with a measured non-zero
  effect on Stereo Madness; it is not an FPS claim.
* Particles: exactly one particle system per frame was examined and 0 were skipped, because
  Stereo Madness's system stays on screen. The idle/off-screen paths therefore did not fire in
  this level either. They remain enabled in Balanced because they cost one check per system
  and only skip work that produces no pixels, but the report does not claim a saving.
* No node updates, trigger logic or simulation were changed. The hooks only return before
  `draw()`; `visit()` and the scheduler are untouched.

## 20. Benchmark contamination found and fixed (v1.2.1)

The v1.2.0 benchmark counted every drawn frame after the button was pressed. Because the
button lives in the pause panel, the first ~1 s of each run was the pause screen; it still
draws at 60 FPS, so it was included in runs A–D. The window now starts on the first
**unpaused** frame and paused frames are skipped, and the log line gained invisible-sprite
skips. Run E confirms it: 602 frames in 10.0 s after resuming, plus 1,516 invisible-sprite
skips. The four v1.2.0 runs above were taken before the change.

## 21. UI problems observed and fixed

* The v1.0/v1.1 pause panel overflowed the 320-unit design height: the title and footer were
  cut off. v1.2.0 reduced the panel to 310 units and compressed the rows; the v1.2.0 screenshot
  shows the title, all toggles, Reset/Bench/All Settings and the footer line.
* The overlay default was Top Left, where GD's close button and attempt counter sit. It was
  clipped by the close button in the v1.2.0 screenshot. The default is now Top Right. An
  existing install keeps its stored `overlay-corner` value until Reset (or the setting) is
  used; that persistence is expected Geode behavior, not a bug.
* Escape pauses reliably when the window has foreground keyboard focus. Posting key messages
  without focus did not pause GD; the test tool now focuses the window, then sends the key.

## 22. Package size investigation

The user asked why the `.geode` is small. Inspection of the delivered v1.2.1 artifact
(86,352 bytes packed, 241,101 bytes unpacked):

```
.geode_cache 30 B | about.md 17,365 B | changelog.md 4,247 B | mod.json 11,075 B
ziyyhhk.minimum-reborn.dll 208,384 B
```

The v1.2.0 artifact was 84,318 bytes packed (236,025 unpacked) with the same 208,384-byte DLL;
the difference is the regenerated `about.md`/`changelog.md`/`mod.json`.

All seven sources listed in `CMakeLists.txt` (`main`, `config`, `rendering`, `frame_gate`,
`benchmark`, `system_tuning`, `ui`) are compiled into that DLL; there are no other object files
to miss. The binary is small because it contains only this mod: cocos2d-x, Geode and the C++
runtime are imported from the game install, not statically bundled. Nothing was added to
inflate the package; the `about.md`/`changelog.md`/`mod.json` text and the `.geode_cache`
marker are the only extras. A larger package would not mean more optimization.

## 23. Limits of this evidence

* One level (Stereo Madness), one laptop, one window size, 60 FPS cap, with an auto-player mod
  and many other mods installed. Other levels, especially effect-heavy ones, will exercise the
  particle paths differently.
* Culling's visual correctness was not A/B compared frame-by-frame; the argument rests on the
  fail-open test and on the fact that it skipped zero draws in the measured level. A run with a
  non-zero cull count has not been captured.
* No GPU timer, no uncapped test, no long session, no multiplayer/editor testing.
* Runs A–D are v1.2.0 and include ~1 s of pause screen each. Run E is v1.2.1 and excludes
  paused frames, so its draw time is not directly comparable with A–D; it is included to show
  the fixed recording window and a non-zero sprite-skip count, not as a faster result.

## 24. Files touched in phase 3

`src/rendering.cpp` (batch telemetry hook, `CCSpriteBatchNode` include), `src/benchmark.{hpp,cpp}`
(draw ms, batch deltas, invisible sprites, unpaused start), `src/frame_gate.cpp` (feed the
benchmark only when not paused), `src/config.cpp` (Balanced scene cull off, reset value),
`src/ui.cpp` (panel height/layout), `src/config.hpp` (batch counters), `mod.json` (version,
defaults, corrected descriptions), `CMakeLists.txt` (version), `README.md`, `CHANGELOG.md`,
this report.

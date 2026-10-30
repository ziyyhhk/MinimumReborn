# Minimum Reborn — Development Report

Date: 2026-10-30
Target: Geometry Dash 2.2081 (Windows), Geode 5.11.2, C++20, CMake
Repository: https://github.com/ziyyhhk/MinimumReborn
Release package: `ziyyhhk.minimum-reborn.geode` (v1.0.0)

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

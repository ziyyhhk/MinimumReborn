# Resume notes - 2026-11-12 (v2.4.1)

Everything below is checkable in the repo or in the CI run list; nothing here is a claim of
in-game performance.

## Where the project stands

* Version: working tree is **v2.4.1**; `main` is at `05bf062` (v2.4.0). The v2.4.1 changes are
  **uncommitted and not built yet**. CI must pass before this can be called real or installed.
* The live game folder holds the **v2.4.0** install (CI run `43010951715`, artifact
  `ziyyhhk.minimum-reborn.geode`, 290,318 bytes, sha256
  `B1B024BB25D1505EAA2D735FFD1682E2BC039C46038E21CED99386D0B0731659`). It was installed over
  a **v1.3.0** build, which together with your current (extreme) `settings.json` is backed up
  at `C:\Users\akira\AppData\Local\Temp\opencode\live-backup`.
* The game has not been launched or played with v2.4.0 or v2.4.1.
* **v2.4.1 changes** (on top of the v2.4.0 per-quad batch filter):
  1. **Custom Shader Filter** (`custom-shader-filter`, bool, **on by default**, Windows).
     Fragment-only shader effects keep the vertex pass-through, so their vertices sit exactly
     where the stock draw would put them. A batch program whose vertex stage is byte-identical
     to the stock PositionTextureColor program is now filtered like the stock program; a custom
     vertex stage that could displace geometry (waves, shockwaves, lens distortions) never is.
     Verification happens per frame and is fail-open. This is the piece that makes the filter
     relevant to GD's shader library shape; whether GD ships such effects byte-identical at
     runtime is unproven until a log shows it.
  2. **Tighter live-particle cull.** The world-space rectangle through the four AABB corners
     (the same test the sprite fast-out uses) replaces the bounding circle bound; strictly
     tighter, no visual change.
  3. **Scan budget raised to 65,536 quads per frame** (was 24,000) so a dense 2.2 object count
     is actually filtered, not silently left untested.
* `batch-culling` is on in Balanced/Performance/Extreme; Default (Engine) leaves the optimized
  draw path off. `custom-shader-filter` defaults on and requires `batch-culling`.
* `shader-state-cache` is retired; `shader-trace` references GD-side CPU timings; `gd-trace`
  logs visibility, shader layer, `ShaderLayer::visit` and the batch scan every 5 s.
* The live `settings.json` still has your old extreme values (`target-fps: 360`,
  `particle-cap: 4`, `render-gate-fps: 360`, `background-throttle: true`, `shader-trace: true`)
  **and** your `profile` is `Extreme`. On first launch, the v2.4.x profile logic overwrites the
  profile-managed keys: `particle-cap` becomes 32, `background-throttle` becomes false. To keep
  your manual values, set `profile` to **Custom** on the native Geode settings page (the MR
  button) before trusting the game's behavior.

## Open work, in priority order

1. Build v2.4.1 in CI (push to `main`; the pinned Geode 5.11.2 workflow) and fix any compile or
   link errors. CI is the only compiler here.
2. After CI passes, install the new artifact to the live folder (backup again first).
3. **The Jawbreaker with `shader-trace` on**, ~30 s in the slow section. Send the log:
   * visibility / `updateShaderLayer` CPU high -> that path is the next target;
   * `gpu` line near the frame time while `gd-trace` is small -> GPU-bound; no CPU cull fixes it;
   * `quads culled` near zero with shaders active -> the level objects are not being filtered;
     the log lines will also say whether the filter ever found a pass-through program.
4. Visual A/B (`batch-culling` on/off) to confirm identical rendering.
5. `shader-trace` is currently on in your settings; it costs a GL query per program bind. Turn
   it off after the log is captured.

## Honest limits

* No in-game launch or measurement exists for any of these builds. No FPS claim is made.
* The custom-shader filter is only as good as GD's fragment-shaders-with-stock-vertex-pass
  shape, and only when the byte-identical verification passes at runtime. If GD displaces
  vertices in its effect programs, the filter correctly draws untouched and helps nothing -
  that is exactly what the `gd-trace` lines will show.
* GPU fragment cost of a full-screen shader is not removable by any draw cull; if the frame is
  GPU-bound the answer is the GPU timer line and a visual-quality choice, not more culling.

## Licensing hygiene to keep

GPL-licensed third-party renderers are a design reference only, never copied, and not named in
release text. All code here is original.

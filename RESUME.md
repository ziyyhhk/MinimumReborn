# Resume notes - 2026-11-12 (v2.3.2)

Everything below is checkable in the repo or in the CI run list; nothing here is a claim of
in-game performance.

## Where the project stands

* Version: **v2.3.2** on `main`, released on GitHub on 2026-11-12
  (<https://github.com/ziyyhhk/MinimumReborn/releases/tag/v2.3.2>). v2.3.1 shipped a crash in
  the GPU Batch Filter (DEP violation in glew32.dll at `_glewCreateShader`; root cause: GLEW
  `__glew*` exports are function-pointer table variables, dereferenced only in the fix, not in
  the shipped call path). v2.3.2 dereferences them, treats `wglGetProcAddress` failure markers
  as unavailable, and latches the GPU path off when anything is missing - CPU filter fallback,
  same as every other GL feature. Do not use v2.3.1 with GPU Batch Filter enabled.
* The v2.3.2 artifact is **untested in-game and is the first build with a testable GPU Batch
  Filter**. It is installed to the live game folder at
  `C:\Users\akira\Documents\Games\Geometry Dash\geode\mods\ziyyhhk.minimum-reborn.geode`
  (hash-verified). The previous v2.3.1 build (and the v2.3.1+Phase-1 diagnostic test build) are
  backed up at `C:\Users\akira\AppData\Local\Temp\opencode\live-backup`. Settings are the
  owner's Extreme setup (`gpu-batch-cull` currently ON for testing, `shader-trace` still on).
* The game has not been launched or played with any of these builds. No in-game claim exists.
* v2.3.2 release artifact: `ziyyhhk.minimum-reborn.geode`, 299,233 bytes, sha256
  `3E37AFF982BD6F8ECA69B22BEEF5CE274B62C11C5BDA591146164D51822D20B4` (matches the GitHub
  release digest). Installed to the live folder and hash-verified after install. CI run
  `43134984050` (main push, merge `b92cc5b`), PR #7 CI `43134729468`. v2.3.1 release remains
  on GitHub for history but crashes with GPU Batch Filter; do not install it.
* **v2.3.1 changes** (on top of the v2.4.0 per-quad batch filter):
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
  4. **GPU Batch Filter** (`gpu-batch-cull`, bool, Windows, **off by default**, experimental,
     requires `batch-culling`). The per-quad batch-filter scan moved onto the GPU: a GL 4.3
     compute shader reads the atlas vertex VBO as a read-only SSBO and emits the surviving
     indices plus the draw count into a `glDrawElementsIndirect` command; no GPU result is
     ever read back. Only batches larger than 1024 quads use it. Every existing gate applies
     first; any failure (pre-4.3 GL, missing entry points, shader build error, non-screen-space
     projection) falls back to the CPU filter, and the failure latches for the session. Written
     from scratch for this project; GPL-licensed third-party renderers were a design reference
     only. Overlay gains `gpu batches` / `gpu quads` counters. NOT verified in-game.
* `batch-culling` is on in Balanced/Performance/Extreme; Default (Engine) leaves the optimized
  draw path off. `custom-shader-filter` defaults on and requires `batch-culling`. `gpu-batch-cull`
  is off by default and in every profile; turn it on only to test the compute path.
* `shader-state-cache` is retired; `shader-trace` references GD-side CPU timings; `gd-trace`
  logs visibility, shader layer, `ShaderLayer::visit` and the batch scan every 5 s.
* The live `settings.json` still has your old extreme values (`target-fps: 360`,
  `particle-cap: 4`, `render-gate-fps: 360`, `background-throttle: true`, `shader-trace: true`)
  **and** your `profile` is `Extreme`. On first launch, the v2.4.x profile logic overwrites the
  profile-managed keys: `particle-cap` becomes 32, `background-throttle` becomes false. To keep
  your manual values, set `profile` to **Custom** on the native Geode settings page (the MR
  button) before trusting the game's behavior.

## Open work, in priority order

1. **Run The Jawbreaker with `shader-trace` on**, ~30 s in the slow section. Send the log:
   * visibility / `updateShaderLayer` CPU high -> that path is the next target;
   * `gpu` line near the frame time while `gd-trace` is small -> GPU-bound; no CPU cull fixes it;
   * `quads culled` near zero with shaders active -> the batch's shader programs are not
     passing the byte-identical vertex check; that is the signal to stop optimizing CPU side and
     measure GPU fragment cost instead.
2. Visual A/B (`batch-culling` and `custom-shader-filter` on/off) to confirm identical rendering.
   For v2.3.1 add a `gpu-batch-cull` A/B on the same level: it should only engage above 1024
   quads per batch (Stereo Madness ~400/frame will never engage it); `gpu batches` / `gpu quads`
   should move; rendering must stay identical. If it does not, turn `gpu-batch-cull` off - the
   failure latch will disable it for the session anyway, but the CPU filter should stay your
   default until a log shows the compute path is safe and useful.
3. `shader-trace` is currently on in your settings; it costs a GL query per program bind. Turn
   it off after the log is captured.

## Honest limits

* No in-game launch or measurement exists for any of these builds, including v2.3.2. No FPS
  claim is made. v2.3.1 crashed on the first GPU Batch Filter draw (DEP violation); v2.3.2
  dereferences the GLEW table and fails safe, but the GPU path still has never run a frame.
* The GPU batch filter crashed v2.3.1 on the first eligible batch (DEP violation inside
  glew32.dll; my bug - GLEW table variables were called as functions). v2.3.2 dereferences
  the table and latches the path off on any failure, which bounds the damage to
  "falls back to the CPU filter", but it still has not run a frame in-game. Until a log
  shows `gpu batches` moving, treat it as unavailable.
* The custom-shader filter is only as good as GD's fragment-shaders-with-stock-vertex-pass
  shape, and only when the byte-identical verification passes at runtime. If GD displaces
  vertices in its effect programs, the filter correctly draws untouched and helps nothing -
  that is exactly what the `gd-trace` lines will show.
* GPU fragment cost of a full-screen shader is not removable by any draw cull; if the frame is
  GPU-bound the answer is the GPU timer line and a visual-quality choice, not more culling.

## Licensing hygiene to keep

GPL-licensed third-party renderers are a design reference only, never copied, and not named in
release text. All code here is original.

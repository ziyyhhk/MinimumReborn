# Resume notes - 2026-11-12 (v2.3.1)

Everything below is checkable in the repo or in the CI run list; nothing here is a claim of
in-game performance.

## Where the project stands

* Version: **v2.3.1** in the working tree, not committed, built or installed yet. CI run for
  the release (Windows, Geode 5.11.2) is pending at the time of writing; release `v2.3.1` on
  GitHub is the goal of this session. `main` was at `ea22d75` (labeled v2.4.1, built green by
  CI run `43120821956` on 2026-11-12). The v2.4.0/v2.4.1 line was never released, so the
  version was deliberately reset to **2.3.1**, which ships v2.4.0 + v2.4.1 + the new GPU batch
  filter.
* The v2.4.1 artifact (`ziyyhhk.minimum-reborn.geode`, 291,638 bytes, sha256
  `FA1A00859E4B924BFFC2C60F734E81F64D7F9973C08EC68EE61158B2A4DE921D`) is installed to the
  live game folder at `%LOCALAPPDATA%\GeometryDash\geode\mods\ziyyhhk.minimum-reborn.geode`.
  The previously installed v2.4.0 build (`B1B024BB...`) and your current `settings.json` are
  backed up at `C:\Users\akira\AppData\Local\Temp\opencode\live-backup`. The v1.3.0 build is
  backed up there too.
* The game has not been launched or played with any of these builds. No in-game claim exists.
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

* No in-game launch or measurement exists for any of these builds, including v2.3.1. No FPS
  claim is made.
* The GPU batch filter has never run anywhere; it is compile-verified-only. It is off by
  default, absent from every profile, and falls back to the CPU filter on the first GL
  failure, but it has not been watched rendering a frame.
* The custom-shader filter is only as good as GD's fragment-shaders-with-stock-vertex-pass
  shape, and only when the byte-identical verification passes at runtime. If GD displaces
  vertices in its effect programs, the filter correctly draws untouched and helps nothing -
  that is exactly what the `gd-trace` lines will show.
* GPU fragment cost of a full-screen shader is not removable by any draw cull; if the frame is
  GPU-bound the answer is the GPU timer line and a visual-quality choice, not more culling.

## Licensing hygiene to keep

GPL-licensed third-party renderers are a design reference only, never copied, and not named in
release text. All code here is original.

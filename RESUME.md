# Resume notes - 2026-11-11 (v2.4.0)

Written as the hand-off for the v2.4.0 work. Nothing here is a claim of work done in-game;
every line below is checkable in the repo or in the CI run list.

## Where the project stands

* Version: **v2.4.0** on `main`. The v2.4.0 work is committed directly on `main`; the push
  triggers the CI build (pinned Geode 5.11.2, Windows Release). Nothing has been tagged or
  released yet.
* The one shipped optimization change is the **per-quad filter inside
  `CCSpriteBatchNode::draw`** (`src/rendering.cpp` section 4). It removes zero-area and fully
  off-screen quads from the index list while keeping the batch's single GL draw, blend state,
  atlas order and normal vertex upload. It replaces the v2.0 whole-batch skip, which rarely
  fired and could not help a level that was on screen but dense.
* `batch-culling` is now **on in Balanced** and on in Performance/Extreme; Default (Engine)
  leaves the mod's draw path off entirely.
* `shader-state-cache` is **retired**. cocos2d already caches program binds inside
  `ccGLUseProgram`; the extra per-bind `glGetIntegerv` was removed. The key stays in
  `mod.json` as an inert legacy key so old settings files remain valid.
* `shader-trace` now also logs `gd-trace`: average/max CPU time for
  `PlayLayer::updateVisibility`, `LevelEditorLayer::updateVisibility`,
  `GJBaseGameLayer::updateShaderLayer`, `ShaderLayer::visit` and the batch scan.
* Local install still holds the released **v2.3.0** build. v2.4.0 has not been built or
  installed.

## Static re-check (2026-11-11, still no compiler available)

* The 16,383-quad ceiling is the engine's own 16-bit atlas index limit (`CCTextureAtlas::setupIndices`
  truncates to `GLushort`), so the filtered draw can never submit an index the stock path cannot
  represent.
* The `CC_INCREMENT_GL_DRAWS` reference resolves through the SDK win64 import library
  (`__imp_?g_uNumberOfDraws@@3IA` is present); every other GL entry point in the filtered draw is one
  the stock atlas already calls.
* `batch-culling` counters only move when a quad is genuinely omitted or a submission is genuinely
  skipped. No in-game run of v2.4.0 exists, so no FPS claim is made.
* Public docs were swept for the removed panel/benchmark references and for third-party renderer
  names; both are gone from the release-facing text.

## What to test first, in one minute

1. Build v2.4.0 through GitHub Actions (Geode 5.11.2 pinned, Windows Release). Confirm the
   workflow is green and the artifact is `MinimumReborn-Windows`.
2. Install it, open a level, and check the Detailed overlay: `quads culled`, `batch scans` and
   `fully culled` should
   move on a normal level, and the level must look identical. Toggle `batch-culling` off and on
   to compare.
3. Play **The Jawbreaker** with `shader-trace` on for ~30 s. The log gets a `gd-trace` line
   every 5 s. Send the log. The split decides the next change:
   * visibility / `updateShaderLayer` CPU high -> attack that path next;
   * `gpu` line near the frame time while `gd-trace` is small -> the frame is GPU-bound;
   * `quads culled` near zero -> the level's visible density is not the issue.

## Not done, honestly

* No in-game launch, FPS or 1% low measurement exists for v2.4.0. The per-quad filter's
  correctness and gain are argued from the engine headers and the fail-open gates, not
  observed.
* No Jawbreaker `shader-trace` / `gd-trace` log has been captured.
* The latest local build of any version is v2.3.0; v2.4.0 source has not been compiled on this
  machine (no local CMake/MSVC/Windows SDK).

## Open work, in priority order

1. **Build v2.4.0 in CI** and fix any compile errors. Then run the trace procedure above.
2. **Visual A/B on a dense level** with `batch-culling` on/off: screenshots or a recording,
   same position, to prove no quad that should paint is missing.
3. **GPU frame time** is implemented, on by default, and still unmeasured on a known-heavy
   level.
4. **Size**: the v2.3.0 DLL was 194,560 bytes. A 1 MB build is not a goal this project can
   honestly hit without padding or extra features; do not pad. If size must grow, add real
   functionality, not filler.
5. **Mobile**: `gd.win` only; no mobile build exists.
6. **Licensing hygiene to keep**: the GPL-3.0 project with the anti-AI clause is reference
   only, never copied, and is not named in release text. All code here is original.

# Resume notes - 2026-11-11 (v2.0.0)

Written as the hand-off for the v2.0.0 build. Nothing here is a claim of work done in-game;
every line below is checkable in the repo or in the CI run list.

## Where the project stands

* Version: **v2.0.0**, branch `v2.0.0-rewrite`, release tag `v2.0.0`.
* Built by GitHub Actions (Windows) — see Actions; the local build toolchain on this machine
  has no CMake/MSVC, so CI is the only compiler check that exists here.
* No in-game testing was done for v1.4.2, v1.5.x, v1.6.x or v2.0.0. The only in-game numbers
  in the report remain the v1.2.x and v1.4.0 runs.
* Installed locally: `C:/Users/akira/Documents/Games/Geometry Dash/geode/mods/` holds only
  the released `ziyyhhk.minimum-reborn.geode` (v2.0.0); the old v1.6.1/v1.6.2 duplicates were
  moved to `mods_disabled_by_test/`.

## What to test first, in one minute

1. Set the profile to **Performance** (or toggle **batch-culling** on the Culling tab), play a
   level that scrolls past big chunks, and watch the **Batches** row change to
   `N empty / K culled / M drawn`. `K` should only move when whole object layers are off screen.
   The level must look **identical**. If anything is missing from a screen edge, turn batch
   culling off — that is the bug this feature was redesigned around, and the fail-open logic
   should make it impossible, but only the game can prove it.
2. Run a 10 s **Bench** on a level with off-screen chunks. The log line now ends with
   `batches N (M quads, K culled)`.
3. Diagnostics page -> **Spikes / draw cost**: if `worst frame` is large while `worst drawScene`
   stays small, the stall is outside the draw path and this mod cannot fix it (unchanged).
4. The particle cull now uses live particle data — effect-heavy levels may skip more than
   before. If a particle effect visibly freezes, report it; that path is `particle-cull-mode`
   on the Culling tab and the `cull-offscreen-particles` switch.

## Open work, in priority order

1. **In-game validation of v2.0.0.** Batch cull and the live-particle cull are argued from the
   cocos2d-x 2.2 headers (`CCTextureAtlas::m_bDirty`, `m_bManualSortAllChildrenDirty`,
   `getTotalQuads()` == submitted quads, `m_uParticleCount` / `m_pParticles`). No launch has
   happened yet.
2. **Mobile is still not real.** `mod.json` declares `gd.win` only. Needs CI jobs and a device.
3. **Settings with no panel control**: `target-fps`, `render-gate-fps`, `background-fps`,
   `overlay-scale`, `overlay-opacity`, `benchmark-seconds`, `disable-power-throttling` —
   Geode settings page only.
4. **Licensing hygiene to keep**: `iliasHDZ/Bismuth` is GPL-3.0-or-later with a clause banning
   AI-assisted GD level generation/modification. Reference only - no code, shaders or assets
   from it are in this project, and that must stay true.

## Known small inaccuracies still in the tree

* `mod.json` does not mark `low-latency` as Windows-only in metadata (the description says it,
  and the panel hides the switch on other platforms); Geode settings have no per-platform flag.
* `DEVELOPMENT_REPORT.md` documents the v1.x development story and keeps v1.5.0's batch cull as
  "removed" — the resolution lives in `SHADER_CULLING.md` section 9 and this file.

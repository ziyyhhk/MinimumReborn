# Resume notes - 2026-11-11 (v2.1.0)

Written as the hand-off for the v2.1.0 build. Nothing here is a claim of work done in-game;
every line below is checkable in the repo or in the CI run list.

## Where the project stands

* Version: **v2.1.0**, branch `v2.1.0-suppression`, merged to `main` via PR.
* Built by GitHub Actions (Windows, Geode SDK pinned to 5.11.2).
* No in-game testing was done for v1.4.2, v1.5.x, v1.6.x, v2.0.0 or v2.1.0. The only in-game
  numbers in the report remain the v1.2.x and v1.4.0 runs.
* Local install (`C:/Users/akira/Documents/Games/Geometry Dash/geode/mods/`) holds the released
  build; the old v1.6.1/v1.6.2 duplicates live in `mods_disabled_by_test/`.

## What to test first, in one minute

1. Set the profile to **Performance** (or toggle **batch-culling** on the Culling tab), play a
   level that scrolls past big chunks, and watch the **Batches** row change to
   `N empty / K culled / M drawn`. `K` should only move when whole object layers are off screen.
   The level must look **identical**. If anything is missing at a screen edge, turn batch
   culling off.
2. **`shader-state-cache`** (Geode settings page, restart-free): turn it on and watch the
   Detailed overlay's `program binds skipped` counter. If the frame rate moves either way,
   report it — this machine could not measure the per-call query against the bind.
3. **`particle-suppression`** (Geode settings page): turn it on in an effect-heavy level;
   the Detailed overlay shows `sims suppressed` per total. If a particle effect looks frozen or
   pops on re-entry, turn it off and report the level.
4. Diagnostics page -> **Spikes / draw cost**: if `worst frame` is large while `worst drawScene`
   stays small, the stall is outside the draw path and this mod cannot fix it (unchanged).
5. Run a 10 s **Bench**; the log line now ends with `batches N (M quads, K culled)`.

## Open work, in priority order

1. **In-game validation of v2.x.** All the newest systems are argued from the cocos2d-x 2.2
   headers, not from launches. The v2.1.0 hooks (`CCGLProgram::use`, `CCParticleSystem::update`)
   are build-clean and their symbols are exported by the Windows cocos2d DLL, but hook
   installation happens at runtime and nothing here has watched it happen.
2. **Mobile is still not real.** `mod.json` declares `gd.win` only. Needs CI jobs and a device.
3. **Settings with no panel control**: `target-fps`, `render-gate-fps`, `background-fps`,
   `overlay-scale`, `overlay-opacity`, `benchmark-seconds`, `disable-power-throttling`,
   `batch-culling`, `shader-state-cache`, `particle-suppression` — Geode settings page only.
4. **FPS measurement for v2.x.** Nothing in this repo has run; one dense level, before/after,
   same settings and toggle states, is the cheapest meaningful number this project could get.
5. **Licensing hygiene to keep**: `iliasHDZ/Bismuth` is GPL-3.0-or-later with a clause banning
   AI-assisted GD level generation/modification. Reference only - no code, shaders or assets
   from it are in this project, and that must stay true.

## Known small inaccuracies still in the tree

* `mod.json` does not mark `low-latency` / `shader-state-cache` as Windows-only in metadata (the
  descriptions say so, and `shader-state-cache` does nothing off-Windows); Geode settings have no
  blanket per-platform flag beyond `platforms` on a setting, which only `shader-state-cache` uses.
* `DEVELOPMENT_REPORT.md` documents the v1.x development story and keeps v1.5.0's batch cull as
  "removed" — the resolution lives in `SHADER_CULLING.md` section 9 and this file.

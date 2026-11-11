# Resume notes - 2026-11-11 (v2.3.0)

Written as the hand-off for the v2.3.0 release. Nothing here is a claim of work done
in-game; every line below is checkable in the repo or in the CI run list.

## Where the project stands

* Version: **v2.3.0**, branch `v2.3.0-native-settings`, merged to `main` via PR.
* Built by GitHub Actions (Windows, Geode 5.11.2 pinned).
* The custom panel is **deleted**. All settings live on the native Geode settings page;
  the pause menu's MR button opens it (`openSettingsPopup`). The corner performance
  overlay (`StatsHud`) is the only remaining UI of the mod.
* Local install holds the released v2.3.0 build; v2.2.0 (the ugly panel build) and earlier
  builds are in `mods_disabled_by_test/`.
* Hooks install clean at runtime (14:13 log): all 10 hooks including `CCGLProgram::use`
  and `CCParticleSystem::update`.

## What to test first, in one minute

1. Pause the level, press **MR**. The native settings page opens; `Profile`, `Renderer Mode`,
   `batch-culling` and friends are all there; `Reset All` works. Close the pause and check the
   overlay shows the live counters.
2. FPS in The Jawbreaker with `Extreme` profile (batch cull on): 1% low before/after; the
   overlay's `gpu` line tells you whether this level is GPU-bound.
3. `shader-trace` (Geode settings page): enable, play ~30 s, quit, send the log - it prints
   `shader-trace:` lines every 5 s. The fix for the Jawbreaker lag still waits on this.

## Not done, honestly

* No launch verified v2.3.0 (CI build only); no FPS/1% low number exists for any v2.x build.
* The screenshot you sent showed the panel broken (white box over the selected profile) -
  removing it is the fix for that report.

## Open work, in priority order

1. **The Jawbreaker trace.** The shader path for the 2.2 gameplay triggers has not been
   measured; until it is, any "fix" would be a guess.
2. **GPU frame time** is implemented and on by default, unmeasured.
3. **Mobile**: `gd.win` only; no mobile build exists.
4. **Licensing hygiene to keep**: Bismuth is GPL-3.0-or-later with an anti-AI clause; reference
   only, never copied.

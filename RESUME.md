# Resume notes - end of 2026-10-30 session

Written when the owner said stop and continue tomorrow. Nothing here is a claim of work done;
every line below is checkable in the repo or in the CI run list.

## Where the project stands

* Version: **v1.6.2**, HEAD pushed to `ziyyhhk/MinimumReborn`, latest CI run green.
* Installed and identical to the repo copy:
  `C:/Users/akira/Documents/Games/Geometry Dash/geode/mods/ziyyhhk.minimum-reborn.geode`
  (sha256 prefix at the time of writing: `e2e9b7fee371`, 296,368 bytes).
* Source snapshot: `../MinimumReborn-Source.zip` (31 entries, built from HEAD with
  `git archive --format=zip --prefix=MinimumReborn/ -o ../MinimumReborn-Source.zip HEAD`).
* No in-game testing was done for v1.4.2, v1.5.x or v1.6.x - the owner asked for code work and
  said they would test. The only in-game numbers in the report are the v1.2.x and v1.4.0 runs.

## What to test first, in one minute

1. Input page -> **Low Latency (drain GPU queue)**. This is the mechanism the original Minimum
   actually uses (`glFinish()` after the frame is submitted). Expect clicks to feel sooner;
   expect average FPS to possibly drop, especially with vsync on. If it feels worse, turn it
   back off - it is a trade, not a free win.
2. Diagnostics page -> **Spikes / draw cost** and **Last benchmark**. Run a 10 s benchmark in a
   level that lags. If `worst frame` is large while `worst drawScene` stays small, the stall is
   outside the draw path and this mod cannot fix it.
3. Culling page -> the four cycle/toggle buttons now write real settings (they were dead in
   v1.6.1). Confirm the label changes and the Geode settings page agrees.

## Open work, in priority order

1. **Mobile is still not real.** `mod.json` declares `gd.win` only and CI builds Windows only.
   The original Minimum ships win/mac/android/ios, so a mobile build is possible, but it needs
   CI jobs for those platforms and a device test. Nothing in the code is unguarded for them
   (the Windows-only timing and priority features sit behind `GEODE_IS_WINDOWS`), but
   "compiles elsewhere" has never been verified.
2. **The batched level-object path is untouched.** Since v1.5.1 the whole-batch visibility test
   is removed (see `SHADER_CULLING.md` section 8 for the two failure modes that killed it). Any
   future attempt has to settle, in the actual GD binary, whether `CCSpriteBatchNode::draw()`
   assembles the vertex buffer - if it does, skipping the draw freezes the buffer and the cull
   becomes self-sustaining. That question needs one instrumented run, not more code.
3. **Settings with no panel control**: `target-fps`, `render-gate-fps`, `background-fps`,
   `overlay-scale`, `overlay-opacity`, `benchmark-seconds`, `disable-power-throttling`. They
   work, but only through the Geode settings page. A slider row type in the panel would cover
   them.
4. **Never measured**: non-zero scene/particle cull counts in a live level, any latency or FPS
   benefit of the new mode, and the editor counters (the editor entry point was reverted in
   v1.4.2 because opening that menu crashed - the faulting module was `hjfod.betteredit`, no
   frame from this mod in the stack).
5. **Licensing hygiene to keep**: `iliasHDZ/Bismuth` is GPL-3.0-or-later with a clause banning
   AI-assisted GD level generation/modification. Reference only - no code, shaders or assets
   from it are in this project, and that must stay true.

## Known small inaccuracies still in the tree

* The in-game `changelog.md` inside the `.geode` is correct as of v1.6.2, but `about.md` still
  describes the mod at a higher level than the settings list; worth a refresh if it is ever
  published to the repository.
* `mod.json` does not mark `low-latency` as Windows-only in metadata (the description says it,
  and the panel hides the switch on other platforms); Geode settings have no per-platform flag,
  so this is as far as it can be documented.

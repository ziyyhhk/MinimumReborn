# Changelog

## v1.0.0 (2026-10-30)

First build.

* Sprite draw fast-out for sprites that cannot produce a pixel (opacity 0, zero scale,
  empty content size).
* Particle fast paths: idle-system draw skip, off-screen culling with a conservative reach
  bound, pool-size cap applied before allocation, optional per-frame spawn budget.
* Optional render gate: draws at Render FPS / Background FPS while the scheduler keeps the
  game's normal rate. Off by default; experimental.
* Performance overlay with FPS, average/p95 frame time, 1% low, jitter, logic rate, draw CPU
  time, skipped-draw counters and a bottleneck hint. One label, four updates per second.
* Pause menu panel with profile selection, quick toggles, reset and a link to the native
  settings page.
* Windows system tuning: 1 ms timer resolution, power-throttling opt-out, optional priority.
* Settings cached in a snapshot; no setting lookups in draw paths. No input-path code.

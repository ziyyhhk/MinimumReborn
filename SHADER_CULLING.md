# Shader-related culling in Minimum Reborn

The Phase 5 brief asks the implementation to identify seven things about its shader-related
optimization instead of shipping a toggle with a convincing name. This file answers them for
v1.5.0, and states plainly where the engine does not allow what was asked.

## 1. Which shader-related operations are avoided

Every skip in this mod returns **before** the original cocos2d `draw()` runs. For a skipped
`CCSprite` or `CCParticleSystemQuad`, the work that therefore never happens is:

* `CCGLProgram` activation for that node (`useProgram` plus its uniform setup - texture,
  color and alpha attributes, the model-view-projection uniform);
* the vertex attribute pointer setup for the quad buffer;
* the texture bind;
* the draw call itself for that node's four vertices;
* for a particle system, the per-frame rebuild and upload of its quad buffer;
* for a skipped batch (v1.5.0), the same sequence once for the whole batch, plus the
  rasterization of every triangle in it.

That is the honest content of "avoiding shader setup for objects that will not be rendered".

## 2. Where the culling decision is made

| Decision | Function (`src/rendering.cpp`) | Runs |
|---|---|---|
| Zero-pixel sprite skip | `SpriteDrawFastPath::draw` | per sprite, at draw time |
| Off-screen sprite skip | same, via `spriteOffScreen` | per sprite, at draw time |
| Idle / zero-opacity / off-screen particle skip | `ParticleDrawFastPath::draw` | per particle system |
| Empty batch skip | `BatchNodeTelemetry::draw` | per batch node |
| Whole-batch off-screen skip (opt-in) | `batchOffScreen`, called from `BatchNodeTelemetry::draw` | per batch node |
| Viewport snapshot used by all of them | `renderFrameBegin` | once per frame |
| Editor vs gameplay policy | `renderFrameBegin` (one `LevelEditorLayer::get()` per frame) | once per frame |

No decision is taken inside an input callback, a scheduler callback, or a node's `update`.

## 3. How visibility is determined

* **Viewport**: `CCDirector::getVisibleSize()` plus `getVisibleOrigin()`, snapshotted once per
  frame. If it is unusable, every test answers "draw".
* **Sprites**: a circle centered on the node position whose radius covers the whole diagonal
  of `contentSize * scale`, so any anchor point, any rotation and any scale is inside it.
  Compared against the viewport expanded by the configured margin, using squared distance
  against squared radius (no square root in the hot path).
* **Parent transforms**: the node's world basis (origin plus two unit vectors) is computed with
  three `convertToWorldSpace` calls per parent per frame, memoized in a 1024-slot direct-mapped
  table with a one-entry sibling memo. Nothing is kept across frames.
* **Particle systems**: relative-position systems only. The bound is the worst-case reach -
  speed plus speed variance, life plus life variance, gravity, radial and tangential
  acceleration, start/end radius, position variance, source offset and particle size - scaled
  to world units. Free and grouped systems are always drawn because no safe bound exists. A
  center-inside shortcut keeps the common case at one circle test.
* **Batches (opt-in)**: the axis-aligned box of the live vertex buffer that `visit()` filled
  this frame, built from all four vertices of every quad, whose four corners are then
  transformed by the batch node's world basis and tested against the viewport with a margin
  floor of 64 world units. The buffer is scanned in full or not at all under a fixed per-frame
  budget of 24,000 quads; a batch that cannot be measured exactly is drawn.
* **Anything uncertain** - missing parent, missing atlas, empty or degenerate bounds,
  non-finite numbers, unknown position type - answers "draw".

## 4. Which object types are supported

Supported: `CCSprite` (which includes glow sprites, decorations, trails and any node that uses
the sprite draw path), `CCParticleSystemQuad`, `CCSpriteBatchNode`.
Not supported, by design: `CCLabelBMFont` internals, texture cache operations, spine/ffmpeg and
other custom renderer nodes, and any drawing that does not go through those three functions.

## 5. Which operations remain unchanged

Physics, collision, triggers, portals, object updates, scheduler order, node visibility flags,
z-order, draw order, blending functions, glow, clipping, shaders, texture loading, atlas
contents and particle simulation. Every draw that is not skipped goes through the original
cocos2d function in the original order. The mod does not compile, replace, patch or cache any
shader program.

## 6. How visual correctness is preserved

A draw is skipped only when the submission cannot produce a pixel this frame:

* rendered opacity 0, scale 0 on either axis, or an empty content size - the quad covers zero
  pixels or multiplies to zero alpha, which contributes nothing under either of cocos2d's
  standard blend modes;
* entirely outside the viewport plus a margin, using a bound that contains the object at any
  rotation and scale;
* a batch whose measured box contains every vertex it would have submitted.

Nothing is reordered or merged, so transparency and glow ordering cannot change. `visit()`
still runs on every node, so animations, triggers, children and physics are untouched even when
a draw is skipped - which is why an object that scrolls back on screen looks identical.

## 7. What measurements demonstrate it

* The panel's Culling and Render pages report `skipped / tested` per second and per session,
  and `culled / measured / drawn` for batches. A counter only moves when a `draw()` call is
  genuinely not submitted, so it counts skipped submissions rather than estimating savings.
* The Diagnostics page reports frames over 25 ms, worst frame and worst `drawScene` time, so a
  spike can be attributed to the draw path or ruled out.
* The in-game benchmark records average, p50, p95, 1% low, variability, CPU draw time and the
  counter deltas over the same window, and writes them to the Geode log.
* Measured so far (Stereo Madness, 60 FPS cap, 10 s per run): identical FPS either way, the
  optimized path costing +0.07 ms CPU per frame while removing 1,830 zero-pixel sprite draws
  per 10 s, with 0 scene and 0 particle culls because every level object was batched. That last
  number is the reason batch culling exists; its own gain is **not yet measured in a live
  level**, and it ships off by default for exactly that reason.

## 8. The limitation, stated plainly

Geometry Dash renders through cocos2d-x with a per-texture-atlas batch per layer. A mod cannot
do what Sodium does for Minecraft chunks - rebuild the geometry into visibility-aware sections,
upload once and re-upload only what changed - because GD owns that buffer, rebuilds only dirty
children, and would render stale geometry if a mod rewrote the quad array. The strongest
defensible statement this file can make is that this mod removes draw submissions and their
shader setup for work that cannot be seen, measures the removal, and leaves GD's batching,
ordering and shaders alone. Anything beyond that would be a claim, not an implementation.

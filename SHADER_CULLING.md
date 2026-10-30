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
* for an empty batch, the program bind and the zero-quad submission that would follow it.

That is the honest content of "avoiding shader setup for objects that will not be rendered".

## 2. Where the culling decision is made

| Decision | Function (`src/rendering.cpp`) | Runs |
|---|---|---|
| Zero-pixel sprite skip | `SpriteDrawFastPath::draw` | per sprite, at draw time |
| Off-screen sprite skip | same, via `spriteOffScreen` | per sprite, at draw time |
| Idle / zero-opacity / off-screen particle skip | `ParticleDrawFastPath::draw` | per particle system |
| Empty batch skip | `BatchNodeTelemetry::draw` | per batch node |
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
* **Batches**: only the empty case. A whole-batch visibility test was implemented in v1.5.0
  and removed in v1.5.1; section 8 explains the two failure modes the review found.

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

* local opacity 0, scale 0 on either axis, or an empty content size - the quad covers zero
  pixels;
* displayed (inherited) opacity 0, and only when `blendFunc.src == GL_SRC_ALPHA`, i.e. the
  blend itself multiplies the source by its alpha. Premultiplied blends use `GL_ONE` there and
  a program that ignores vertex alpha could still paint, so those nodes are drawn;
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

## 8. The batch-level test that was built and then removed

Level objects go through `CCSpriteBatchNode`, which submits one draw for many children and
never calls `CCSprite::draw` for them. That is where the remaining draw work in a level
actually lives, so v1.5.0 added a whole-batch visibility test: measure the axis-aligned box of
the atlas vertex buffer, transform its four corners through the batch node's world basis, and
skip the batch's single draw call when the result is entirely outside the viewport.

A review of that code found two failure modes that cannot be ruled out from inside a mod, and
both hide entire pieces of a level rather than one sprite:

1. The measured range can be narrower than the submitted range. The scan covers
   `getTotalQuads()` slots, while the engine can submit more entries than that: a child added
   through `addSpriteWithoutQuad` exists in the descendant list with no quad, and slots past
   `totalQuads` can still hold stale geometry from removed objects. Any of that can lie outside
   the measured box, so a visible batch gets culled.
2. Skipping the assembly can become self-sustaining. In cocos2d-x 3 and later the batch node
   assembles its vertex buffer inside `draw()`. If GD's fork does the same, skipping that
   `draw()` also skips the rebuild, so the next frame measures a buffer frozen at the moment of
   the cull - and a batch whose children have since scrolled into view can stay culled
   indefinitely, with nothing inside the mod able to notice.

Which case applies depends on GD's exact fork, and settling it needs an in-game test on a level
with off-screen batches. The options were therefore to ship it off by default and let the first
player discover an invisible-piece-of-level bug, or remove it. It is removed. What remains for
batches is the empty-buffer skip, which cannot hide anything.

## 9. The limitation, stated plainly

Geometry Dash renders through cocos2d-x with a per-texture-atlas batch per layer. A mod cannot
do what Sodium does for Minecraft chunks - rebuild the geometry into visibility-aware sections,
upload once and re-upload only what changed - because GD owns that buffer, decides when it is
dirty, and would render stale geometry if a mod rewrote it or skipped its assembly. The strongest
defensible statement this file can make: this mod removes draw submissions and their shader
setup for work that cannot be seen - per sprite and per particle system, where the node's own
transform is available and the bound is exact - measures the removal, and leaves GD's batching,
ordering and shaders alone. Anything beyond that would be a claim, not an implementation.

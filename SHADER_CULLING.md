# Shader-related culling in Minimum Reborn

The Phase 5 brief asks the implementation to identify seven things about its shader-related
optimization instead of shipping a toggle with a convincing name. This file answers them for
v2.4.0 (the current shader-culling behaviour). Sections 9-11 describe the earlier whole-batch
and program-bind attempts and are kept as history; section 12 is the per-quad filter that
replaced the whole-batch skip, and states plainly where the engine does not allow what was
asked.

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
| Per-quad batch filter | `BatchNodeTelemetry::draw` + `filterBatchQuads` | per batch node, this frame's vertices |
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
* **Batches**: every quad is projected through the current P * MV. A quad is left out of the
  index list when its four projected corners have zero area (GD's hidden objects) or when the
  whole projected box is outside the clip volume plus 0.005 NDC. Kept quads stay in atlas
  order. Custom shaders, active grids, non-screen-space projections, non-finite data, more
  than 16,383 quads and an exhausted scan budget all fall back to the original draw. Section
  12 has the full gate list.

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
* a batch quad whose projected corners are collinear (zero area) or whose whole projected box
  lies outside the clip volume; kept quads keep their exact vertices and atlas position.

Nothing is reordered or merged, so transparency and glow ordering cannot change. `visit()`
still runs on every node, so animations, triggers, children and physics are untouched even when
a draw is skipped - which is why an object that scrolls back on screen looks identical.

## 7. What measurements demonstrate it

* The Detailed overlay reports `batch culls`, `quads culled`, `batch scans` and `fully culled`
  for the batch path, plus the per-second skip rates for the sprite and particle hooks. A
  counter only moves when a `draw()` call is genuinely not submitted, or a quad is genuinely
  omitted from the index list, so it counts real removals rather than estimated savings.
* The Detailed overlay shows average/p95 and the 1% low frame time and, when `gpu-frame-time` is
  on, the GPU frame-time line, so a spike can be attributed to the draw path, the GPU, or ruled
  out.
* The `shader-trace` setting adds `shader-trace` and `gd-trace` log lines every 5 s: program
  binds per batch, and wall-clock timings for visibility, the shader layer, `ShaderLayer::visit`
  and the batch scan. It is a diagnostic and is off by default.
* Measured so far (Stereo Madness, 60 FPS cap, 10 s per run): identical FPS either way, the
  optimized path costing +0.07 ms CPU per frame while removing 1,830 zero-pixel sprite draws
  per 10 s, with 0 scene and 0 particle culls because every level object was batched. That
  number is why v2.4.0 filters quads inside the batch itself; it is not a measurement of
  v2.4.0, and nobody has watched a v2.4.0 build render a level yet, so no FPS claim is made.

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

Which case applies depends on GD's exact fork. Section 9 settles it from the same headers the
binary was built against: the VBO is only authoritative when `m_bDirty` is clear, and
`drawQuads()` submits exactly `getTotalQuads()` quads. What remains common to both eras of
this mod is the empty-buffer skip, which cannot hide anything.

## 9. v2.0.0: the batch path, closed properly

> **Superseded in v2.4.0 by section 12.** The whole-batch skip is gone; quads are filtered
> individually and the batch's single GL draw is kept. Section 9 is the design that made the
> batched path reachable without freezing the atlas.

Section 8 removed the whole-batch cull because two failure modes could not be ruled out. Both are
now closed by construction, using the cocos2d-x 2.2 headers this binary was built against
(`loader/include/Geode/cocos/...` in the SDK this mod compiles against), not by assumption:

1. "The scan covers fewer entries than draw() submits." `CCTextureAtlas::drawQuads()` draws
   `m_uTotalQuads` quads and nothing else, and `getTotalQuads()` is that same count. Children
   added via `addSpriteWithoutQuad` never receive an atlas slot at all; they draw through their
   own `CCSprite::draw` path, which this change does not touch. The scan covers `[0, total)`
   and all four corners of every quad in the batch node's local space, and the local AABB is
   converted to a rotation-invariant circle mapped through the batch's world basis, so a rotated
   batch cannot escape its own bound.
2. "Skipping the assembly can become self-sustaining." The engine marks
   `CCTextureAtlas::m_bDirty` whenever the VBO needs an upload. The hook never skips a batch
   whose atlas `isDirty()` is true, nor one whose `m_bManualSortAllChildrenDirty` is set (a
   pending sort rebuilds the buffer inside the draw). A skipped frame therefore leaves the next
   frame byte-identical to the one the game would have produced: same quads, same order.

Two further bounds: a per-frame scan budget of 24,000 quads caps the CPU cost of the feature
(once spent, batches are drawn untested), and any missing or non-finite data answers "draw".
At the time, the whole-batch cull was opt-in (`batch-culling`, off by default; on in
Performance/Extreme) and is counted in its own `batchCulls` counter. Like every draw skip in this mod the effect is
measured, not assumed. v2.4.0 turns `batch-culling` on in Balanced and shows the counters on
the Detailed overlay.

The particle cull was rebuilt the same way. It now measures the live particle AABB from
`m_pParticles` every frame (Relative and Grouped systems - both live in emitter-local space),
which is rotation-invariant and exactly tight to this frame's own data. The old worst-case reach
cone could only reject a system when every possible future position was off screen, which is
rarely true. Free systems remain drawn: their absolute frame has no transform safe to assume.

## 10. The limitation, stated plainly

> **v2.4.0 note:** section 12 replaces the whole-batch skip with per-quad filtering. A batch is
> normally drawn with one call, but individual quads that cannot paint are omitted from the
> index list. The batch buffer is still never rewritten, reordered or merged.

Note: section 9 (v2.0.0) adds an opt-in, fail-open whole-batch skip on top of what is said
there. The batch buffer is still never rewritten, reordered or merged - a batch is either drawn
in full or not drawn at all, and only when its own buffer is provably in sync.

Geometry Dash renders through cocos2d-x with a per-texture-atlas batch per layer. A mod cannot
do what Sodium does for Minecraft chunks - rebuild the geometry into visibility-aware sections,
upload once and re-upload only what changed - because GD owns that buffer, decides when it is
dirty, and would render stale geometry if a mod rewrote it or skipped its assembly. The strongest
defensible statement this file can make: this mod removes draw submissions and their shader
setup for work that cannot be seen - per sprite and per particle system, where the node's own
transform is available and the bound is exact - measures the removal, and leaves GD's batching,
ordering and shaders alone. Anything beyond that would be a claim, not an implementation.

## 11. v2.1.0: redundant program binds (shader-state-cache)

> **Retired in v2.4.0.** cocos2d already routes every bind through `ccGLUseProgram`, whose
> `CC_ENABLE_GL_STATE_CACHE` path skips an unchanged program. The extra per-bind GL query was
> redundant and is removed. The setting key stays in `mod.json` as an inert legacy key; the
> `programBindsSkipped` counter is gone and only trace-only bind counting remains.

The per-node GL program churn happens inside `CCGLProgram::use()`. The hook asks the driver for
`GL_CURRENT_PROGRAM` on every call and skips `glUseProgram` only when it matches the program's
`m_uProgram`. There is no cached assumption or tracking table: the GL state itself is the
oracle, so a bind made anywhere outside the hook is picked up by the very next `use()`. The
`bound > 0` guard keeps an unlinked program from silencing the original call. Cost is one
driver query per candidate; whether that beats the bind is not measured, which is why it ships
off by default with a `programBindsSkipped` counter on the Detailed overlay.

Not claimed, deliberately: no texture-bind elimination (cocos2d's ccGLStateCache already dedups
`glBindTexture(0, ...)` calls), no program sorting or batching (blending/z-order; see section 8
and the section 10 note), no shader precompilation at load (`CCShaderCache` already preloads the
standard programs at engine init).

Section 9 added the whole-batch draw skip and v2.1.0 adds a redundant-bind skip and off-screen
particle simulation suppression; the invariant is unchanged: a batch is drawn in full or not at
all, particles are simulated or not at all, and nothing is ever rewritten, reordered or merged.

## 12. v2.4.0: per-quad filtering inside the batch (current behaviour)

The whole-batch skip removed a submission only when *every* quad in the batch was off screen.
On a shader-heavy level the batch is on screen while many of its quads are not: GD's own
visibility pass collapses hidden objects to four `(0,0,0)` vertices, and those quads are still
uploaded, shaded and discarded. The per-quad filter attacks exactly that.

**What is avoided.** For each dropped quad: the vertex shading for four vertices and the two
triangles they would form. The batch's program bind, attribute setup, texture bind and single
`glDrawElements` still happen when at least one quad survives; the saving is per quad, not per
batch. When no quad survives, the GL submission is skipped entirely.

**How the decision is made.** Inside `CCSpriteBatchNode::draw`, after the child
`updateTransform` pass (the same place the original runs it), the mod reads the atlas quads and
projects all four corners through the current projection and modelview matrices. A quad is
dropped when:

* its projected area is zero (collinear or collapsed corners), or
* its whole projected box is outside `[-1 - 0.005, 1 + 0.005]` in NDC on either axis.

Surviving quads are written to a static `GLushort` index array in atlas order and submitted
with a client-side index pointer after unbinding the element buffer. The vertex buffer is
uploaded for the full `[0, total)` range whenever the atlas is dirty, exactly as the original
draw would, so a later unfiltered draw sees identical bytes. The index VBO is never modified.

**The gates (all fail-open).** The filter runs only when all of these hold:

* the batch's shader program is the stock `kCCShader_PositionTextureColor` (a custom shader can
  displace vertices, so its quads are never judged by their original positions);
* no active `CCGridBase` on the batch (a grid redirects rendering);
* no pending `m_bManualSortAllChildrenDirty` (a sort rebuilds quad order inside draw);
* a screen-space projection: finite matrix and a w row of `(0,0,0,1)`;
* `1 <= total <= 16,383` and `total <= capacity` (16-bit indices), with non-null quads,
  texture and both VBO names;
* the per-frame scan budget (24,000 quads) still covers this batch.

Any failed check, non-finite coordinate, `w <= 0` or unusual projection draws the batch
untouched through the original tail. The filter never allocates, never reorders and never
changes blend state.

**Why this is not "rewriting GD's buffer".** The atlas vertices and the atlas index VBO are
untouched. The only new data is the mod's own index list, used for one draw call, and the normal
dirty upload is preserved byte-for-byte. GD's next frame starts from the same state it would
have had without the mod.

**What is measured.** `batchScans`, `batchCulls` (batches with at least one quad removed),
`batchFullyCulled` and `batchQuadsCulled`; the Detailed overlay shows quads culled, batch
scans and fully culled batches.
The `gd-trace` shader-trace line adds the scan's average time. **No in-game run has measured
the pixel or frame-time effect yet**, so no FPS claim is made. The claim is the mechanism: a
non-visible quad is not submitted, and the counters prove when that happened.

**The remaining limit.** Quads are filtered, not merged: the batch still issues one draw per
texture atlas, and GPU fragment cost for visible quads is unchanged. If the frame is
fragment-bound (large glow, full-screen shader effects) this filter cannot help, which is why
the GPU line and the `gd-trace` CPU timings ship together: they decide where the next change
belongs.

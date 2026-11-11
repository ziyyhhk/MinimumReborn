#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <gl/GL.h>
#endif
#include <Geode/modify/CCGLProgram.hpp>
#include <Geode/modify/CCParticleSystem.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include "config.hpp"
#include "rendering.hpp"

using namespace geode::prelude;

// Optimized draw path
//
// Everything in this file runs inside the game's normal draw traversal. Each function can
// skip a draw submission; none of them changes transforms, updates, physics, timers or input.
// With renderer mode = Engine (or the master switch off) every hook calls the original
// cocos2d function unchanged.
//
// ---------------------------------------------------------------------------------------
// 1. CCSprite fast-out
// ---------------------------------------------------------------------------------------
// CCSprite::draw always sets up the quad, updates the model-view-projection uniform, binds
// the program/texture and submits a draw call. Sprites that cannot produce a pixel still pay
// all of it:
//   * opacity 0              (faded-out decorations, hidden effect layers)
//   * scale 0 on either axis (collapsed sprites)
//   * empty content size     (placeholder sprites with no quad)
// Skipping draw() for exactly those cases cannot change the image: the quad covers zero
// pixels, children are drawn by visit() independently of the parent's draw(), and no shared
// state is set up for anyone else.
//
// ---------------------------------------------------------------------------------------
// 2. Scene visibility culling (sprites)
// ---------------------------------------------------------------------------------------
// A sprite whose whole quad is outside the viewport (plus a margin) cannot contribute a
// pixel. The test is done per sprite, in world coordinates:
//
//   * the parent's world basis (origin + two unit vectors) is computed once per frame per
//     parent and cached. In a level thousands of objects share one parent, so the expensive
//     convertToWorldSpace walk is paid a handful of times per frame, not per object.
//   * the sprite is reduced to a circle around its anchor point: radius = distance from the
//     anchor to the farthest corner of the unrotated bounding box, scaled by the parent. A
//     rotation happens about the anchor in cocos2d, so that circle covers the sprite at any
//     rotation and any scale - the test can never reject a visible sprite because of rotation.
//   * the circle is tested against the viewport expanded by the configured margin, which
//     absorbs camera movement between one test and the next.
//   * no parent, non-finite values, empty bounds or a degenerate basis all answer "draw".
//
// Only draw() is skipped. visit() still runs, so node updates, animations, triggers, children
// and physics are completely untouched.
//
// ---------------------------------------------------------------------------------------
// 3. Particle systems
// ---------------------------------------------------------------------------------------
// * Idle: no draw at all at zero live particles.
// * Off-screen: a live particle system whose AABB (measured from the actual particle
//   positions of this frame, expanded by the largest particle size and the viewport
//   margin) lies fully outside the visible rect is not drawn. It still updates, so
//   emission timing, lifetimes and motion are unchanged and it looks the same when it
//   scrolls back. Covers the Relative and Grouped position types, whose particle
//   positions live in emitter-local space; the AABB center is mapped to the world with
//   a rotation-safe circle test, so emitter rotation is handled. Free systems are
//   always drawn: their particles live in an absolute frame and no transform is safe
//   to assume. If the emitter itself is on screen the system is drawn without touching
//   the particle data at all.
// * Pool cap: clamping the requested pool BEFORE the original initWithTotalParticles runs
//   means the particle, quad and index buffers are all allocated for the capped size, so no
//   code can ever index past its buffer. Only affects systems created afterwards.
// * Spawn budget: once N systems have been created in one frame during a level, further
//   systems still exist but get a pool of 4, so an effect burst looks thinner instead of
//   stalling the frame. Off by default.
//
// Culling modes: Conservative keeps a generous margin (default). Aggressive keeps a small
// one and is meant for checking how much draw work the culling actually removes. Both are
// draw-only.
//
// ---------------------------------------------------------------------------------------
// 4. Batch culling (the level objects themselves)
// ---------------------------------------------------------------------------------------
// Level objects are submitted through CCSpriteBatchNode::draw, one call per batch. This is
// where the bulk of a level lives, and it was deliberately untouched in every previous
// release because two failure modes existed. Both are closed here, so the whole-batch skip
// now ships behind the `batch-culling` switch (default off, opt-in, measurement first):
//
//   * "the scan covers fewer entries than draw() submits" - read the real member truth
//     instead of assuming: drawQuads() submits exactly getTotalQuads() quads (CCTextureAtlas
//     can draw nothing more), and children added via addSpriteWithoutQuad never got an atlas
//     slot at all - they draw through their own CCSprite::draw path, which this does not
//     touch. The scan reads [0, getTotalQuads()) and all four corners of every quad, so the
//     measured box contains everything the draw would submit.
//
//   * "skipping draw() freezes the VBO and the cull becomes self-sustaining" - the engine
//     marks CCTextureAtlas::m_bDirty when the VBO needs an upload. This hook never skips
//     while that flag is set (the skipped draw is exactly where the upload happens), nor
//     while the batch reports m_bManualSortAllChildrenDirty (a pending sort rebuilds the
//     quad order inside the draw). Only a batch whose buffer is already in sync can be
//     skipped, and then the next frame is identical to this one.
//
// The box scan is also capped: a per-frame budget of 24,000 quads bounds the CPU cost of
// the feature no matter how big the level is; once the budget is spent, batches are drawn
// without further testing until the next frame. Empty batches (zero quads) keep the old
// empty-batch skip, which is unconditional.
//
// Only draw() is skipped; visit(), updates, triggers and remove/add logic are untouched.
//
// ---------------------------------------------------------------------------------------
// 6. What "shader culling" can honestly mean here
// ---------------------------------------------------------------------------------------
// cocos2d-x sets up the shader per node inside draw(): CCGLProgram activation, the vertex
// attribute pointers, the texture bind and only then the draw call. Every skip in this file
// returns *before* that sequence, so the work that is actually avoided per skipped node is:
//   * the program activation and uniform setup (texture, GL state),
//   * the vertex attribute pointers,
//   * the draw call itself (and for a particle system, the per-frame quad buffer rebuild),
//   * the fragment work those triangles would have produced.
// That is the defensible version of "avoiding shader setup for objects that will not be
// rendered", and it is measured: the counters on the Render and Culling pages only move when
// a draw() call is really not submitted.
//
// What is NOT done, because the engine does not allow it safely from a mod:
//   * no shader is compiled, replaced or re-bound, and no GL program cache is kept - the
//     bindings GD uses are per-node and can change under a batch, so caching them would
//     change state that other nodes depend on;
//   * no draw is reordered, merged or re-batched, so blending, glow, clipping and z-order
//     stay exactly as the game produced them;
//   * no object is culled on its center alone - sprites use a rotation-safe circle around
//     the node position covering the whole diagonal, particle systems add their worst-case
//     reach (speed, life, acceleration, radius, position variance and particle size) scaled
//     to world units, and both keep a margin;
//   * batched level objects are not touched: CCSpriteBatchNode submits every child in one
//     call and never runs the per-sprite path, so a mod cannot drop individual quads from it
//     without rewriting the atlas buffer, which would break the next frame's cached geometry.
//
// What this is NOT: a replacement renderer. Draw order, blending, glow, clipping, shaders and
// texture state are untouched because every kept draw still goes through the original
// cocos2d function in the original order.

namespace {

    struct ViewRect {
        double left = 0.0;
        double bottom = 0.0;
        double right = 0.0;
        double top = 0.0;
        bool valid = false;
    };

    ViewRect g_view;
    unsigned g_frameId = 1;
    std::uint64_t g_frameCreatedBase = 0;
    bool g_editorFrame = false;

    // Batch culling scans every quad in the atlas, which is cheap per quad but could add
    // up in a level with giant batches. This is the per-frame CPU budget, in quads. Once
    // it is spent, batches are drawn untested for the rest of the frame - the cost of the
    // feature is capped no matter how big the scene is.
    constexpr std::uint64_t kBatchScanBudget = 24000;
    std::uint64_t g_batchScanBudget = kBatchScanBudget;

    // Local -> world mapping of a node: world = origin + x * ux + y * uy.
    struct Basis {
        CCPoint origin;
        CCPoint ux;
        CCPoint uy;
    };

    struct Slot {
        CCNode const* node = nullptr;
        // A node freed and reallocated at the same address inside one frame would otherwise
        // be handed back the previous occupant's basis, so the entry also carries two cheap
        // identity fields that differ for any different node.
        CCNode const* parent = nullptr;
        CCPoint position{};
        unsigned frame = 0;
        Basis basis;
    };

    // Direct-mapped, rebuilt every frame because transforms change with the camera. A miss
    // overwrites the slot; nothing grows and nothing is kept across frames.
    //
    // 256 slots proved too few: a busy scene walks past the cache line of a parent that is
    // visited again later in the same frame, and every one of those misses costs three
    // convertToWorldSpace calls - the mod's own worst source of extra frame time, in the path
    // that is supposed to remove frame time. Two changes: more slots, and a one-entry memo for
    // the overwhelmingly common case of consecutive siblings sharing a parent, which makes the
    // typical cost of the test a single pointer compare.
    constexpr unsigned kTransformSlots = 1024;
    Slot g_slots[kTransformSlots];

    struct Memo {
        CCNode const* node = nullptr;
        CCNode const* parent = nullptr;
        Basis basis;
    };

    Memo g_memo;
    unsigned g_memoFrame = 0;

    bool isFinite(float value) {
        return std::isfinite(value);
    }

    bool basisOf(CCNode* node, Basis& out) {
        if (!node) return false;

        if (g_memoFrame == g_frameId && g_memo.node == node
            && g_memo.parent == node->getParent()) {
            out = g_memo.basis;
            return true;
        }

        auto const key = reinterpret_cast<std::uintptr_t>(node);
        Slot& slot = g_slots[(key >> 4u) % kTransformSlots];
        bool const same = slot.frame == g_frameId && slot.node == node
                       && slot.parent == node->getParent()
                       && slot.position.x == node->getPositionX()
                       && slot.position.y == node->getPositionY();
        if (same) {
            out = slot.basis;
            g_memo.node = node;
            g_memo.parent = node->getParent();
            g_memo.basis = slot.basis;
            g_memoFrame = g_frameId;
            return true;
        }

        CCPoint const origin = node->convertToWorldSpace(ccp(0.f, 0.f));
        CCPoint const unitX = node->convertToWorldSpace(ccp(1.f, 0.f));
        CCPoint const unitY = node->convertToWorldSpace(ccp(0.f, 1.f));
        if (!isFinite(origin.x) || !isFinite(origin.y)
            || !isFinite(unitX.x) || !isFinite(unitX.y)
            || !isFinite(unitY.x) || !isFinite(unitY.y)) {
            return false;
        }

        Basis b;
        b.origin = origin;
        b.ux = ccp(unitX.x - origin.x, unitX.y - origin.y);
        b.uy = ccp(unitY.x - origin.x, unitY.y - origin.y);
        if ((b.ux.x == 0.f && b.ux.y == 0.f) || (b.uy.x == 0.f && b.uy.y == 0.f)) {
            return false;
        }

        slot.node = node;
        slot.parent = node->getParent();
        slot.position = node->getPosition();
        slot.frame = g_frameId;
        slot.basis = b;
        g_memo.node = node;
        g_memo.parent = node->getParent();
        g_memo.basis = b;
        g_memoFrame = g_frameId;
        out = b;
        return true;
    }

    double basisScale(Basis const& b) {
        return std::max(std::hypot(b.ux.x, b.ux.y), std::hypot(b.uy.x, b.uy.y));
    }

    // Squared version of the same number. The per-sprite hot path never needs the square
    // root: the radius test below compares squared distance against squared radius.
    double basisScaleSquared(Basis const& b) {
        return std::max(
            static_cast<double>(b.ux.x) * b.ux.x + static_cast<double>(b.ux.y) * b.ux.y,
            static_cast<double>(b.uy.x) * b.uy.x + static_cast<double>(b.uy.y) * b.uy.y
        );
    }

    CCPoint toWorld(Basis const& b, CCPoint local) {
        return ccp(b.origin.x + local.x * b.ux.x + local.y * b.uy.x,
                   b.origin.y + local.x * b.ux.y + local.y * b.uy.y);
    }

    // Circle vs viewport expanded by margin. A zero radius tests the center only.
    bool circleTouches(double cx, double cy, double radius, double margin) {
        if (!g_view.valid) return true;
        return cx + radius >= g_view.left - margin
            && cx - radius <= g_view.right + margin
            && cy + radius >= g_view.bottom - margin
            && cy - radius <= g_view.top + margin;
    }

    // Same test without a square root: distance from the circle center to the expanded
    // rectangle, squared, against the radius squared.
    bool circleTouchesSquared(double cx, double cy, double radiusSquared, double margin) {
        if (!g_view.valid) return true;
        double const left = g_view.left - margin;
        double const bottom = g_view.bottom - margin;
        double const right = g_view.right + margin;
        double const top = g_view.top + margin;
        double const dx = cx < left ? left - cx : (cx > right ? cx - right : 0.0);
        double const dy = cy < bottom ? bottom - cy : (cy > top ? cy - top : 0.0);
        return dx * dx + dy * dy <= radiusSquared;
    }

    // True when the whole sprite is outside the expanded viewport. Conservative by
    // construction: any doubt (missing parent, bad numbers, empty bounds) answers false.
    bool spriteOffScreen(CCSprite* sprite, double margin) {
        CCNode* parent = sprite->getParent();
        if (!parent) return false;

        // The sprite's quad is contentSize * scale, placed relative to its anchor point. The
        // anchor is not queried (the binding differs between engines), so the radius covers
        // the whole diagonal measured from the node position: correct for any anchor, any
        // rotation and any scale, at the cost of keeping a few sprites that a tighter bound
        // would reject.
        CCSize const size = sprite->getContentSize();
        if (!(size.width > 0.f) || !(size.height > 0.f)) return false;

        double const scale = std::max(
            std::fabs(static_cast<double>(sprite->getScaleX())),
            std::fabs(static_cast<double>(sprite->getScaleY()))
        );
        double const radiusSquared =
            (static_cast<double>(size.width) * size.width + static_cast<double>(size.height) * size.height)
            * scale * scale;
        if (!std::isfinite(radiusSquared)) return false;

        Basis basis;
        if (!basisOf(parent, basis)) return false;

        CCPoint const center = toWorld(basis, sprite->getPosition());
        if (!isFinite(center.x) || !isFinite(center.y)) return false;

        return !circleTouchesSquared(
            center.x, center.y, radiusSquared * basisScaleSquared(basis), margin
        );
    }

    // True when a particle system whose particles live in emitter-local space cannot paint
    // anything inside the expanded view. Covers Relative and Grouped; Free systems are left
    // alone (their particles are in an absolute frame and no transform is safe to assume).
    //
    // The bound is measured from the actual particle positions this draw will submit - the
    // same data a few calls later, with no time in between - so the test is exact, not a
    // worst case. The local AABB is expanded by each particle's own size; the center is
    // mapped to the world as a circle (rotation-safe for any emitter rotation) and scaled
    // by the emitter's and its parents' scales. Any doubt answers "draw".
    bool particleOffScreen(CCParticleSystem* system, double margin) {
        tCCPositionType const type = system->getPositionType();
        if (type != kCCPositionTypeRelative && type != kCCPositionTypeGrouped) return false;

        CCNode* parent = system->getParent();
        if (!parent) return false;

        Basis basis;
        if (!basisOf(parent, basis)) return false;

        double const ownScale = std::max(std::fabs(system->getScaleX()), std::fabs(system->getScaleY()));
        double const scale = basisScale(basis) * ownScale;
        if (!std::isfinite(scale) || scale <= 0.0) return false;

        CCPoint const world = toWorld(basis, system->getPosition());
        if (!isFinite(world.x) || !isFinite(world.y)) return false;

        // Common case: the emitter is on screen, so a particle is guaranteed to be visible.
        // This keeps the per-frame cost of visible systems to one circle test.
        if (circleTouches(world.x, world.y, 0.0, margin)) return false;

        unsigned int const count = system->m_uParticleCount;
        if (count == 0) return false;  // idle systems go through the idle skip, never through a cull
        tCCParticle const* particles = system->m_pParticles;
        if (!particles) return false;

        double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
        for (unsigned int i = 0; i < count; ++i) {
            CCPoint const& pos = particles[i].pos;
            float const size = particles[i].size;
            if (!isFinite(pos.x) || !isFinite(pos.y) || !isFinite(size)) return false;

            float const pad = size > 0.f ? size * 0.5f : 0.f;
            double const x0 = static_cast<double>(pos.x) - pad;
            double const x1 = static_cast<double>(pos.x) + pad;
            double const y0 = static_cast<double>(pos.y) - pad;
            double const y1 = static_cast<double>(pos.y) + pad;
            if (i == 0) {
                minX = x0; maxX = x1; minY = y0; maxY = y1;
            } else {
                minX = std::min(minX, x0); maxX = std::max(maxX, x1);
                minY = std::min(minY, y0); maxY = std::max(maxY, y1);
            }
        }

        double const centerLocalX = (minX + maxX) * 0.5;
        double const centerLocalY = (minY + maxY) * 0.5;
        double const width = maxX - minX;
        double const height = maxY - minY;
        double const radiusLocal = std::sqrt(width * width + height * height) * 0.5;
        if (!std::isfinite(radiusLocal)) return false;

        CCPoint const center = toWorld(basis, ccp(
            static_cast<float>(centerLocalX), static_cast<float>(centerLocalY)
        ));
        double const radius = radiusLocal * scale;

        return !circleTouches(center.x, center.y, radius, margin);
    }

    // True when every quad in a batch's vertex buffer lies entirely outside the expanded
    // view, so the batch's single GL submission can be skipped. Only called when the atlas
    // is in sync (not dirty, no pending child sort) and the scan budget allows; anything
    // unexpected answers "draw". Quad positions live in the batch node's local space, so
    // the local box is mapped through the batch's own world basis as a circle.
    constexpr double kBatchMargin = 64.0;

    bool batchOffScreen(CCSpriteBatchNode* batch, double margin) {
        CCTextureAtlas* atlas = batch->getTextureAtlas();
        if (!atlas) return false;
        unsigned int const total = atlas->getTotalQuads();
        if (total == 0) return false;  // empty batches use the empty-batch skip
        if (atlas->isDirty()) return false;  // the skipped draw is where the VBO upload happens
        if (batch->m_bManualSortAllChildrenDirty) return false;  // a pending sort rebuilds the buffer inside draw()
        if (g_batchScanBudget == 0) return false;

        ccV3F_C4B_T2F_Quad const* quads = atlas->getQuads();
        if (!quads) return false;

        double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
        for (unsigned int i = 0; i < total; ++i) {
            if (g_batchScanBudget == 0) return false;  // budget spent: draw untested, claim nothing
            --g_batchScanBudget;

            ccV3F_C4B_T2F_Quad const& quad = quads[i];
            float const xs[4] = {
                quad.tl.vertices.x, quad.bl.vertices.x, quad.tr.vertices.x, quad.br.vertices.x
            };
            float const ys[4] = {
                quad.tl.vertices.y, quad.bl.vertices.y, quad.tr.vertices.y, quad.br.vertices.y
            };
            for (int v = 0; v < 4; ++v) {
                if (!isFinite(xs[v]) || !isFinite(ys[v])) return false;
            }
            for (int v = 0; v < 4; ++v) {
                double const x = xs[v];
                double const y = ys[v];
                if (i == 0 && v == 0) {
                    minX = maxX = x; minY = maxY = y;
                } else {
                    minX = std::min(minX, x); maxX = std::max(maxX, x);
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                }
            }
        }

        double const centerLocalX = (minX + maxX) * 0.5;
        double const centerLocalY = (minY + maxY) * 0.5;
        double const width = maxX - minX;
        double const height = maxY - minY;
        double const radiusLocal = std::sqrt(width * width + height * height) * 0.5;
        if (!std::isfinite(radiusLocal)) return false;

        Basis basis;
        if (!basisOf(batch, basis)) return false;
        double const radius = radiusLocal * basisScale(basis);
        CCPoint const center = toWorld(basis, ccp(
            static_cast<float>(centerLocalX), static_cast<float>(centerLocalY)
        ));

        return !circleTouches(center.x, center.y, radius, margin);
    }

    // ---------------------------------------------------------------------------------------
    // TRACE (diagnostic)
    //
    // Everything in this block exists to answer one question: which path do the GD 2.2
    // shader gameplay objects actually take? It only runs while the `shader-trace`
    // setting is on; nothing here allocates and every line of state is fixed-size. The
    // window flushes every 5 s to the Geode log: totals, binds per batch, top 5 GL
    // program objects by bind count. No behavior depends on this block; it only counts.
    // ---------------------------------------------------------------------------------------
    constexpr unsigned kTraceSlots = 48;
    constexpr auto kTraceWindow = std::chrono::seconds(5);

    struct TraceEntry {
        GLuint program = 0;
        std::uint64_t binds = 0;
        std::uint64_t redundant = 0;
    };

    TraceEntry g_trace[kTraceSlots];
    std::uint64_t g_traceBinds = 0;
    std::uint64_t g_traceRedundant = 0;
    std::uint64_t g_traceBatches = 0;
    std::uint64_t g_traceBatchBinds = 0;
    std::uint64_t g_traceMaxBatchBinds = 0;
    std::uint64_t g_traceUntracked = 0;
    unsigned g_traceCurrentBatchBinds = 0;
    bool g_traceInBatch = false;
    std::chrono::steady_clock::time_point g_traceLastFlush = std::chrono::steady_clock::now();

    void traceProgramBind(GLuint program, bool redundant) {
        ++g_traceBinds;
        if (redundant) ++g_traceRedundant;
        for (auto& entry : g_trace) {
            if (entry.program == program) {
                ++entry.binds;
                if (redundant) ++entry.redundant;
                return;
            }
        }
        for (auto& entry : g_trace) {
            if (entry.program == 0) {
                entry.program = program;
                entry.binds = 1;
                entry.redundant = redundant ? 1 : 0;
                return;
            }
        }
        // Table full of live programs: keep the ones already tracked, count this one as lost.
        ++g_traceUntracked;
    }

    void traceNoteBatchStart() {
        g_traceInBatch = true;
        g_traceCurrentBatchBinds = 0;
    }

    void traceNoteBatchEnd() {
        if (!g_traceInBatch) return;
        g_traceInBatch = false;
        ++g_traceBatches;
        g_traceBatchBinds += g_traceCurrentBatchBinds;
        if (g_traceCurrentBatchBinds > g_traceMaxBatchBinds) {
            g_traceMaxBatchBinds = g_traceCurrentBatchBinds;
        }
        g_traceCurrentBatchBinds = 0;
    }

    void traceNoteWindowBind(bool redundant) {
        if (!g_traceInBatch) return;
        if (redundant) return;  // a switch, not a redundant bind, is what matters per batch
        ++g_traceCurrentBatchBinds;
    }

    std::string traceTopPrograms() {
        TraceEntry top[5]{};
        for (auto const& entry : g_trace) {
            if (entry.program == 0 || entry.binds == 0) continue;
            for (int i = 0; i < 5; ++i) {
                if (entry.binds > top[i].binds) {
                    for (int j = 4; j > i; --j) top[j] = top[j - 1];
                    top[i] = entry;
                    break;
                }
            }
        }
        std::string out;
        for (auto const& entry : top) {
            if (entry.program == 0) break;
            if (!out.empty()) out += ", ";
            out += "prog " + std::to_string(entry.program) + " x" + std::to_string(entry.binds);
        }
        return out;
    }

    // Called from renderFrameEnd(); cheap when disabled (one bool + one clock read per frame).
    void traceMaybeFlush(std::chrono::steady_clock::time_point now) {
        if (!reborn::config().shaderTrace) return;
        if (now - g_traceLastFlush < kTraceWindow) return;
        g_traceLastFlush = now;

        // Publish the window totals through the atomic counters the overlay reads.
        g_hot.shaderTraceBinds += g_traceBinds;
        g_hot.shaderTraceRedundant += g_traceRedundant;

        log::info(
            "shader-trace: binds {} (redundant {}), batches {}, binds/batch avg {:.1f} max {}, "
            "untracked {}, top programs [{}]",
            g_traceBinds, g_traceRedundant, g_traceBatches,
            g_traceBatches > 0 ? static_cast<double>(g_traceBatchBinds) / g_traceBatches : 0.0,
            g_traceMaxBatchBinds, g_traceUntracked, traceTopPrograms()
        );

        for (auto& entry : g_trace) entry = TraceEntry{};
        g_traceBinds = 0;
        g_traceRedundant = 0;
        g_traceBatches = 0;
        g_traceBatchBinds = 0;
        g_traceMaxBatchBinds = 0;
        g_traceUntracked = 0;
    }

    // Hot-path counters.
    //
    // The draw hooks run once per sprite and once per particle system, which in a busy level
    // is tens of thousands of times per frame. A std::atomic increment is a locked read-
    // modify-write, so counting per object would make the *instrumentation* one of the more
    // expensive things in the frame - exactly what an optimization layer must not do. The
    // hooks therefore bump plain 64-bit integers here (main thread only, no tearing concern)
    // and renderFrameEnd() folds them into the published atomics once per frame.
    struct HotCounts {
        std::uint64_t invisibleSprites = 0;
        std::uint64_t sceneExamined = 0;
        std::uint64_t sceneSkipped = 0;
        std::uint64_t particleExamined = 0;
        std::uint64_t particleOffscreenSkipped = 0;
        std::uint64_t particleIdleSkipped = 0;
        std::uint64_t particleInvisibleSkipped = 0;
        std::uint64_t editorSpritesExamined = 0;
        std::uint64_t editorSpritesSkipped = 0;
        std::uint64_t editorParticlesExamined = 0;
        std::uint64_t editorParticlesSkipped = 0;
        std::uint64_t batchDraws = 0;
        std::uint64_t batchQuads = 0;
        std::uint64_t emptyBatches = 0;
        std::uint64_t batchCulled = 0;
        std::uint64_t programBindsSkipped = 0;
        std::uint64_t particleSimsSuppressed = 0;
        std::uint64_t shaderTraceBinds = 0;
        std::uint64_t shaderTraceRedundant = 0;
    };

    HotCounts g_hot;

    // Per-frame snapshot of the switches the draw hooks consult. Reading them costs one bool
    // test per node instead of a config lookup plus several field reads, which matters
    // because the sprite hook runs tens of thousands of times per frame. Everything is
    // refreshed once in renderFrameBegin(), on the same thread that draws.
    struct Flags {
        bool any = false;
        bool spriteFastPath = false;
        bool sceneCull = false;
        bool editorCull = false;
        bool particleIdle = false;
        bool particleCull = false;
        bool editor = false;
        bool batchCull = false;
        double sceneMargin = 160.0;
        double editorMargin = 160.0;
        double particleMargin = 128.0;
    };

    Flags g_flags;

    void add(std::atomic<std::uint64_t>& target, std::uint64_t& source) {
        if (source == 0) return;
        target.fetch_add(source, std::memory_order_relaxed);
        source = 0;
    }

    std::uint64_t createdEarlierThisFrame() {
        std::uint64_t const total = reborn::counters().particleSystemsCreated.load(std::memory_order_relaxed);
        return total >= g_frameCreatedBase ? total - g_frameCreatedBase : 0;
    }

    unsigned int limitedPool(unsigned int requested, std::uint64_t createdEarlier) {
        auto const& cfg = reborn::config();
        if (!cfg.enabled || !cfg.rendererOptimized) return requested;

        unsigned int pool = requested;
        if (cfg.spawnBudget > 0 && PlayLayer::get() != nullptr && createdEarlier >= cfg.spawnBudget) {
            pool = std::min(pool, 4u);
        }
        if (cfg.capParticles) {
            pool = std::min(pool, reborn::effectiveParticleCap());
        }
        if (pool < requested) ++reborn::counters().particlePoolsCapped;
        return pool;
    }

}  // namespace

namespace reborn {

    void renderFrameBegin() {
        ++g_frameId;
        {
            auto const& c = config();
            bool const on = c.enabled && c.rendererOptimized;
            g_flags.any = on && (c.spriteFastPath
                              || c.sceneCull != CullMode::Off
                              || (c.cullOffscreenParticles && c.particleCull != CullMode::Off)
                              || c.skipIdleParticles
                              || (c.editorCulling && c.editorCull != CullMode::Off)
                              || c.batchCulling);
            g_flags.spriteFastPath = on && c.spriteFastPath;
            g_flags.sceneCull = on && c.sceneCull != CullMode::Off;
            g_flags.editorCull = on && c.editorCulling && c.editorCull != CullMode::Off;
            g_flags.particleIdle = on && c.skipIdleParticles;
            g_flags.particleCull = on && c.cullOffscreenParticles && c.particleCull != CullMode::Off;
            g_flags.batchCull = on && c.batchCulling;
            g_flags.sceneMargin = c.sceneCullMargin;
            g_flags.editorMargin = c.editorCullMargin;
            g_flags.particleMargin = c.particleCullMargin;
        }
        g_frameCreatedBase = counters().particleSystemsCreated.load(std::memory_order_relaxed);
        // Each frame gets a fresh batch-scan budget; the previous frame's spend does not
        // carry over, so one huge level cannot starve the next.
        g_batchScanBudget = kBatchScanBudget;
        // One cheap scene check per frame, shared by every draw hook. The editor is a
        // separate scene from PlayLayer; both use the same draw hooks, so the culling
        // policy is selected here rather than by installing editor-only hooks.
        g_editorFrame = LevelEditorLayer::get() != nullptr;
        g_flags.editor = g_editorFrame;

        // The visible rect lives in the scene's coordinate space, which is the space
        // convertToWorldSpace() reports into. If it is not usable, every test answers "draw".
        auto* director = CCDirector::get();
        if (!director) {
            g_view.valid = false;
            return;
        }
        CCSize const size = director->getVisibleSize();
        CCPoint const origin = director->getVisibleOrigin();
        bool const usable = size.width > 0.f && size.height > 0.f
                         && isFinite(size.width) && isFinite(size.height)
                         && isFinite(origin.x) && isFinite(origin.y);
        if (!usable) {
            g_view.valid = false;
            return;
        }
        g_view.left = origin.x;
        g_view.bottom = origin.y;
        g_view.right = origin.x + size.width;
        g_view.top = origin.y + size.height;
        g_view.valid = true;
    }

    void renderFrameEnd() {
        auto& c = counters();
        add(c.invisibleSpriteDrawsSkipped, g_hot.invisibleSprites);
        add(c.sceneSpritesExamined, g_hot.sceneExamined);
        add(c.sceneSpriteDrawsSkipped, g_hot.sceneSkipped);
        add(c.particleSystemsExamined, g_hot.particleExamined);
        add(c.offscreenParticleDrawsSkipped, g_hot.particleOffscreenSkipped);
        add(c.idleParticleDrawsSkipped, g_hot.particleIdleSkipped);
        add(c.invisibleParticleDrawsSkipped, g_hot.particleInvisibleSkipped);
        add(c.editorSpritesExamined, g_hot.editorSpritesExamined);
        add(c.editorSpriteDrawsSkipped, g_hot.editorSpritesSkipped);
        add(c.editorParticlesExamined, g_hot.editorParticlesExamined);
        add(c.editorParticleDrawsSkipped, g_hot.editorParticlesSkipped);
        add(c.batchDraws, g_hot.batchDraws);
        add(c.batchQuads, g_hot.batchQuads);
        add(c.emptyBatchDrawsSkipped, g_hot.emptyBatches);
        add(c.batchCulls, g_hot.batchCulled);
        add(c.programBindsSkipped, g_hot.programBindsSkipped);
        add(c.particleSimsSuppressed, g_hot.particleSimsSuppressed);
        add(c.shaderTraceBinds, g_hot.shaderTraceBinds);
        add(c.shaderTraceRedundant, g_hot.shaderTraceRedundant);

        traceMaybeFlush(std::chrono::steady_clock::now());
    }

    unsigned int effectiveParticleCap();  // defined in config.cpp (adaptive step included)

    bool inEditor() {
        return g_editorFrame;
    }

    bool visibilityCullingActive() {
        auto const& cfg = config();
        if (!cfg.enabled || !cfg.rendererOptimized) return false;
        if (g_editorFrame) {
            return cfg.editorCulling && cfg.editorCull != CullMode::Off;
        }
        return cfg.sceneCull != CullMode::Off
            || (cfg.cullOffscreenParticles && cfg.particleCull != reborn::CullMode::Off)
            || cfg.batchCulling;
    }
}

// The sprite hook. This is a real branch in the draw path of every sprite, so it stays small:
// the fast-out is three field reads; the visibility test is one cached basis lookup, one
// circle test and - only on a miss - three convertToWorldSpace calls for the parent.
struct SpriteDrawFastPath : Modify<SpriteDrawFastPath, CCSprite> {
    void draw() {
        if (g_flags.any) {
            if (g_flags.spriteFastPath) {
                // The node's own opacity, not getDisplayedOpacity(): a review of the shader
                // path pointed out that a program which reads only vColor.rgb, or an additive
                // blend on a non-premultiplied texture, can still paint with vertex alpha 0.
                // Zero local opacity is what has been measured in-game (1,830 skips per 10 s
                // with no visual complaint), so the wider case stays out.
                if (this->getOpacity() == 0
                    || this->getScaleX() == 0.f || this->getScaleY() == 0.f) {
                    ++g_hot.invisibleSprites;
                    return;
                }
                // Inherited opacity, guarded by the blend equation. A node under a faded-out
                // layer renders with displayed opacity 0, but that only guarantees zero
                // contribution when the blend multiplies the source by its alpha, which is
                // what GL_SRC_ALPHA as the source factor does (standard alpha blend and
                // additive blend both use it). Premultiplied blends use GL_ONE there, so a
                // program that ignores vertex alpha could still paint - those nodes are
                // drawn, and the skip stays provably correct.
                if (this->getDisplayedOpacity() == 0
                    && this->getBlendFunc().src == GL_SRC_ALPHA) {
                    ++g_hot.invisibleSprites;
                    return;
                }
                CCSize const size = this->getContentSize();
                if (size.width <= 0.f || size.height <= 0.f) {
                    ++g_hot.invisibleSprites;
                    return;
                }
            }
            if (g_flags.editor) {
                // The editor has its own policy. Object data, selection and serialization are
                // never involved: this only decides whether a sprite's draw is submitted.
                if (g_flags.editorCull) {
                    ++g_hot.editorSpritesExamined;
                    if (spriteOffScreen(this, g_flags.editorMargin)) {
                        ++g_hot.editorSpritesSkipped;
                        return;
                    }
                }
            }
            else if (g_flags.sceneCull) {
                ++g_hot.sceneExamined;
                if (spriteOffScreen(this, g_flags.sceneMargin)) {
                    ++g_hot.sceneSkipped;
                    return;
                }
            }
        }
        CCSprite::draw();
    }
};

// Telemetry for the path GD actually uses for level objects, plus the batch cull: the one
// skip in this file that can remove a whole level-object GL submission. Off by default
// (the `batch-culling` setting); when on, only a batch whose atlas buffer is in sync and
// whose entire vertex box lies outside the expanded view is skipped. Only draw() is
// skipped; visit(), updates, triggers and adding/removing objects are untouched.
struct BatchNodeTelemetry : Modify<BatchNodeTelemetry, CCSpriteBatchNode> {
    void draw() {
        if (g_flags.any) {
            ++g_hot.batchDraws;
            if (m_pobTextureAtlas) {
                unsigned int const quads = m_pobTextureAtlas->getTotalQuads();
                g_hot.batchQuads += quads;
                // Empty batches are common while effects expire. The original draw would bind
                // the program and texture and then submit zero quads; skipping that changes
                // no pixels and no state anyone reads afterwards (draw(), not visit()).
                if (quads == 0) {
                    ++g_hot.emptyBatches;
                    return;
                }
                // The level-object path. The cull fails open: a dirty atlas buffer, a
                // pending child sort, a bad vertex or an exhausted scan budget all draw.
                if (g_flags.batchCull && batchOffScreen(this, kBatchMargin)) {
                    ++g_hot.batchCulled;
                    return;
                }
            }
        }
        bool const trace = reborn::config().shaderTrace;
        if (trace) traceNoteBatchStart();
        CCSpriteBatchNode::draw();
        if (trace) traceNoteBatchEnd();
    }
};

struct ParticleDrawFastPath : Modify<ParticleDrawFastPath, CCParticleSystemQuad> {
    bool initWithTotalParticles(unsigned int total, bool flag) {
        std::uint64_t const earlier = createdEarlierThisFrame();
        ++reborn::counters().particleSystemsCreated;
        return CCParticleSystemQuad::initWithTotalParticles(limitedPool(total, earlier), flag);
    }

    void setTotalParticles(unsigned int total) {
        CCParticleSystemQuad::setTotalParticles(
            limitedPool(total, createdEarlierThisFrame())
        );
    }

    void draw() {
        if (g_flags.any) {
            // Same zero-pixel cases as the sprite fast-out: a system at zero opacity or zero
            // scale cannot paint anything, so its full quad upload and GL state setup is
            // wasted. Simulation still updates; only this draw submission is skipped. The
            // inherited-opacity case is covered for sprites, whose binding exposes
            // getDisplayedOpacity; CCNode's binding here does not.
            if (this->getOpacity() == 0 || this->getScaleX() == 0.f || this->getScaleY() == 0.f) {
                ++g_hot.particleInvisibleSkipped;
                return;
            }
            if (g_flags.particleIdle && m_uParticleCount == 0) {
                ++g_hot.particleIdleSkipped;
                return;
            }
            if (m_uParticleCount > 0 && g_flags.editor) {
                if (g_flags.editorCull) {
                    ++g_hot.editorParticlesExamined;
                    if (particleOffScreen(this, g_flags.editorMargin)) {
                        ++g_hot.editorParticlesSkipped;
                        return;
                    }
                }
            }
            else if (g_flags.particleCull && m_uParticleCount > 0) {
                ++g_hot.particleExamined;
                if (particleOffScreen(this, g_flags.particleMargin)) {
                    ++g_hot.particleOffscreenSkipped;
                    return;
                }
            }
        }
        CCParticleSystemQuad::draw();
    }
};

// Redundant-bind elimination for cocos2d's single GL entry point for programs. The check asks
// the driver what is bound on every call instead of trusting a cached assumption, so a bind
// that happens anywhere outside this hook is picked up by the very next use() - there is no
// cache to go stale. Skipping is only valid when the GL state already matches the program
// being used; the counter reports how often that holds. Windows-only for the same reason as
// low-latency: this is the one caller here that issues glGetIntegerv. Off by default.
struct ShaderStateCache : Modify<ShaderStateCache, CCGLProgram> {
    void use() {
#ifdef GEODE_IS_WINDOWS
        auto const& cfg = reborn::config();
        bool const trace = cfg.shaderTrace;
        bool const wantsSkip = cfg.enabled && cfg.shaderStateCache;
        if (trace || wantsSkip) {
            GLint bound = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
            bool const redundant = bound > 0 && static_cast<GLuint>(bound) == this->getProgram();
            if (trace) {
                traceProgramBind(this->getProgram(), redundant);
                traceNoteWindowBind(redundant);
            }
            if (wantsSkip && redundant) {
                ++g_hot.programBindsSkipped;
                return;
            }
        }
        CCGLProgram::use();
#else
        CCGLProgram::use();
#endif
    }
};

// Full suppression for off-screen particle systems: no simulation, no draws, zero work.
// Particle state freezes off screen; on re-entering the view the normal dt resumes, so there
// is no emission backlog burst and nothing is deleted mid-frame. Uses the same live-particle
// AABB test as the off-screen draw cull; Relative/Grouped only, Free systems simulate
// unchanged. Off by default.
struct ParticleSystemSuppress : Modify<ParticleSystemSuppress, CCParticleSystem> {
    void update(float dt) {
        auto const& cfg = reborn::config();
        if (cfg.enabled && cfg.particleSuppression) {
            tCCPositionType const type = this->getPositionType();
            if (type != kCCPositionTypeFree
                && particleOffScreen(this, cfg.particleCullMargin)) {
                ++g_hot.particleSimsSuppressed;
                return;
            }
        }
        CCParticleSystem::update(dt);
    }
};

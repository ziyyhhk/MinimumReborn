#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <gl/GL.h>
#endif
#include <Geode/cocos/effects/CCGrid.h>
#include <Geode/cocos/kazmath/include/kazmath/GL/matrix.h>
#include <Geode/cocos/shaders/CCGLProgram.h>
#include <Geode/cocos/shaders/CCShaderCache.h>
#include <Geode/cocos/shaders/ccGLStateCache.h>
#include <Geode/cocos/textures/CCTextureAtlas.h>
#include <Geode/modify/CCGLProgram.hpp>
#include <Geode/modify/CCParticleSystem.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/utils/cocos.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "config.hpp"
#include "gpu_cull.hpp"
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
// * Off-screen: a live particle system whose draws live in emitter-local space and whose live
//   particles lie fully outside the visible rect is not drawn. The AABB is measured from the
//   actual particle positions of this frame (plus each particle's own size), mapped to the
//   world through the emitter's own rotation, scale and origin, and tested against the
//   expanded viewport with the same world-space corner test the sprite fast-out uses. It
//   still updates, so emission timing, lifetimes and motion are unchanged. Covers the
//   Relative and Grouped position types; Free systems are always drawn: their particles live
//   in an absolute frame and no transform is safe to assume. If the emitter itself is on
//   screen the system is drawn without touching the particle data at all.
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
// 4. Batched level objects: per-quad filtering
// ---------------------------------------------------------------------------------------
// Level objects are submitted through CCSpriteBatchNode::draw, one GL draw per batch. A
// batch can hold thousands of quads. GD's visibility pass hides an object by collapsing its
// four vertices to (0,0,0), but the batch still uploads and shades every hidden quad.
//
// This path keeps the batch's single GL draw, its blend state and its atlas order, and
// rewrites only the index list so that quads which cannot produce a pixel are not submitted:
//
//   * zero-area quads (GD's hidden objects, collapsed sprites) are dropped;
//   * quads whose four projected corners lie fully outside the clip volume plus a small
//     margin are dropped. Every surviving quad keeps its original position in atlas order,
//     so overlapping objects blend in exactly the same sequence as before.
//
// Safety rules, all fail-open:
//   * only the stock PositionTextureColor program is filtered, or - on Windows - a custom
//     program whose vertex stage is verified byte-identical to the stock pass-through, the
//     shape of fragment-only effects. A spatial distortion that could move vertices always
//     draws untouched.
//   * no active grid; a grid redirects rendering and invalidates the screen-space test.
//   * the child updateTransform pass runs first - exactly where the original runs it - and
//     getTotalQuads() is read again afterwards because the pass can grow the atlas.
//   * each vertex is projected through P * MV. A non-finite value, w <= 0, an unusual
//     projection or more than 16,383 quads falls back to the original draw.
//   * the filtered index list lives in the mod's own buffer. The atlas's VBO and index
//     buffer are never modified, except for the normal dirty upload, which uploads the same
//     full [0,total) range the original draw would.
//   * a per-frame scan budget of 65,536 quads bounds the CPU cost. A batch that does not fit
//     in the remaining budget is drawn untested.
//   * if no quad survives, the GL submission is skipped entirely. The atlas dirty flag is
//     left set, so the next draw that needs the buffer uploads it once, normally.
//
// Only draw() is affected; visit(), updates, triggers, physics and object management are
// untouched.
//
// ---------------------------------------------------------------------------------------
// 5. What "shader culling" can honestly mean here
// ---------------------------------------------------------------------------------------
// cocos2d-x sets up the shader per node inside draw(): CCGLProgram activation, the vertex
// attribute pointers, the texture bind and only then the draw call. Every skip in this file
// returns *before* that sequence, so the work that is actually avoided per skipped node is:
//   * the program activation and uniform setup (texture, GL state),
//   * the vertex attribute pointers,
//   * the draw call itself (and for a particle system, the per-frame quad buffer rebuild),
//   * the fragment work those triangles would have produced.
// That is the defensible version of "avoiding shader setup for objects that will not be
// rendered", and it is measured: the counters on the Detailed overlay only move when a draw()
// call is really not submitted.
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
//   * no geometry is rewritten in the atlas and no batch is split, merged or reordered. The
//     filtered path only selects a subset of the atlas's existing quads through its own
//     index list, in atlas order; every kept quad is byte-identical to the original draw.
//
// What this is NOT: a replacement renderer. Draw order, blending, glow, clipping, shaders and
// texture state are untouched because every kept draw still goes through the original
// cocos2d function in the original order.

namespace reborn {

    // Uploads the atlas dirty range - byte-identical to what the original draw uploads -
    // and clears the dirty flag. Shared by the CPU and GPU filter paths.
    void uploadAtlasVerticesIfDirty(CCTextureAtlas* atlas, unsigned int total) {
        glBindBuffer(GL_ARRAY_BUFFER, atlas->m_pBuffersVBO[0]);
        if (atlas->m_bDirty) {
            glBufferSubData(
                GL_ARRAY_BUFFER, 0,
                static_cast<GLsizeiptr>(sizeof(ccV3F_C4B_T2F_Quad) * total),
                atlas->getQuads()
            );
            atlas->m_bDirty = false;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Enables PosColorTex attributes and binds the interleaved pointers against the atlas
    // vertex VBO. The buffer binding captured at glVertexAttribPointer time persists.
    void setupAtlasVertexAttribs(CCTextureAtlas* atlas) {
        glBindBuffer(GL_ARRAY_BUFFER, atlas->m_pBuffersVBO[0]);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
        GLsizei const stride = static_cast<GLsizei>(sizeof(ccV3F_C4B_T2F));
        glVertexAttribPointer(
            kCCVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<GLvoid const*>(offsetof(ccV3F_C4B_T2F, vertices))
        );
        glVertexAttribPointer(
            kCCVertexAttrib_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            reinterpret_cast<GLvoid const*>(offsetof(ccV3F_C4B_T2F, colors))
        );
        glVertexAttribPointer(
            kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<GLvoid const*>(offsetof(ccV3F_C4B_T2F, texCoords))
        );
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

}

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
    // Bounds the CPU cost; a dense shader level can carry ~50k quads per frame, so the cap
    // is the whole GD object limit rather than a guess at typical density.
    constexpr std::uint64_t kBatchScanBudget = 65536;
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

    // True when a particle system whose draws live in emitter-local space cannot paint
    // anything inside the expanded view. Covers Relative and Grouped; Free systems are left
    // alone (their particles live in an absolute frame and no transform is safe to assume).
    //
    // The bound is measured from the same particle data this frame draws - including the same
    // per-particle translation correction cocos2d applies for Relative emitters, and the emitter's
    // own world basis for position, rotation and scale. The local AABB (expanded by each
    // particle's own size) is mapped through the emitter's frame; the containing
    // world-space rectangle therefore holds every quad this frame will submit. Any doubt
    // answers "draw".
    bool particleOffScreen(CCParticleSystem* system, double margin) {
        tCCPositionType const type = system->getPositionType();
        if (type != kCCPositionTypeRelative && type != kCCPositionTypeGrouped) return false;

        // The emitter's own world basis, including its rotation and scale - not just the
        // parent's, which would under-bound a rotated or scaled emitter.
        Basis basis;
        if (!basisOf(system, basis)) return false;

        CCPoint const world = basis.origin;
        if (!isFinite(world.x) || !isFinite(world.y)) return false;

        // Common case: the emitter is on screen, so a particle is guaranteed to be visible.
        // This keeps the per-frame cost of visible systems to one point test.
        if (circleTouches(world.x, world.y, 0.0, margin)) return false;

        unsigned int const count = system->m_uParticleCount;
        if (count == 0) return false;  // idle systems go through the idle skip, never through a cull
        tCCParticle const* particles = system->m_pParticles;
        if (!particles) return false;

        // Relative draws live in the emitter's frame after the engine has compensated for the
        // emitter's translation, exactly as cocos2d's update() computes it. Grouped particles
        // are already emitter-local.
        CCPoint const current(system->getPositionX(), system->getPositionY());

        double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
        for (unsigned int i = 0; i < count; ++i) {
            CCPoint const rel(particles[i].pos);
            float const size = particles[i].size;
            if (!isFinite(rel.x) || !isFinite(rel.y) || !isFinite(size)) return false;

            CCPoint const draw = (type == kCCPositionTypeRelative)
                ? ccp(rel.x - (current.x - particles[i].startPos.x),
                      rel.y - (current.y - particles[i].startPos.y))
                : rel;

            float const pad = size > 0.f ? size * 0.5f : 0.f;
            double const x0 = static_cast<double>(draw.x) - pad;
            double const x1 = static_cast<double>(draw.x) + pad;
            double const y0 = static_cast<double>(draw.y) - pad;
            double const y1 = static_cast<double>(draw.y) + pad;
            if (i == 0) {
                minX = x0; maxX = x1; minY = y0; maxY = y1;
            } else {
                minX = std::min(minX, x0); maxX = std::max(maxX, x1);
                minY = std::min(minY, y0); maxY = std::max(maxY, y1);
            }
        }

        // The particle AABB is a rotated rectangle in world space; its exact containing
        // rectangle is a tighter bound than the old circle test, so more off-screen systems can
        // be skipped without changing any pixel. Any doubt answers "draw".
        CCPoint const p00 = toWorld(basis, ccp(static_cast<float>(minX), static_cast<float>(minY)));
        CCPoint const p10 = toWorld(basis, ccp(static_cast<float>(maxX), static_cast<float>(minY)));
        CCPoint const p01 = toWorld(basis, ccp(static_cast<float>(minX), static_cast<float>(maxY)));
        CCPoint const p11 = toWorld(basis, ccp(static_cast<float>(maxX), static_cast<float>(maxY)));
        if (!g_view.valid) return false;

        double const left = std::min(std::min(p00.x, p10.x), std::min(p01.x, p11.x));
        double const right = std::max(std::max(p00.x, p10.x), std::max(p01.x, p11.x));
        double const bottom = std::min(std::min(p00.y, p10.y), std::min(p01.y, p11.y));
        double const top = std::max(std::max(p00.y, p10.y), std::max(p01.y, p11.y));

        return right < g_view.left - margin || left > g_view.right + margin
            || top < g_view.bottom - margin || bottom > g_view.top + margin;
    }

    // ---------------------------------------------------------------------------------------
    // Batched level objects: per-quad filter
    // ---------------------------------------------------------------------------------------
    // The draw call only sees an index list. Rebuilding that list lets one batch keep its
    // single GL submission while dropping quads that cannot paint. The scan runs on the
    // vertex data the original draw would submit, so there is no separate visibility model
    // to disagree with the engine.
    constexpr unsigned int kMaxFilterQuads = 16383;  // 4 * 16383 + 3 = 65535
    constexpr double kFilterNdcMargin = 0.005;
    // Batches larger than this are scanned by the GL 4.3 compute path when it is enabled;
    // smaller ones stay on the CPU filter, where a dispatch plus its barrier would cost
    // more than the scan. The threshold is a static tuning constant, not a setting.
    constexpr unsigned int kGpuBatchCullMinQuads = 1024;

    // Static storage: one consumer on the main thread and a fixed upper bound, so the draw
    // path never allocates.
    GLushort g_filterIndices[kMaxFilterQuads * 6];

    bool projectionIsScreenSpace(kmMat4 const& projection) {
        for (float const value : projection.mat) {
            if (!std::isfinite(value)) return false;
        }
        // Standard cocos2d 2D projection: w row = (0,0,0,1). A 3D camera, a grid or a custom
        // projection fails this test and gets the original draw.
        return std::fabs(projection.mat[3]) < 1e-6f
            && std::fabs(projection.mat[7]) < 1e-6f
            && std::fabs(projection.mat[11]) < 1e-6f
            && std::fabs(projection.mat[15] - 1.f) < 1e-6f;
    }

    // Custom shader programs are verified at draw time, with no cross-frame cache: cocos
    // programs can keep the same handle while their source changes only outside the frame, so
    // a verdict held across frames could mis-judge a reused handle. The verification itself is
    // a handful of GL calls per batch, bounded by the number of batches per frame.
    constexpr GLint kMaxShaderSourceBytes = 8192;

    bool readVertexSource(GLuint program, char* out, GLsizei maxBytes, GLsizei* outLength) {
#ifdef GEODE_IS_WINDOWS
        GLsizei count = 0;
        GLuint shaders[4] = {};
        glGetAttachedShaders(program, 4, &count, shaders);
        if (count > 0) {
            for (GLsizei i = 0; i < count; ++i) {
                GLint type = 0;
                glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
                if (type != GL_VERTEX_SHADER) continue;
                GLint length = 0;
                glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
                if (length <= 0 || length > static_cast<GLint>(maxBytes)) {
                    return false;
                }
                glGetShaderSource(shaders[i], maxBytes, nullptr, out);
                *outLength = length;
                return true;
            }
        }
        return false;
#else
        (void)program; (void)out; (void)maxBytes; (void)outLength;
        return false;
#endif
    }

    // True when the batch's shader program cannot alter vertex positions: it is the stock
    // PositionTextureColor program, or - on Windows - its vertex stage is byte-identical to the
    // stock program's vertex stage. That accepts fragment-only effects (the shape of GD's
    // shader library) but never a shader that could displace geometry, and any doubt fails
    // open to the original draw.
    bool verifiablePassthroughProgram(CCSpriteBatchNode* batch) {
        CCGLProgram* program = batch->getShaderProgram();
        if (!program) return false;
        CCShaderCache* cache = CCShaderCache::sharedShaderCache();
        CCGLProgram* stock = cache ? cache->programForKey(kCCShader_PositionTextureColor)
                                   : nullptr;
        if (!stock) return false;
        if (program == stock) return true;
        if (!reborn::config().customShaderFilter) return false;

#ifdef GEODE_IS_WINDOWS
        bool ok = false;
        char custom[kMaxShaderSourceBytes] = {};
        char stockSource[kMaxShaderSourceBytes] = {};
        GLsizei lenCustom = -1, lenStock = -1;
        if (readVertexSource(program->getProgram(), custom,
                             kMaxShaderSourceBytes, &lenCustom)
            && readVertexSource(stock->getProgram(), stockSource,
                                kMaxShaderSourceBytes, &lenStock)
            && lenCustom == lenStock && lenCustom > 0
            && std::memcmp(custom, stockSource, static_cast<size_t>(lenCustom)) == 0) {
            ok = true;
        }
        return ok;
#else
        return false;
#endif
    }

    // Checks that can be made before the child transform pass. The caller re-checks the quad
    // count afterwards because updateTransform can grow the atlas.
    bool batchFilterEligible(CCSpriteBatchNode* batch, CCTextureAtlas* atlas, unsigned int total) {
        if (!atlas || total == 0 || total > kMaxFilterQuads) return false;
        if (total > atlas->getCapacity()) return false;
        if (!atlas->getQuads() || !atlas->getTexture()) return false;
        if (atlas->m_pBuffersVBO[0] == 0 || atlas->m_pBuffersVBO[1] == 0) return false;
        if (batch->m_bManualSortAllChildrenDirty) return false;
        CCGridBase* grid = batch->getGrid();
        if (grid && grid->isActive()) return false;
        if (!verifiablePassthroughProgram(batch)) return false;
        return g_batchScanBudget >= total;
    }

    // Returns the number of quads written to `indices`, or `total` when the projection is not
    // screen-space (fail open). The caller has already checked the budget and the quad count.
    unsigned int filterBatchQuads(CCTextureAtlas* atlas, unsigned int total, GLushort* indices) {
        kmMat4 projection{};
        kmMat4 modelview{};
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelview);
        if (!projectionIsScreenSpace(projection)) return total;

        kmMat4 mvp{};
        kmMat4Multiply(&mvp, &projection, &modelview);

        ccV3F_C4B_T2F_Quad const* quads = atlas->getQuads();
        if (!quads) return total;

        unsigned int kept = 0;
        for (unsigned int i = 0; i < total; ++i) {
            ccV3F_C4B_T2F_Quad const& quad = quads[i];
            float const xs[4] = {
                quad.tl.vertices.x, quad.bl.vertices.x, quad.tr.vertices.x, quad.br.vertices.x
            };
            float const ys[4] = {
                quad.tl.vertices.y, quad.bl.vertices.y, quad.tr.vertices.y, quad.br.vertices.y
            };
            float const zs[4] = {
                quad.tl.vertices.z, quad.bl.vertices.z, quad.tr.vertices.z, quad.br.vertices.z
            };

            double ndcX[4] = {};
            double ndcY[4] = {};
            double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
            bool usable = true;

            for (int v = 0; v < 4; ++v) {
                double const x = xs[v];
                double const y = ys[v];
                double const z = zs[v];
                double const cx = mvp.mat[0] * x + mvp.mat[4] * y + mvp.mat[8] * z + mvp.mat[12];
                double const cy = mvp.mat[1] * x + mvp.mat[5] * y + mvp.mat[9] * z + mvp.mat[13];
                double const cw = mvp.mat[3] * x + mvp.mat[7] * y + mvp.mat[11] * z + mvp.mat[15];
                if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw) || cw <= 1e-6) {
                    usable = false;
                    break;
                }
                ndcX[v] = cx / cw;
                ndcY[v] = cy / cw;
                if (!std::isfinite(ndcX[v]) || !std::isfinite(ndcY[v])) {
                    usable = false;
                    break;
                }
                if (v == 0) {
                    minX = maxX = ndcX[v];
                    minY = maxY = ndcY[v];
                } else {
                    minX = std::min(minX, ndcX[v]);
                    maxX = std::max(maxX, ndcX[v]);
                    minY = std::min(minY, ndcY[v]);
                    maxY = std::max(maxY, ndcY[v]);
                }
            }

            if (usable) {
                bool const outside =
                    maxX < -1.0 - kFilterNdcMargin || minX > 1.0 + kFilterNdcMargin ||
                    maxY < -1.0 - kFilterNdcMargin || minY > 1.0 + kFilterNdcMargin;
                // The quad is a parallelogram (tl, bl, tr, br). Collinear edges mean no
                // pixels; GD's hidden objects are four identical corners.
                double const ax = ndcX[1] - ndcX[0];
                double const ay = ndcY[1] - ndcY[0];
                double const bx = ndcX[2] - ndcX[0];
                double const by = ndcY[2] - ndcY[0];
                double const area = std::fabs(ax * by - ay * bx);
                if (outside || area < 1e-12) continue;
            }

            GLushort const base = static_cast<GLushort>(i * 4);
            GLushort* out = indices + kept * 6;
            out[0] = base;
            out[1] = static_cast<GLushort>(base + 1);
            out[2] = static_cast<GLushort>(base + 2);
            out[3] = static_cast<GLushort>(base + 3);
            out[4] = static_cast<GLushort>(base + 2);
            out[5] = static_cast<GLushort>(base + 1);
            ++kept;
        }
        return kept;
    }

    // Same tail as CCTextureAtlas::drawNumberOfQuads for the non-VAO Windows path, except the
    // element buffer is unbound and the filtered list is supplied from client memory. The
    // vertex upload is byte-for-byte the range the original would upload, so a later,
    // unfiltered draw of this atlas sees identical VBO contents.
    void drawFilteredAtlas(
        CCTextureAtlas* atlas, unsigned int total, unsigned int kept, GLushort const* indices
    ) {
        if (!atlas || kept == 0) return;
        CCTexture2D* texture = atlas->getTexture();
        if (!texture) return;

        ccGLBindTexture2D(texture->getName());

        reborn::uploadAtlasVerticesIfDirty(atlas, total);
        reborn::setupAtlasVertexAttribs(atlas);

        // With no element buffer bound, the pointer addresses client memory. That is legal in
        // the compatibility context cocos2d 2.2 creates on Windows.
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(kept * 6), GL_UNSIGNED_SHORT, indices);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        CC_INCREMENT_GL_DRAWS(1);
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

    // CPU timings for the GD-side paths that a 2.2 shader level exercises. Wall-clock
    // steady_clock around each call; flushed with the same 5 s window as the bind counts.
    struct TraceTiming {
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
        std::uint64_t calls = 0;
    };

    TraceTiming g_traceVisibility;
    TraceTiming g_traceEditorVisibility;
    TraceTiming g_traceShaderLayer;
    TraceTiming g_traceShaderVisit;
    std::uint64_t g_traceBatchScanNs = 0;
    std::uint64_t g_traceBatchScanQuads = 0;
    std::uint64_t g_traceBatchScans = 0;

    void traceNoteTime(TraceTiming& timing, std::chrono::steady_clock::duration elapsed) {
        auto const ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
        );
        timing.totalNs += ns;
        if (ns > timing.maxNs) timing.maxNs = ns;
        ++timing.calls;
    }

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

    struct TraceFlush {
        std::uint64_t binds = 0;
        std::uint64_t redundant = 0;
    };

    // Called from renderFrameEnd(); cheap when disabled (one bool + one clock read per frame).
    // Returns the window's deltas so the caller can publish them once the flush ran.
    TraceFlush traceMaybeFlush(std::chrono::steady_clock::time_point now) {
        TraceFlush out;
        if (!reborn::config().shaderTrace) return out;
        if (now - g_traceLastFlush < kTraceWindow) return out;
        g_traceLastFlush = now;

        out.binds = g_traceBinds;
        out.redundant = g_traceRedundant;

        auto const avgMs = [](TraceTiming const& timing) {
            return timing.calls > 0
                ? static_cast<double>(timing.totalNs) / static_cast<double>(timing.calls) / 1e6
                : 0.0;
        };
        auto const maxMs = [](TraceTiming const& timing) {
            return static_cast<double>(timing.maxNs) / 1e6;
        };

        log::info(
            "shader-trace: binds {} (redundant {}), batches {}, binds/batch avg {:.1f} max {}, "
            "untracked {}, top programs [{}]",
            g_traceBinds, g_traceRedundant, g_traceBatches,
            g_traceBatches > 0 ? static_cast<double>(g_traceBatchBinds) / g_traceBatches : 0.0,
            g_traceMaxBatchBinds, g_traceUntracked, traceTopPrograms()
        );
        log::info(
            "gd-trace: visibility {:.3f}/{:.3f} ms avg/max ({}), editor {:.3f}/{:.3f} ({}), "
            "shader-layer {:.3f}/{:.3f} ({}), shader-visit {:.3f}/{:.3f} ({}), "
            "batch scan {:.1f} us avg, {} quads in {} scans",
            avgMs(g_traceVisibility), maxMs(g_traceVisibility), g_traceVisibility.calls,
            avgMs(g_traceEditorVisibility), maxMs(g_traceEditorVisibility), g_traceEditorVisibility.calls,
            avgMs(g_traceShaderLayer), maxMs(g_traceShaderLayer), g_traceShaderLayer.calls,
            avgMs(g_traceShaderVisit), maxMs(g_traceShaderVisit), g_traceShaderVisit.calls,
            g_traceBatchScans > 0
                ? static_cast<double>(g_traceBatchScanNs) / static_cast<double>(g_traceBatchScans) / 1e3
                : 0.0,
            g_traceBatchScanQuads, g_traceBatchScans
        );

        for (auto& entry : g_trace) entry = TraceEntry{};
        g_traceBinds = 0;
        g_traceRedundant = 0;
        g_traceBatches = 0;
        g_traceBatchBinds = 0;
        g_traceMaxBatchBinds = 0;
        g_traceUntracked = 0;
        g_traceVisibility = TraceTiming{};
        g_traceEditorVisibility = TraceTiming{};
        g_traceShaderLayer = TraceTiming{};
        g_traceShaderVisit = TraceTiming{};
        g_traceBatchScanNs = 0;
        g_traceBatchScanQuads = 0;
        g_traceBatchScans = 0;
        return out;
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
        std::uint64_t batchCulled = 0;       // batches with at least one quad removed
        std::uint64_t batchFullyCulled = 0;  // batches with no visible quads
        std::uint64_t batchScans = 0;        // batches scanned
        std::uint64_t batchQuadsCulled = 0;  // individual quads removed
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
        add(c.batchFullyCulled, g_hot.batchFullyCulled);
        add(c.batchScans, g_hot.batchScans);
        add(c.batchQuadsCulled, g_hot.batchQuadsCulled);
        auto const flush = traceMaybeFlush(std::chrono::steady_clock::now());
        g_hot.shaderTraceBinds += flush.binds;
        g_hot.shaderTraceRedundant += flush.redundant;

        add(c.particleSimsSuppressed, g_hot.particleSimsSuppressed);
        add(c.shaderTraceBinds, g_hot.shaderTraceBinds);
        add(c.shaderTraceRedundant, g_hot.shaderTraceRedundant);
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

// Per-quad filtering inside CCSpriteBatchNode::draw, the path GD uses for level objects.
// The batch keeps its single GL submission, its blend state and its atlas order; only quads
// that cannot produce a pixel are left out of the index list. The child updateTransform pass
// and the atlas dirty upload still run exactly where they did. Any unexpected state falls
// back to the original draw. See section 4 at the top of this file.
struct BatchNodeTelemetry : Modify<BatchNodeTelemetry, CCSpriteBatchNode> {
    void draw() {
        if (g_flags.any) {
            ++g_hot.batchDraws;
            CCTextureAtlas* atlas = m_pobTextureAtlas;
            if (!atlas) {
                CCSpriteBatchNode::draw();
                return;
            }

            unsigned int total = atlas->getTotalQuads();
            g_hot.batchQuads += total;
            // Empty batches are common while effects expire. The original draw would bind
            // the program and texture and then submit zero quads; skipping that changes
            // no pixels and no state anyone reads afterwards (draw(), not visit()).
            if (total == 0) {
                ++g_hot.emptyBatches;
                return;
            }

            if (g_flags.batchCull && batchFilterEligible(this, atlas, total)) {
                // The original draw runs this pass immediately before it submits. It has to
                // run before the scan so the vertex data describes this frame; the pass is
                // dirty-flag guarded, so the original would not repeat real work.
                CCArray* children = getChildren();
                if (children && children->count() > 0) {
                    for (CCSprite* child : getChildrenExt<CCSprite>()) {
                        if (child) child->updateTransform();
                    }
                }

                total = atlas->getTotalQuads();
                if (total == 0) {
                    ++g_hot.emptyBatches;
                    return;
                }
                // The pass can grow the atlas. Re-check every bound before indexing, and
                // draw whole if the grown batch does not fit the remaining scan budget.
                if (total > kMaxFilterQuads || total > atlas->getCapacity()
                    || total > g_batchScanBudget) {
                    drawWholeBatch();
                    return;
                }

                g_batchScanBudget -= total;

                // GPU compute path for large batches (opt-in). One dispatch, no readback,
                // indices built on the GPU, submitted with glDrawElementsIndirect. Any
                // failure falls back to the CPU filter below with nothing drawn and no
                // GL state left bound. The CPU path re-uploads nothing (the dirty flag was
                // cleared above).
                if (reborn::config().gpuBatchCull && total > kGpuBatchCullMinQuads) {
                    CC_NODE_DRAW_SETUP();
                    ccGLBlendFunc(m_blendFunc.src, m_blendFunc.dst);
                    reborn::uploadAtlasVerticesIfDirty(atlas, total);
                    if (reborn::drawBatchGpuCull(
                            atlas, total, this->getShaderProgram()->getProgram())
                        == reborn::GpuCullResult::Submitted) {
                        return;
                    }
                    // Submitted nothing; the CPU filter runs below instead.
                }
                ++g_hot.batchScans;
                unsigned int kept = 0;
                if (reborn::config().shaderTrace) {
                    auto const scanStart = std::chrono::steady_clock::now();
                    kept = filterBatchQuads(atlas, total, g_filterIndices);
                    auto const scanTime = std::chrono::steady_clock::now() - scanStart;
                    g_traceBatchScanNs += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(scanTime).count()
                    );
                    g_traceBatchScanQuads += total;
                    ++g_traceBatchScans;
                }
                else {
                    kept = filterBatchQuads(atlas, total, g_filterIndices);
                }

                if (kept >= total) {
                    drawWholeBatch();
                    return;
                }

                ++g_hot.batchCulled;
                g_hot.batchQuadsCulled += total - kept;
                if (kept == 0) {
                    ++g_hot.batchFullyCulled;
                    // No GL work at all. The atlas dirty flag stays set, so the next draw
                    // that needs the buffer uploads the full vertex range normally.
                    return;
                }

                bool const traceDraw = reborn::config().shaderTrace;
                if (traceDraw) traceNoteBatchStart();
                CC_NODE_DRAW_SETUP();
                ccGLBlendFunc(m_blendFunc.src, m_blendFunc.dst);
                drawFilteredAtlas(atlas, total, kept, g_filterIndices);
                if (traceDraw) traceNoteBatchEnd();
                return;
            }
        }

        bool const trace = reborn::config().shaderTrace;
        if (trace) traceNoteBatchStart();
        CCSpriteBatchNode::draw();
        if (trace) traceNoteBatchEnd();
    }

private:
    void drawWholeBatch() {
        bool const trace = reborn::config().shaderTrace;
        if (trace) traceNoteBatchStart();
        CC_NODE_DRAW_SETUP();
        ccGLBlendFunc(m_blendFunc.src, m_blendFunc.dst);
        m_pobTextureAtlas->drawQuads();
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

// Diagnostic-only program-bind accounting. When shader-trace is off this is a pass-through.
// The old shader-state-cache skip is gone: cocos2d already routes binds through
// ccGLUseProgram's own state cache (CC_ENABLE_GL_STATE_CACHE), so a second cache here was
// redundant, and its per-bind glGetIntegerv was itself a driver call.
struct ShaderBindTrace : Modify<ShaderBindTrace, CCGLProgram> {
    void use() {
#ifdef GEODE_IS_WINDOWS
        if (reborn::config().shaderTrace) {
            GLint bound = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
            bool const redundant = bound > 0 && static_cast<GLuint>(bound) == this->getProgram();
            traceProgramBind(this->getProgram(), redundant);
            traceNoteWindowBind(redundant);
        }
#endif
        CCGLProgram::use();
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

// shader-trace CPU timings for the GD-side 2.2 paths. Each hook is one branch and a direct
// pass-through when the setting is off. When on, it records the wall time of the named
// function. These are CPU times, not GPU times, and no behavior depends on them.
struct TracePlayLayerVisibility : Modify<TracePlayLayerVisibility, PlayLayer> {
    void updateVisibility(float dt) {
        if (!reborn::config().shaderTrace) {
            PlayLayer::updateVisibility(dt);
            return;
        }
        auto const start = std::chrono::steady_clock::now();
        PlayLayer::updateVisibility(dt);
        traceNoteTime(g_traceVisibility, std::chrono::steady_clock::now() - start);
    }
};

struct TraceEditorVisibility : Modify<TraceEditorVisibility, LevelEditorLayer> {
    void updateVisibility(float dt) {
        if (!reborn::config().shaderTrace) {
            LevelEditorLayer::updateVisibility(dt);
            return;
        }
        auto const start = std::chrono::steady_clock::now();
        LevelEditorLayer::updateVisibility(dt);
        traceNoteTime(g_traceEditorVisibility, std::chrono::steady_clock::now() - start);
    }
};

struct TraceShaderLayerUpdate : Modify<TraceShaderLayerUpdate, GJBaseGameLayer> {
    void updateShaderLayer(float dt) {
        if (!reborn::config().shaderTrace) {
            GJBaseGameLayer::updateShaderLayer(dt);
            return;
        }
        auto const start = std::chrono::steady_clock::now();
        GJBaseGameLayer::updateShaderLayer(dt);
        traceNoteTime(g_traceShaderLayer, std::chrono::steady_clock::now() - start);
    }
};

struct TraceShaderLayerVisit : Modify<TraceShaderLayerVisit, ShaderLayer> {
    void visit() {
        if (!reborn::config().shaderTrace) {
            ShaderLayer::visit();
            return;
        }
        auto const start = std::chrono::steady_clock::now();
        ShaderLayer::visit();
        traceNoteTime(g_traceShaderVisit, std::chrono::steady_clock::now() - start);
    }
};

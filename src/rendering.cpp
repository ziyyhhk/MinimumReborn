#include <Geode/Geode.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>
#include <algorithm>
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
// * Off-screen: a relative-position system whose worst-case reach (speed * life plus
//   gravity/radial/tangential acceleration, start/end radius, position variance and particle
//   size, converted to world units) does not touch the expanded viewport is not drawn. It
//   still updates, so emission timing, lifetimes and motion are unchanged and it looks the
//   same when it scrolls back. Free and grouped systems are always drawn: their particles are
//   not placed relative to the emitter and no safe bound exists.
// * Center-inside shortcut: if the emitter position itself is inside the view the system is
//   drawn without computing the reach bound, which keeps the per-frame cost of the common
//   case to one circle test.
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

    // Local -> world mapping of a node: world = origin + x * ux + y * uy.
    struct Basis {
        CCPoint origin;
        CCPoint ux;
        CCPoint uy;
    };

    struct Slot {
        CCNode const* node = nullptr;
        unsigned frame = 0;
        Basis basis;
    };

    // Direct-mapped, rebuilt every frame because transforms change with the camera. A miss
    // overwrites the slot; nothing grows and nothing is kept across frames.
    constexpr unsigned kTransformSlots = 256;
    Slot g_slots[kTransformSlots];

    bool isFinite(float value) {
        return std::isfinite(value);
    }

    bool basisOf(CCNode* node, Basis& out) {
        if (!node) return false;

        auto const key = reinterpret_cast<std::uintptr_t>(node);
        Slot& slot = g_slots[(key >> 4) % kTransformSlots];
        if (slot.frame == g_frameId && slot.node == node) {
            out = slot.basis;
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
        slot.frame = g_frameId;
        slot.basis = b;
        out = b;
        return true;
    }

    double basisScale(Basis const& b) {
        return std::max(std::hypot(b.ux.x, b.ux.y), std::hypot(b.uy.x, b.uy.y));
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
        double const radius = std::hypot(size.width, size.height) * scale;
        if (!std::isfinite(radius)) return false;

        Basis basis;
        if (!basisOf(parent, basis)) return false;

        CCPoint const center = toWorld(basis, sprite->getPosition());
        if (!isFinite(center.x) || !isFinite(center.y)) return false;

        return !circleTouches(center.x, center.y, radius * basisScale(basis), margin);
    }

    struct Bounds {
        double speed = 0.0;
        double life = 0.0;
        double accel = 0.0;
        double radius = 0.0;
        double posVar = 0.0;
        double source = 0.0;
        double size = 0.0;
    };

    // Worst-case distance a particle can travel from the emitter, in local units.
    double particleReach(Bounds const& b) {
        double const life = std::max(0.0, b.life);
        double const flight = b.speed * life + 0.5 * b.accel * life * life + b.radius;
        double const spread = b.posVar + b.source + b.size * 0.5;
        return flight + spread;
    }

    // True when a relative-position system cannot paint anything inside the expanded view.
    bool particleOffScreen(CCParticleSystemQuad* system, double margin) {
        if (system->getPositionType() != kCCPositionTypeRelative) return false;

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

        CCPoint const& gravity = system->getGravity();
        CCPoint const& source = system->getSourcePosition();
        CCPoint const& posVar = system->getPosVar();

        Bounds bounds;
        bounds.speed = std::fabs(system->getSpeed()) + std::fabs(system->getSpeedVar());
        bounds.life = std::fabs(system->getLife()) + std::fabs(system->getLifeVar());
        bounds.accel = std::hypot(gravity.x, gravity.y)
                     + std::fabs(system->getRadialAccel()) + std::fabs(system->getRadialAccelVar())
                     + std::fabs(system->getTangentialAccel()) + std::fabs(system->getTangentialAccelVar());
        bounds.radius = std::max(
            std::fabs(system->getStartRadius()) + std::fabs(system->getStartRadiusVar()),
            std::fabs(system->getEndRadius()) + std::fabs(system->getEndRadiusVar())
        );
        bounds.posVar = std::hypot(posVar.x, posVar.y);
        bounds.source = std::hypot(source.x, source.y);
        bounds.size = std::max(
            std::fabs(system->getStartSize()) + std::fabs(system->getStartSizeVar()),
            std::fabs(system->getEndSize()) + std::fabs(system->getEndSizeVar())
        );

        double const reach = particleReach(bounds) * scale;
        if (!std::isfinite(reach)) return false;

        return !circleTouches(world.x, world.y, reach, margin);
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
        g_frameCreatedBase = counters().particleSystemsCreated.load(std::memory_order_relaxed);
        // One cheap scene check per frame, shared by every draw hook. The editor is a
        // separate scene from PlayLayer; both use the same draw hooks, so the culling
        // policy is selected here rather than by installing editor-only hooks.
        g_editorFrame = LevelEditorLayer::get() != nullptr;

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
            || (cfg.cullOffscreenParticles && cfg.particleCull != reborn::CullMode::Off);
    }
}

// The sprite hook. This is a real branch in the draw path of every sprite, so it stays small:
// the fast-out is three field reads; the visibility test is one cached basis lookup, one
// circle test and - only on a miss - three convertToWorldSpace calls for the parent.
struct SpriteDrawFastPath : Modify<SpriteDrawFastPath, CCSprite> {
    void draw() {
        auto const& cfg = reborn::config();
        if (cfg.enabled && cfg.rendererOptimized) {
            if (cfg.spriteFastPath) {
                if (this->getOpacity() == 0 || this->getScaleX() == 0.f || this->getScaleY() == 0.f) {
                    ++reborn::counters().invisibleSpriteDrawsSkipped;
                    return;
                }
                CCSize const size = this->getContentSize();
                if (size.width <= 0.f || size.height <= 0.f) {
                    ++reborn::counters().invisibleSpriteDrawsSkipped;
                    return;
                }
            }
            if (reborn::inEditor()) {
                // The editor has its own policy. Object data, selection and serialization are
                // never involved: this only decides whether a sprite's draw is submitted.
                if (cfg.editorCulling && cfg.editorCull != reborn::CullMode::Off) {
                    ++reborn::counters().editorSpritesExamined;
                    if (spriteOffScreen(this, cfg.editorCullMargin)) {
                        ++reborn::counters().editorSpriteDrawsSkipped;
                        return;
                    }
                }
            }
            else if (cfg.sceneCull != reborn::CullMode::Off) {
                ++reborn::counters().sceneSpritesExamined;
                if (spriteOffScreen(this, cfg.sceneCullMargin)) {
                    ++reborn::counters().sceneSpriteDrawsSkipped;
                    return;
                }
            }
        }
        CCSprite::draw();
    }
};

// Telemetry for the path GD actually uses for level objects. CCSpriteBatchNode draws all of
// its children in one submission and never calls CCSprite::draw for them, so the per-sprite
// counters above describe unbatched sprites only. Counting batches and their quads here shows
// how much of the scene is batched (and therefore already outside the per-sprite hook).
struct BatchNodeTelemetry : Modify<BatchNodeTelemetry, CCSpriteBatchNode> {
    void draw() {
        auto const& cfg = reborn::config();
        if (cfg.enabled && cfg.rendererOptimized) {
            ++reborn::counters().batchDraws;
            if (m_pobTextureAtlas) {
                unsigned int const quads = m_pobTextureAtlas->getTotalQuads();
                reborn::counters().batchQuads.fetch_add(quads, std::memory_order_relaxed);
                // Empty batches are common while effects expire. The original draw would bind
                // the program and texture and then submit zero quads; skipping that changes
                // no pixels and no state anyone reads afterwards (draw(), not visit()).
                if (quads == 0) {
                    ++reborn::counters().emptyBatchDrawsSkipped;
                    return;
                }
            }
        }
        CCSpriteBatchNode::draw();
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
        auto const& cfg = reborn::config();
        if (cfg.enabled && cfg.rendererOptimized) {
            if (cfg.skipIdleParticles && m_uParticleCount == 0) {
                ++reborn::counters().idleParticleDrawsSkipped;
                return;
            }
            if (m_uParticleCount > 0 && reborn::inEditor()) {
                if (cfg.editorCulling && cfg.editorCull != reborn::CullMode::Off) {
                    ++reborn::counters().editorParticlesExamined;
                    if (particleOffScreen(this, cfg.editorCullMargin)) {
                        ++reborn::counters().editorParticleDrawsSkipped;
                        return;
                    }
                }
            }
            else if (cfg.cullOffscreenParticles && cfg.particleCull != reborn::CullMode::Off && m_uParticleCount > 0) {
                ++reborn::counters().particleSystemsExamined;
                if (particleOffScreen(this, cfg.particleCullMargin)) {
                    ++reborn::counters().offscreenParticleDrawsSkipped;
                    return;
                }
            }
        }
        CCParticleSystemQuad::draw();
    }
};

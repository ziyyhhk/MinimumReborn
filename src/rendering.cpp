#include <Geode/Geode.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <algorithm>
#include <cmath>
#include "config.hpp"
#include "rendering.hpp"

using namespace geode::prelude;

// Rendering fast paths
//
// ---------------------------------------------------------------------------------------
// 1. CCSprite draw fast-out
// ---------------------------------------------------------------------------------------
// Original path: CCNode::visit calls draw() for every visible child, and CCSprite::draw
// always sets up the quad, binds the program/texture and submits a draw call. Sprites that
// cannot produce a single visible pixel still pay that whole cost:
//   * opacity 0              (faded-out decorations, hidden effect layers)
//   * scale 0 on either axis (collapsed sprites)
//   * empty content size     (placeholder sprites with no quad)
// Skipping draw() for exactly those cases cannot change the image: the quad covers zero
// pixels (or is fully transparent), children are drawn by visit() independently of their
// parent's draw(), and no state is set up. Counters prove how often it fires.
//
// ---------------------------------------------------------------------------------------
// 2. Particle systems
// ---------------------------------------------------------------------------------------
// CCParticleSystemQuad::draw binds a texture, uploads the quad/index buffers and issues a
// draw even when the system has no live particles. Levels keep dozens of systems alive
// (player effects, trails, ground dust) that are idle most of the time. Skipping the draw
// at m_uParticleCount == 0 is visually identical.
//
// Off-screen culling: a relative-position system whose worst-case reach (speed * life plus
// gravity/radial/tangential acceleration, start/end radius, position variance and particle
// size, converted to world units) does not touch the visible rectangle is not drawn. Its
// update still runs, so emission timing and motion are unchanged and it looks the same when
// it scrolls back on screen. Free and grouped systems are always drawn: their particles are
// not placed relative to the emitter and no safe bound exists.
//
// Pool cap: clamping the requested pool BEFORE the original initWithTotalParticles runs
// means the particle, quad and index buffers are all allocated for the capped size, so no
// code can ever index past its buffer. Only affects systems created afterwards.
//
// Spawn budget: once N systems have been created in one frame during a level, further
// systems still exist but get a pool of 4, so an effect burst looks thinner instead of
// stalling the frame. Off by default.
//
// Nothing here touches gameplay, timing or physics. All of it can be switched off in the
// settings; the original path is then taken unchanged.

namespace {

        std::uint64_t g_frameCreatedBase = 0;

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

        bool squareTouches(double cx, double cy, double reach, cocos2d::CCRect const& view) {
            double const left = view.origin.x;
            double const right = view.origin.x + view.size.width;
            double const bottom = view.origin.y;
            double const top = view.origin.y + view.size.height;
            return cx + reach >= left && cx - reach <= right && cy + reach >= bottom && cy - reach <= top;
        }

        bool isFarOffScreen(CCParticleSystemQuad* system) {
            if (system->getPositionType() != kCCPositionTypeRelative) return false;

            CCPoint const origin = system->convertToWorldSpace(ccp(0.f, 0.f));
            CCPoint const unitX = system->convertToWorldSpace(ccp(1.f, 0.f));
            CCPoint const unitY = system->convertToWorldSpace(ccp(0.f, 1.f));
            double const scale = std::max(
                std::hypot(unitX.x - origin.x, unitX.y - origin.y),
                std::hypot(unitY.x - origin.x, unitY.y - origin.y)
            );
            if (!std::isfinite(scale) || scale <= 0.0) return false;

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

            CCSize const size = CCDirector::get()->getVisibleSize();
            CCPoint const visibleOrigin = CCDirector::get()->getVisibleOrigin();
            cocos2d::CCRect const view{visibleOrigin.x, visibleOrigin.y, size.width, size.height};
            return !squareTouches(origin.x, origin.y, reach, view);
        }

        std::uint64_t createdEarlierThisFrame() {
            std::uint64_t const total = reborn::counters().particleSystemsCreated.load(std::memory_order_relaxed);
            return total >= g_frameCreatedBase ? total - g_frameCreatedBase : 0;
        }

        unsigned int limitedPool(unsigned int requested, std::uint64_t createdEarlier) {
            auto const& cfg = reborn::config();
            if (!cfg.enabled) return requested;

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
    }

}  // namespace

namespace reborn {

    void particleFrameBegin() {
        g_frameCreatedBase = reborn::counters().particleSystemsCreated.load(std::memory_order_relaxed);
    }

    unsigned int effectiveParticleCap() {
        return config().particleCap;
    }
}

// The sprite fast-out. This is a real branch in the draw path of every sprite, so it stays
// tiny: three field reads and three comparisons when the feature is on, one bool when off.
struct SpriteDrawFastPath : Modify<SpriteDrawFastPath, CCSprite> {
    void draw() {
        auto const& cfg = reborn::config();
        if (cfg.enabled && cfg.spriteFastPath) {
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
        CCSprite::draw();
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
        if (cfg.enabled) {
            if (cfg.skipIdleParticles && m_uParticleCount == 0) {
                ++reborn::counters().idleParticleDrawsSkipped;
                return;
            }
            if (cfg.cullOffscreenParticles && m_uParticleCount > 0 && isFarOffScreen(this)) {
                ++reborn::counters().offscreenParticleDrawsSkipped;
                return;
            }
        }
        CCParticleSystemQuad::draw();
    }
};

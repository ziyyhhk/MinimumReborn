#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#endif
#include <Geode/utils/cocos.hpp>
#include <Geode/cocos/base_nodes/CCNode.h>
#include <Geode/cocos/include/ccTypes.h>
#include <Geode/cocos/cocoa/CCObject.h>
#include <cstdint>
#include "config.hpp"
#include "renderer_diag.hpp"

using namespace geode::prelude;

// Phase 1 diagnostics for the planned GPU renderer. Read renderer_diag.hpp first.

namespace reborn {

    namespace {
        std::uint64_t s_objects = 0;
        std::uint64_t s_sprites = 0;
        std::uint64_t s_hidden = 0;
        std::uint64_t s_visSum = 0;
        std::uint64_t s_visAvg = 0;
        std::uint64_t s_visMax = 0;
        std::uint64_t s_visSamples = 0;
        std::uint64_t s_visMaxWindow = 0;
    }

    void rendererDiagScan() {
#ifdef GEODE_IS_WINDOWS
        PlayLayer* layer = PlayLayer::get();
        if (!layer) return;

        std::uint64_t objects = 0;
        std::uint64_t sprites = 0;
        std::uint64_t hidden = 0;

        if (layer->m_objects) {
            for (GameObject* object : CCArrayExt<GameObject*>(layer->m_objects)) {
                ++objects;
                if (!object || !object->getChildren()) continue;
                for (CCSprite* sprite : object->getChildrenExt<CCSprite>()) {
                    if (!sprite) continue;
                    ++sprites;
                    if (sprite->getOpacity() == 0
                        || sprite->getScaleX() == 0.f
                        || sprite->getScaleY() == 0.f) {
                        ++hidden;
                    }
                }
            }
        }

        s_objects = objects;
        s_sprites = sprites;
        s_hidden = hidden;
        s_visSum = 0;
        s_visAvg = 0;
        s_visMax = 0;
        s_visSamples = 0;
        s_visMaxWindow = 0;
        log::info(
            "reborn-diag (renderer phase 1): objects={} sprites={} hidden-sprites={} "
            "- the population the GPU visibility pass will target",
            objects, sprites, hidden
        );
#else
        (void)0;
#endif
    }

    void rendererDiagVisibility() {
#ifdef GEODE_IS_WINDOWS
        PlayLayer* layer = PlayLayer::get();
        if (!layer) return;
        int const count = layer->m_visibleObjectsCount;
        std::uint64_t const value = count >= 0 ? static_cast<std::uint64_t>(count) : 0;
        s_visSum += value;
        ++s_visSamples;
        if (value > s_visMaxWindow) s_visMaxWindow = value;
        if (s_visSamples >= 300) {
            s_visAvg = s_visSum / s_visSamples;
            s_visMax = s_visMaxWindow;
            log::info(
                "reborn-diag (renderer phase 1): GD visibility set avg={} max={} "
                "over {} samples - the per-frame object set the GPU pass would need to process",
                s_visAvg, s_visMax, s_visSamples
            );
            s_visSum = 0;
            s_visSamples = 0;
            s_visMaxWindow = 0;
        }
#endif
    }

    std::uint64_t diagObjectCount() { return s_objects; }
    std::uint64_t diagSpriteCount() { return s_sprites; }
    std::uint64_t diagHiddenCount() { return s_hidden; }
    std::uint64_t diagVisibleAvg() { return s_visAvg; }
    std::uint64_t diagVisibleMax() { return s_visMax; }
    std::uint64_t diagVisibleSamples() { return s_visSamples; }

}  // namespace reborn

#ifdef GEODE_IS_WINDOWS

struct RendererDiagPlayLayer : geode::Modify<RendererDiagPlayLayer, PlayLayer> {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        bool ret = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (ret && reborn::config().rendererDiagnostics) {
            reborn::rendererDiagScan();
        }
        return ret;
    }

    void updateVisibility(float dt) {
        PlayLayer::updateVisibility(dt);
        if (reborn::config().rendererDiagnostics) {
            reborn::rendererDiagVisibility();
        }
    }
};

#endif

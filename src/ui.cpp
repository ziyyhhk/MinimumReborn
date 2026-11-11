#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include "config.hpp"
#include "frame_stats.hpp"
#include "hud.hpp"
#include "rendering.hpp"

using namespace geode::prelude;

// The custom panel was removed in v2.3.0. Every setting is edited on the native Geode
// settings page, which the MR button on the pause layer opens directly; the panel this
// file used to host was the only thing here. What remains is the performance overlay
// (corner HUD) and the pause-layer button.

namespace {

    char const* bottleneckLabel(reborn::Bottleneck hint) {
        switch (hint) {
            case reborn::Bottleneck::DrawBound: return "draw path busy";
            case reborn::Bottleneck::GpuOrOther: return "GPU or wait";
            default: return "not measured";
        }
    }

    // Performance overlay. Numbers only; no color semantics.
    class StatsHud : public CCNode {
    protected:
        CCLabelBMFont* m_label = nullptr;
        std::string m_lastText;
        bool m_right = false;
        bool m_bottom = false;

        bool init() override {
            if (!CCNode::init()) return false;
            m_label = CCLabelBMFont::create("Minimum Reborn", "chatFont.fnt");
            if (!m_label) return false;
            this->addChild(m_label);
            this->schedule(schedule_selector(StatsHud::tick), 0.25f);
            this->applyLayout();
            return true;
        }

        void applyLayout() {
            auto const& cfg = reborn::config();
            auto const win = CCDirector::get()->getWinSize();
            bool const right = cfg.overlayCorner == reborn::Corner::TopRight
                            || cfg.overlayCorner == reborn::Corner::BottomRight;
            bool const bottom = cfg.overlayCorner == reborn::Corner::BottomLeft
                             || cfg.overlayCorner == reborn::Corner::BottomRight;

            if (right != m_right) {
                m_label->setAlignment(right ? kCCTextAlignmentRight : kCCTextAlignmentLeft);
                m_right = right;
            }
            m_bottom = bottom;
            m_label->setAnchorPoint({right ? 1.f : 0.f, bottom ? 0.f : 1.f});
            m_label->setPosition({
                right ? win.width - 6.f : 6.f,
                bottom ? 6.f : win.height - 6.f
            });
            m_label->setScale(cfg.overlayScale);
            m_label->setOpacity(cfg.overlayOpacity);
            m_label->setColor(ccc3(238, 241, 248));
            m_label->setVisible(cfg.showOverlay && cfg.enabled);
        }

        void tick(float) {
            auto const& cfg = reborn::config();
            this->applyLayout();
            if (!cfg.showOverlay || !cfg.enabled) return;

            auto& counters = reborn::counters();
            auto& stats = reborn::frameStats();
            std::uint32_t const fps = counters.fps.load(std::memory_order_relaxed);
            std::uint32_t const logic = counters.logicFps.load(std::memory_order_relaxed);
            double const avgMs = stats.averageFrameMs(120);
            double const p95Ms = stats.percentileFrameMs(120, 0.95);
            double const lowFps = stats.onePercentLowFps(240);
            double const workMs = stats.averageWorkMs(60);

            char text[768];
            if (cfg.overlayDetailed) {
                char lowText[32];
                if (lowFps > 0.5) std::snprintf(lowText, sizeof(lowText), "%.0f", lowFps);
                else std::snprintf(lowText, sizeof(lowText), "--");
                char gpuText[32];
                float const gpuMs = counters.gpuFrameMs.load(std::memory_order_relaxed);
                if (gpuMs >= 0.01f) std::snprintf(gpuText, sizeof(gpuText), "%.2f ms", gpuMs);
                else std::snprintf(gpuText, sizeof(gpuText), "--");
                std::snprintf(
                    text, sizeof(text),
                    "%s%u FPS%s | avg %.1f ms | p95 %.1f ms | 1%% low %s\n"
                    "logic %u/s | draw %.2f ms (worst %.2f ms) | gpu %s | %s | renderer %s\n"
                    "scene %u/%u/s | sprite fast-out %u/s | particles %u/%u/s | editor %u/s | capped %u/s | batch culls %llu\n"
                    "quads culled %llu | batch scans %llu | fully culled %llu | sims supp %llu | trace binds %llu",
                    cfg.enabled ? "" : "(OFF) ", fps,
                    counters.throttled.load(std::memory_order_relaxed) ? " (background)" : "",
                    avgMs, p95Ms, lowText,
                    logic, workMs, stats.worstWorkMs(240), gpuText,
                    bottleneckLabel(static_cast<reborn::Bottleneck>(counters.bottleneck.load(std::memory_order_relaxed))),
                    cfg.rendererOptimized ? "optimized" : "engine",
                    counters.sceneCulledPerSec.load(std::memory_order_relaxed),
                    counters.sceneExaminedPerSec.load(std::memory_order_relaxed),
                    counters.spriteSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleExaminedPerSec.load(std::memory_order_relaxed),
                    counters.editorCulledPerSec.load(std::memory_order_relaxed),
                    counters.cappedPerSec.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(
                        counters.batchCulls.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.batchQuadsCulled.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.batchScans.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.batchFullyCulled.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.particleSimsSuppressed.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.shaderTraceBinds.load(std::memory_order_relaxed))
                );
            }
            else {
                std::snprintf(
                    text, sizeof(text), "%s%u FPS | %.1f ms | 1%% low %.0f",
                    cfg.enabled ? "" : "(OFF) ", fps, avgMs, lowFps
                );
            }

            if (m_lastText != text) {
                m_lastText = text;
                m_label->setString(text);
            }
        }

    public:
        static StatsHud* create() {
            auto* ret = new StatsHud();
            if (ret && ret->init()) {
                ret->autorelease();
                return ret;
            }
            CC_SAFE_DELETE(ret);
            return nullptr;
        }
    };

    bool g_hudCreated = false;
}

void openRebornSettings() {
    openSettingsPopup(Mod::get(), true);
}

void installPanelButton(CCNode* layer, CCNode* target, SEL_MenuHandler selector) {
    float const width = 48.f;
    auto* holder = CCNode::create();
    holder->setContentSize({width, width});

    if (auto* icon = CCSprite::create("icon-button.png"_spr)) {
        float const h = icon->getContentSize().height;
        icon->setScale(h > 0.f ? 34.f / h : 1.f);
        icon->setPosition({width / 2.f, width / 2.f});
        holder->addChild(icon, 1);
    }

    auto* button = CCMenuItemSpriteExtra::create(holder, target, selector);
    if (!button) return;
    button->setID("reborn-button"_spr);

    auto* menu = CCMenu::create();
    if (!menu) return;
    menu->setID("reborn-menu"_spr);
    menu->addChild(button);
    menu->setPosition({0.f, 0.f});

    auto* director = CCDirector::get();
    auto const win = director->getWinSize();
    auto const visible = director->getVisibleSize();
    float const x = std::min(win.width / 2.f + 205.f, visible.width - 30.f);
    float const y = std::max(win.height / 2.f - 11.f, 30.f);
    button->setPosition({x, y});
    menu->setTouchPriority(-600);
    layer->addChild(menu, 100);
}

namespace reborn {
    void ensureHud(unsigned int totalFrames) {
        // Create early and let applyLayout control visibility; an overlay created only
        // after frame 300 never appears if the setting is flipped later.
        if (g_hudCreated || totalFrames < 120) return;
        auto* overlay = OverlayManager::get();
        if (!overlay) return;
        if (auto* hud = StatsHud::create()) {
            hud->setID("minimum-reborn-hud"_spr);
            overlay->addChild(hud, 99999);
            g_hudCreated = true;
        }
    }
}

class $modify(RebornPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        installPanelButton(this, this, menu_selector(RebornPauseLayer::onOpenReborn));
    }

    void onOpenReborn(CCObject*) {
        openRebornSettings();
    }
};

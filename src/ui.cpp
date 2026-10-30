#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "benchmark.hpp"
#include "config.hpp"
#include "frame_stats.hpp"
#include "hud.hpp"
#include "rendering.hpp"

using namespace geode::prelude;

// User interface, v1.3.
//
//   * Header: the supplied MK icon, the title, the active profile and the close button.
//   * Six tabs that only exist because each one controls something real:
//       Overview   live numbers, profiles, master switch, benchmark
//       Render     renderer mode, sprite fast-out, particle pool controls
//       Culling    gameplay particle/scene culling and their counters
//       Input      No Additional Input Delay and the Windows timing options
//       Editor     editor-only culling, its counters and its limits
//       Diagnostics overlay options and the detailed counters
//   * Every visible button is a CCMenuItemSpriteExtra with a real callback. Selecting a
//     tab rebuilds only the content card; it installs a fresh menu and never stacks
//     duplicate callbacks on an existing item.
//   * Rounded controls use two small nine-slice PNGs generated from the supplied icon's
//     palette (white shapes tinted at runtime). No per-frame nodes, no animations besides
//     the menu item's own scale feedback.
//
// The popup reads and writes the same settings the native Geode page uses, through the
// cached snapshot. It never changes physics, input handling or the game's FPS cap.

namespace {

    char const* bottleneckLabel(reborn::Bottleneck hint) {
        switch (hint) {
            case reborn::Bottleneck::DrawBound: return "draw path busy";
            case reborn::Bottleneck::GpuOrOther: return "GPU or wait";
            default: return "not measured";
        }
    }

    char const* cullName(reborn::CullMode mode) {
        switch (mode) {
            case reborn::CullMode::Conservative: return "Conservative";
            case reborn::CullMode::Aggressive: return "Aggressive";
            default: return "Off";
        }
    }

    // ---------------------------------------------------------------------------------
    // Performance overlay
    // ---------------------------------------------------------------------------------
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
            this->applyLayout();
            this->schedule(schedule_selector(StatsHud::tick), 0.25f);
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
                std::snprintf(
                    text, sizeof(text),
                    "%s%u FPS%s | avg %.1f ms | p95 %.1f ms | 1%% low %s\n"
                    "logic %u/s | draw %.2f ms | %s | renderer %s\n"
                    "scene %u/%u/s | sprite fast-out %u/s | particles %u/%u/s | editor %u/s | capped %u/s",
                    cfg.enabled ? "" : "(OFF) ", fps,
                    counters.throttled.load(std::memory_order_relaxed) ? " (background)" : "",
                    avgMs, p95Ms, lowText,
                    logic, workMs,
                    bottleneckLabel(static_cast<reborn::Bottleneck>(counters.bottleneck.load(std::memory_order_relaxed))),
                    cfg.rendererOptimized ? "optimized" : "engine",
                    counters.sceneCulledPerSec.load(std::memory_order_relaxed),
                    counters.sceneExaminedPerSec.load(std::memory_order_relaxed),
                    counters.spriteSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleExaminedPerSec.load(std::memory_order_relaxed),
                    counters.editorCulledPerSec.load(std::memory_order_relaxed),
                    counters.cappedPerSec.load(std::memory_order_relaxed)
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

            if (fps == 0) {
                m_label->setColor(ccc3(235, 235, 235));
            }
            else if (static_cast<double>(fps) >= cfg.targetFps * 0.95) {
                m_label->setColor(ccc3(120, 255, 120));
            }
            else if (static_cast<double>(fps) >= cfg.targetFps * 0.75) {
                m_label->setColor(ccc3(255, 225, 90));
            }
            else {
                m_label->setColor(ccc3(255, 100, 100));
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

    // ---------------------------------------------------------------------------------
    // Shared visual helpers
    // ---------------------------------------------------------------------------------
    constexpr float kPopupWidth = 540.f;
    constexpr float kPopupHeight = 350.f;
    constexpr float kPageWidth = 500.f;
    constexpr float kPageHeight = 224.f;
    constexpr float kButtonHeight = 23.f;

    void fitLabel(CCLabelBMFont* label, float maxScale, float room) {
        float const natural = label->getContentSize().width;
        label->setScale(natural > 0.f ? std::min(maxScale, room / natural) : maxScale);
    }

    CCLabelBMFont* makeLabel(
        std::string const& text, char const* font, float scale,
        std::uint8_t opacity = 255, bool left = false
    ) {
        auto* label = CCLabelBMFont::create(text.c_str(), font);
        if (!label) return nullptr;
        label->setScale(scale);
        label->setOpacity(opacity);
        if (left) {
            label->setAnchorPoint({0.f, 0.5f});
            label->setAlignment(kCCTextAlignmentLeft);
        }
        return label;
    }

    CCScale9Sprite* makeCard(float width, float height) {
        auto* card = CCScale9Sprite::create("reborn-card.png", {0.f, 0.f, 96.f, 96.f}, {24.f, 24.f, 24.f, 24.f});
        if (!card) return nullptr;
        card->setContentSize({width, height});
        return card;
    }

    CCScale9Sprite* makeButtonBackground(float width, float height, bool selected) {
        auto* bg = CCScale9Sprite::create(
            selected ? "reborn-button-selected.png" : "reborn-button.png",
            {0.f, 0.f, 64.f, 64.f}, {20.f, 20.f, 20.f, 20.f}
        );
        if (!bg) return nullptr;
        bg->setContentSize({width, height});
        bg->setColor(selected ? ccc3(88, 132, 245) : ccc3(37, 43, 63));
        bg->setOpacity(selected ? 255 : 225);
        return bg;
    }

    enum class CycleKind {
        Renderer, Cull, EditorCull, Detail, Corner, Priority, ParticleCap
    };

    struct CycleRef {
        CCLabelBMFont* label = nullptr;
        CycleKind kind = CycleKind::Renderer;
        std::string key;
        std::string prefix;
        float width = 220.f;
    };

    class RebornPopup : public Popup {
    protected:
        enum Page : int {
            Overview = 0, Render, Culling, Input, Editor, Diagnostics, PageCount
        };

        struct ToggleRef {
            CCMenuItemToggler* item = nullptr;
            std::string key;
        };

        struct ButtonRef {
            CCMenuItemSpriteExtra* item = nullptr;
            CCLabelBMFont* label = nullptr;
            CCScale9Sprite* background = nullptr;
        };

        struct ChipRef {
            CCScale9Sprite* bg = nullptr;
            CCLabelBMFont* label = nullptr;
            std::string name;
        };

        Page m_page = Overview;
        CCNode* m_pageRoot = nullptr;
        CCMenu* m_pageMenu = nullptr;
        CCMenu* m_tabMenu = nullptr;
        CCMenu* m_footerMenu = nullptr;
        std::vector<std::function<void()>> m_actions;
        std::vector<CycleRef> m_cycles;
        std::vector<ToggleRef> m_toggles;
        std::vector<ChipRef> m_chips;
        std::vector<CCScale9Sprite*> m_tabBackgrounds;
        std::vector<CCLabelBMFont*> m_tabLabels;
        CCLabelBMFont* m_headerProfile = nullptr;
        CCLabelBMFont* m_liveLabel = nullptr;
        CCLabelBMFont* m_benchLabel = nullptr;
        std::string m_lastLive;

        static constexpr char const* kPageNames[] = {
            "Overview", "Render", "Culling", "Input", "Editor", "Diag"
        };

        static constexpr char const* kProfileNames[] = {
            "Custom", "Default", "Balanced", "Performance", "Extreme"
        };

        bool init() {
            if (!Popup::init(kPopupWidth, kPopupHeight, "square01_001.png")) return false;
            this->setID("minimum-reborn-popup"_spr);

            this->buildHeader();
            this->buildTabs();
            this->buildFooter();
            this->buildPage();
            this->schedule(schedule_selector(RebornPopup::refreshLive), 0.25f);
            this->refreshLive(0.f);
            return true;
        }

        // -----------------------------------------------------------------------------
        // Header, tabs, footer
        // -----------------------------------------------------------------------------
        void buildHeader() {
            auto* logo = CCSprite::create("logo.png");
            if (logo) {
                float const h = logo->getContentSize().height;
                logo->setScale(h > 0.f ? 42.f / h : 1.f);
                m_mainLayer->addChildAtPosition(logo, Anchor::Center, ccp(-235.f, 145.f));
            }

            if (auto* title = makeLabel("MINIMUM REBORN", "bigFont.fnt", 0.62f)) {
                title->setAnchorPoint({0.f, 0.5f});
                m_mainLayer->addChildAtPosition(title, Anchor::Center, ccp(-204.f, 151.f));
            }
            if (auto* sub = makeLabel("draw-only optimization  |  v1.3.0", "chatFont.fnt", 0.42f, 190, true)) {
                m_mainLayer->addChildAtPosition(sub, Anchor::Center, ccp(-203.f, 137.f));
            }

            if (auto* caption = makeLabel("ACTIVE PROFILE", "chatFont.fnt", 0.34f, 150, true)) {
                caption->setAnchorPoint({1.f, 0.5f});
                m_mainLayer->addChildAtPosition(caption, Anchor::Center, ccp(200.f, 154.f));
            }
            m_headerProfile = makeLabel("Balanced", "goldFont.fnt", 0.46f, 255, true);
            if (m_headerProfile) {
                m_headerProfile->setAnchorPoint({1.f, 0.5f});
                m_mainLayer->addChildAtPosition(m_headerProfile, Anchor::Center, ccp(200.f, 141.f));
            }
        }

        void buildTabs() {
            m_tabMenu = CCMenu::create();
            m_tabMenu->setPosition({0.f, 0.f});
            m_mainLayer->addChild(m_tabMenu, 10);

            float const width = 78.f;
            float const gap = 3.f;
            float const total = 6.f * width + 5.f * gap;
            float x = -total / 2.f + width / 2.f;

            for (int i = 0; i < static_cast<int>(PageCount); ++i) {
                auto* holder = CCNode::create();
                holder->setContentSize({width, 24.f});
                auto* bg = makeButtonBackground(width, 24.f, i == static_cast<int>(m_page));
                if (bg) {
                    bg->setPosition({width / 2.f, 12.f});
                    holder->addChild(bg);
                }
                auto* label = makeLabel(kPageNames[i], "bigFont.fnt", 0.39f);
                if (label) {
                    fitLabel(label, 0.39f, width - 8.f);
                    label->setPosition({width / 2.f, 12.f});
                    holder->addChild(label);
                }
                auto* item = CCMenuItemSpriteExtra::create(holder, this, menu_selector(RebornPopup::onTab));
                item->setTag(i);
                m_tabMenu->addChild(item);
                item->setPosition({x + static_cast<float>(i) * (width + gap), 112.f});
                m_tabBackgrounds.push_back(bg);
                m_tabLabels.push_back(label);
            }
        }

        void buildFooter() {
            m_footerMenu = CCMenu::create();
            m_footerMenu->setPosition({0.f, 0.f});
            m_mainLayer->addChild(m_footerMenu, 10);

            this->footerButton("Reset", -120.f, menu_selector(RebornPopup::onReset));
            this->footerButton("All Settings", 0.f, menu_selector(RebornPopup::onAllSettings));
            this->footerButton("Close", 120.f, menu_selector(RebornPopup::closePopup));
        }

        void footerButton(char const* text, float x, SEL_MenuHandler selector) {
            float const width = text[0] == 'A' ? 120.f : (text[0] == 'R' ? 92.f : 92.f);
            auto* holder = CCNode::create();
            holder->setContentSize({width, 26.f});
            if (auto* bg = makeButtonBackground(width, 26.f, false)) {
                bg->setPosition({width / 2.f, 13.f});
                holder->addChild(bg);
            }
            if (auto* label = makeLabel(text, "bigFont.fnt", 0.42f)) {
                fitLabel(label, 0.42f, width - 10.f);
                label->setPosition({width / 2.f, 13.f});
                holder->addChild(label);
            }
            auto* item = CCMenuItemSpriteExtra::create(holder, this, selector);
            m_footerMenu->addChild(item);
            item->setPosition({x, -154.f});
        }

        void onTab(CCObject* sender) {
            int const page = static_cast<CCNode*>(sender)->getTag();
            if (page < 0 || page >= static_cast<int>(PageCount) || page == static_cast<int>(m_page)) return;
            m_page = static_cast<Page>(page);
            this->buildPage();
            this->refreshTabs();
        }

        void refreshTabs() {
            for (int i = 0; i < static_cast<int>(m_tabBackgrounds.size()); ++i) {
                bool const selected = i == static_cast<int>(m_page);
                auto* bg = m_tabBackgrounds[i];
                if (bg) {
                    bg->setColor(selected ? ccc3(88, 132, 245) : ccc3(37, 43, 63));
                    bg->setOpacity(selected ? 255 : 225);
                }
                if (i < static_cast<int>(m_tabLabels.size()) && m_tabLabels[i]) {
                    m_tabLabels[i]->setColor(selected ? ccc3(255, 255, 255) : ccc3(196, 204, 224));
                }
            }
        }

        // -----------------------------------------------------------------------------
        // Page building
        // -----------------------------------------------------------------------------
        void buildPage() {
            if (m_pageRoot) {
                m_pageRoot->removeFromParentAndCleanup(true);
                m_pageRoot = nullptr;
            }
            m_pageMenu = nullptr;
            m_actions.clear();
            m_cycles.clear();
            m_toggles.clear();
            m_chips.clear();
            m_liveLabel = nullptr;
            m_benchLabel = nullptr;
            m_lastLive.clear();

            m_pageRoot = CCNode::create();
            m_pageRoot->setContentSize({kPageWidth, kPageHeight});
            m_pageRoot->setAnchorPoint({0.5f, 0.5f});
            m_mainLayer->addChildAtPosition(m_pageRoot, Anchor::Center, ccp(0.f, -16.f));

            if (auto* card = makeCard(kPageWidth, kPageHeight)) {
                m_pageRoot->addChild(card);
            }

            m_pageMenu = CCMenu::create();
            m_pageMenu->setPosition({0.f, 0.f});
            m_pageRoot->addChild(m_pageMenu);

            switch (m_page) {
                case Overview: this->buildOverview(); break;
                case Render: this->buildRender(); break;
                case Culling: this->buildCulling(); break;
                case Input: this->buildInput(); break;
                case Editor: this->buildEditor(); break;
                default: this->buildDiagnostics(); break;
            }

            this->refreshPage();
        }

        CCLabelBMFont* textAt(
            std::string const& text, float x, float y, float scale,
            char const* font = "bigFont.fnt", std::uint8_t opacity = 255, bool left = false
        ) {
            auto* label = makeLabel(text, font, scale, opacity, left);
            if (label) m_pageRoot->addChild(label), label->setPosition({x, y});
            return label;
        }

        ButtonRef buttonAt(
            float x, float y, float width, float height, std::string const& text,
            std::function<void()> action, bool selected = false, float fontScale = 0.4f
        ) {
            ButtonRef ref;
            auto* holder = CCNode::create();
            holder->setContentSize({width, height});
            if (auto* bg = makeButtonBackground(width, height, selected)) {
                bg->setPosition({width / 2.f, height / 2.f});
                holder->addChild(bg);
                ref.background = bg;
            }
            auto* label = makeLabel(text, "bigFont.fnt", fontScale);
            if (label) {
                label->setTag(1);
                fitLabel(label, fontScale, width - 12.f);
                label->setPosition({width / 2.f, height / 2.f});
                holder->addChild(label);
            }
            auto* item = CCMenuItemSpriteExtra::create(holder, this, menu_selector(RebornPopup::onAction));
            item->setTag(static_cast<int>(m_actions.size()));
            m_actions.push_back(std::move(action));
            if (m_pageMenu) {
                m_pageMenu->addChild(item);
                item->setPosition({x, y});
            }
            ref.item = item;
            ref.label = label;
            return ref;
        }

        void cycleAt(
            float x, float y, float width, std::string const& prefix,
            CycleKind kind, std::string key, std::function<void()> advance
        ) {
            auto ref = this->buttonAt(
                x, y, width, kButtonHeight, prefix + ": ...", std::move(advance), false, 0.37f
            );
            m_cycles.push_back({ref.label, kind, std::move(key), prefix, width});
        }

        void toggleAt(float x, float y, std::string key, std::string const& text) {
            auto* toggler = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(RebornPopup::onToggle), 0.55f
            );
            if (!toggler) return;
            toggler->setTag(static_cast<int>(m_toggles.size()));
            m_pageMenu->addChild(toggler);
            toggler->setPosition({x, y});
            m_toggles.push_back({toggler, key});

            if (auto* label = makeLabel(text, "bigFont.fnt", 0.4f, 235, true)) {
                m_pageRoot->addChild(label);
                label->setPosition({x + 18.f, y});
            }
        }

        void buildOverview() {
            m_liveLabel = textAt("--", 0.f, 76.f, 0.5f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            if (auto* caption = textAt("PROFILE", 0.f, 38.f, 0.34f, "chatFont.fnt", 150)) {
                caption->setAlignment(kCCTextAlignmentCenter);
            }

            float const width = 86.f;
            float const gap = 6.f;
            float const total = 5.f * width + 4.f * gap;
            float x = -total / 2.f + width / 2.f;
            for (int i = 0; i < 5; ++i) {
                std::string name = kProfileNames[i];
                auto ref = this->buttonAt(
                    x + static_cast<float>(i) * (width + gap), 15.f, width, 21.f, name,
                    [this, name]() {
                        Mod::get()->setSettingValue<std::string>("profile", name);
                        reborn::refreshConfig();
                        this->refreshPage();
                    }, false, 0.35f
                );
                m_chips.push_back({ref.background, ref.label, name});
            }

            this->toggleAt(-132.f, -22.f, "enabled", "Mod enabled");
            this->toggleAt(52.f, -22.f, "show-overlay", "Performance overlay");

            auto bench = this->buttonAt(-62.f, -80.f, 104.f, kButtonHeight, "Benchmark",
                [this]() { this->toggleBenchmark(); }, false, 0.38f);
            m_benchLabel = bench.label;
            this->buttonAt(58.f, -80.f, 104.f, kButtonHeight, "Geode Settings",
                [this]() { this->onAllSettings(nullptr); }, false, 0.36f);

            textAt(
                "Physics, collision, input timing and the game's FPS cap are never changed.",
                0.f, -102.f, 0.38f, "chatFont.fnt", 165
            );
        }

        void buildRender() {
            this->cycleAt(-100.f, 84.f, 220.f, "Renderer", CycleKind::Renderer, "renderer-mode",
                this->makeCycleAction(CycleKind::Renderer, "renderer-mode"));

            this->toggleAt(-158.f, 52.f, "sprite-fast-path", "Sprite draw fast-out");
            this->toggleAt(-158.f, 24.f, "skip-idle-particles", "Skip idle particle draws");
            this->toggleAt(-158.f, -4.f, "cap-particles", "Cap particle pool size");
            this->toggleAt(-158.f, -32.f, "adaptive-cap", "Adaptive particle cap (experimental)");

            this->cycleAt(-100.f, -58.f, 180.f, "Particle cap", CycleKind::ParticleCap, "particle-cap",
                this->makeCycleAction(CycleKind::ParticleCap, "particle-cap"));

#ifdef GEODE_IS_WINDOWS
            this->toggleAt(-158.f, -86.f, "render-gate", "Render gate (experimental)");
#endif
            textAt(
                "Draw-only. Order, blending, glow, clipping, shaders and transforms are untouched.",
                0.f, -104.f, 0.37f, "chatFont.fnt", 165
            );
        }

        void buildCulling() {
            this->cycleAt(-94.f, 82.f, 240.f, "Particles", CycleKind::Cull, "particle-cull-mode",
                this->makeCycleAction(CycleKind::Cull, "particle-cull-mode"));
            this->cycleAt(-94.f, 52.f, 240.f, "Scene sprites", CycleKind::Cull, "scene-cull",
                this->makeCycleAction(CycleKind::Cull, "scene-cull"));

            m_liveLabel = textAt("--", 0.f, 10.f, 0.43f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "Particle systems keep updating and emitting even when their draw is skipped.\n"
                "Scene culling only decides whether an unbatched sprite is submitted; nodes,\n"
                "triggers, collisions and children are untouched.",
                -234.f, -46.f, 0.38f, "chatFont.fnt", 185, true
            );
            textAt(
                "GD batches level objects, so scene culling mostly affects UI and effect sprites.",
                -234.f, -94.f, 0.37f, "chatFont.fnt", 150, true
            );
        }

        void buildInput() {
            this->toggleAt(-158.f, 84.f, "no-input-delay", "No Additional Input Delay");

            textAt(
                "Moves all mod bookkeeping out of the pre-update part of the frame: the\n"
                "scheduler counter runs after the game's update, and statistics, benchmark\n"
                "sampling and the Win32 focus probe run after the scene is drawn. The mod\n"
                "then adds no work before the game processes input or simulation.",
                -234.f, 49.f, 0.38f, "chatFont.fnt", 195, true
            );

#ifdef GEODE_IS_WINDOWS
            this->toggleAt(-158.f, -2.f, "timer-resolution", "1 ms timer resolution");
            this->toggleAt(-158.f, -28.f, "background-throttle", "Throttle drawing in background");
            this->cycleAt(-100.f, -56.f, 190.f, "Priority", CycleKind::Priority, "process-priority",
                this->makeCycleAction(CycleKind::Priority, "process-priority"));
#else
            textAt("Windows-only timing options are hidden on this platform.", -234.f, -6.f,
                0.38f, "chatFont.fnt", 160, true);
#endif

            m_liveLabel = textAt("--", 0.f, -84.f, 0.4f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "Ordering change only. Game input handling, physics, timers and the FPS cap are untouched.",
                0.f, -104.f, 0.36f, "chatFont.fnt", 150
            );
        }

        void buildEditor() {
            this->toggleAt(-158.f, 86.f, "editor-culling", "Editor visibility and particle culling");
            this->cycleAt(-94.f, 56.f, 240.f, "Editor mode", CycleKind::EditorCull, "editor-cull-mode",
                this->makeCycleAction(CycleKind::EditorCull, "editor-cull-mode"));

            m_liveLabel = textAt("--", 0.f, 18.f, 0.41f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "Applies the same draw-only tests while the editor is open. Object data,\n"
                "selection, dragging, undo, copy/paste, triggers and saving are never touched.\n"
                "Level objects are batched by GD, so only unbatched editor sprites and\n"
                "particle systems can actually be skipped. The counters show the real result.",
                -234.f, -24.f, 0.38f, "chatFont.fnt", 190, true
            );

            textAt(
                "Open the editor to populate these counters. Nothing is removed from the level.",
                0.f, -102.f, 0.36f, "chatFont.fnt", 150
            );
        }

        void buildDiagnostics() {
            this->toggleAt(-158.f, 88.f, "show-overlay", "Performance overlay");
            this->cycleAt(-94.f, 60.f, 220.f, "Detail", CycleKind::Detail, "overlay-detail",
                this->makeCycleAction(CycleKind::Detail, "overlay-detail"));
            this->cycleAt(-94.f, 32.f, 220.f, "Corner", CycleKind::Corner, "overlay-corner",
                this->makeCycleAction(CycleKind::Corner, "overlay-corner"));

            auto bench = this->buttonAt(-100.f, -4.f, 180.f, kButtonHeight, "Benchmark",
                [this]() { this->toggleBenchmark(); }, false, 0.38f);
            m_benchLabel = bench.label;

            m_liveLabel = textAt("--", 0.f, -42.f, 0.4f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "draw ms is CPU time inside the drawScene call, not GPU time. Batch counts\n"
                "come from GD's CCSpriteBatchNode path. No GPU timer is claimed.",
                -234.f, -78.f, 0.37f, "chatFont.fnt", 180, true
            );
            textAt(
                "Benchmark: start here, close the panel, then resume the level. Pause screens are excluded.",
                0.f, -104.f, 0.35f, "chatFont.fnt", 150
            );
        }

        // -----------------------------------------------------------------------------
        // Actions
        // -----------------------------------------------------------------------------
        std::function<void()> makeCycleAction(CycleKind kind, std::string key) {
            return [this, kind, key]() {
                auto* mod = Mod::get();
                if (kind == CycleKind::Renderer) {
                    bool const optimized = mod->getSettingValue<std::string>("renderer-mode") != "Engine";
                    mod->setSettingValue<std::string>("renderer-mode", optimized ? "Engine" : "Optimized");
                    this->markCustom();
                }
                else if (kind == CycleKind::Cull || kind == CycleKind::EditorCull) {
                    std::string const current = mod->getSettingValue<std::string>(key);
                    std::string const next = current == "Off" ? "Conservative"
                                           : current == "Conservative" ? "Aggressive" : "Off";
                    mod->setSettingValue<std::string>(key, next);
                    if (kind == CycleKind::Cull) this->markCustom();
                }
                else if (kind == CycleKind::Detail) {
                    std::string const current = mod->getSettingValue<std::string>("overlay-detail");
                    mod->setSettingValue<std::string>("overlay-detail", current == "Compact" ? "Detailed" : "Compact");
                }
                else if (kind == CycleKind::Corner) {
                    std::string const current = mod->getSettingValue<std::string>("overlay-corner");
                    std::string const next = current == "Top Right" ? "Bottom Right"
                                           : current == "Bottom Right" ? "Bottom Left"
                                           : current == "Bottom Left" ? "Top Left" : "Top Right";
                    mod->setSettingValue<std::string>("overlay-corner", next);
                }
                else if (kind == CycleKind::Priority) {
                    std::string const current = mod->getSettingValue<std::string>("process-priority");
                    std::string const next = current == "Normal" ? "Above Normal"
                                           : current == "Above Normal" ? "High" : "Normal";
                    mod->setSettingValue<std::string>("process-priority", next);
                }
                else if (kind == CycleKind::ParticleCap) {
                    int64_t const current = mod->getSettingValue<int64_t>("particle-cap");
                    int64_t next = 512;
                    if (current < 64) next = 64;
                    else if (current < 128) next = 128;
                    else if (current < 256) next = 256;
                    else if (current < 512) next = 512;
                    else next = 32;
                    mod->setSettingValue<int64_t>("particle-cap", next);
                    this->markCustom();
                }
                reborn::refreshConfig();
                this->refreshPage();
            };
        }

        void markCustom() {
            if (Mod::get()->getSettingValue<std::string>("profile") != "Custom") {
                Mod::get()->setSettingValue<std::string>("profile", "Custom");
            }
        }

        void onAction(CCObject* sender) {
            int const index = static_cast<CCNode*>(sender)->getTag();
            if (index >= 0 && index < static_cast<int>(m_actions.size())) {
                m_actions[static_cast<std::size_t>(index)]();
            }
        }

        void onToggle(CCObject* sender) {
            auto* toggler = static_cast<CCMenuItemToggler*>(sender);
            int const index = toggler ? toggler->getTag() : -1;
            if (index < 0 || index >= static_cast<int>(m_toggles.size())) return;
            std::string const& key = m_toggles[static_cast<std::size_t>(index)].key;
            auto* mod = Mod::get();
            bool const next = !mod->getSettingValue<bool>(key);
            if (key != "show-overlay" && key != "no-input-delay" && key != "editor-culling"
                && key != "timer-resolution") {
                this->markCustom();
            }
            mod->setSettingValue<bool>(key, next);
            reborn::refreshConfig();
            this->refreshPage();
        }

        void toggleBenchmark() {
            if (reborn::benchmarkActive()) {
                reborn::benchmarkStop(std::chrono::steady_clock::now());
            }
            else {
                int64_t seconds = Mod::get()->getSettingValue<int64_t>("benchmark-seconds");
                seconds = std::clamp<int64_t>(seconds, 3, 60);
                reborn::benchmarkStart(std::chrono::steady_clock::now(), static_cast<double>(seconds));
            }
            this->refreshPage();
        }

        void onReset(CCObject*) {
            createQuickPopup(
                "Reset Minimum Reborn",
                "Restore every setting to the <cy>Balanced</c> defaults?\n"
                "The current profile and all toggles will be replaced.",
                "Cancel", "Reset",
                [this](FLAlertLayer*, bool confirmed) {
                    if (!confirmed) return;
                    reborn::resetToDefaults();
                    this->refreshPage();
                }
            );
        }

        void onAllSettings(CCObject*) {
            this->onClose(nullptr);
            openSettingsPopup(Mod::get(), true);
        }

        void closePopup(CCObject*) {
            this->onClose(nullptr);
        }

        // -----------------------------------------------------------------------------
        // Refresh
        // -----------------------------------------------------------------------------
        void refreshPage() {
            auto* mod = Mod::get();
            std::string const profile = mod->getSettingValue<std::string>("profile");
            if (m_headerProfile) m_headerProfile->setString(profile.c_str());

            for (auto& t : m_toggles) {
                if (t.item) t.item->toggle(mod->getSettingValue<bool>(t.key));
            }

            for (auto& c : m_cycles) {
                if (!c.label) continue;
                std::string text = c.prefix + ": " + this->cycleValue(c, mod);
                c.label->setString(text.c_str());
                fitLabel(c.label, 0.4f, c.width - 12.f);
            }

            for (auto& chip : m_chips) {
                bool const selected = chip.name == profile;
                if (chip.bg) {
                    chip.bg->setColor(selected ? ccc3(88, 132, 245) : ccc3(37, 43, 63));
                    chip.bg->setOpacity(selected ? 255 : 225);
                }
                if (chip.label) chip.label->setColor(selected ? ccc3(255, 255, 255) : ccc3(208, 214, 232));
            }

            if (m_benchLabel) {
                m_benchLabel->setString(reborn::benchmarkActive() ? "Stop benchmark" : "Benchmark");
            }
            this->refreshTabs();
            this->refreshLive(0.f);
        }

        std::string cycleValue(CycleRef const& c, Mod* mod) {
            switch (c.kind) {
                case CycleKind::Renderer:
                    return mod->getSettingValue<std::string>("renderer-mode");
                case CycleKind::Cull:
                case CycleKind::EditorCull:
                    return mod->getSettingValue<std::string>(c.key);
                case CycleKind::Detail:
                    return mod->getSettingValue<std::string>("overlay-detail");
                case CycleKind::Corner:
                    return mod->getSettingValue<std::string>("overlay-corner");
                case CycleKind::Priority:
                    return mod->getSettingValue<std::string>("process-priority");
                case CycleKind::ParticleCap:
                    return std::to_string(mod->getSettingValue<int64_t>("particle-cap"));
            }
            return "--";
        }

        void refreshLive(float) {
            if (!m_liveLabel) return;
            auto& counters = reborn::counters();
            auto& stats = reborn::frameStats();
            auto const& cfg = reborn::config();
            char text[640];

            if (m_page == Overview) {
                double const low = stats.onePercentLowFps(240);
                char lowText[16];
                if (low > 0.5) std::snprintf(lowText, sizeof(lowText), "%.0f", low);
                else std::snprintf(lowText, sizeof(lowText), "--");
                std::snprintf(
                    text, sizeof(text),
                    "%u FPS   |   avg %.1f ms   |   1%% low %s\n"
                    "logic %u/s   |   %s renderer   |   %s",
                    counters.fps.load(std::memory_order_relaxed),
                    stats.averageFrameMs(120), lowText,
                    counters.logicFps.load(std::memory_order_relaxed),
                    cfg.rendererOptimized ? "optimized" : "engine",
                    cfg.noInputDelay ? "no added input bookkeeping" : "legacy frame order"
                );
            }
            else if (m_page == Culling) {
                std::snprintf(
                    text, sizeof(text),
                    "scene  %u skipped / %u tested per second\n"
                    "particles  %u skipped / %u tested per second\n"
                    "empty batches skipped %llu   |   editor culls %u/s",
                    counters.sceneCulledPerSec.load(std::memory_order_relaxed),
                    counters.sceneExaminedPerSec.load(std::memory_order_relaxed),
                    counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleExaminedPerSec.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(counters.emptyBatchDrawsSkipped.load(std::memory_order_relaxed)),
                    counters.editorCulledPerSec.load(std::memory_order_relaxed)
                );
            }
            else if (m_page == Editor) {
                std::snprintf(
                    text, sizeof(text),
                    "%s   |   %s\n"
                    "sprites  %llu skipped / %llu tested\n"
                    "particles  %llu skipped / %llu tested   |   %u/s now",
                    reborn::inEditor() ? "EDITOR ACTIVE" : "not in the editor",
                    cfg.editorCulling ? cullName(cfg.editorCull) : "off",
                    static_cast<unsigned long long>(counters.editorSpriteDrawsSkipped.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.editorSpritesExamined.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.editorParticleDrawsSkipped.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.editorParticlesExamined.load(std::memory_order_relaxed)),
                    counters.editorCulledPerSec.load(std::memory_order_relaxed)
                );
            }
            else if (m_page == Input) {
                std::snprintf(
                    text, sizeof(text),
                    "frame-end bookkeeping: %llu frames   |   scheduler %llu\n"
                    "order: %s",
                    static_cast<unsigned long long>(counters.deferredBookkeeping.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.schedulerUpdates.load(std::memory_order_relaxed)),
                    cfg.noInputDelay ? "game update first, mod work last" : "mod counters before the game update"
                );
            }
            else if (m_page == Diagnostics) {
                double const low = stats.onePercentLowFps(240);
                std::snprintf(
                    text, sizeof(text),
                    "%u FPS | avg %.1f ms | p95 %.1f ms | 1%% low %.0f\n"
                    "logic %u/s | draw %.2f ms | %s\n"
                    "batches %llu | quads %llu | empty %llu",
                    counters.fps.load(std::memory_order_relaxed),
                    stats.averageFrameMs(120), stats.percentileFrameMs(120, 0.95), low,
                    counters.logicFps.load(std::memory_order_relaxed),
                    stats.averageWorkMs(60),
                    bottleneckLabel(static_cast<reborn::Bottleneck>(counters.bottleneck.load(std::memory_order_relaxed))),
                    static_cast<unsigned long long>(counters.batchDraws.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.batchQuads.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.emptyBatchDrawsSkipped.load(std::memory_order_relaxed))
                );
            }
            else {
                std::snprintf(
                    text, sizeof(text),
                    "%u FPS | avg %.1f ms | 1%% low %.0f",
                    counters.fps.load(std::memory_order_relaxed),
                    stats.averageFrameMs(120), stats.onePercentLowFps(240)
                );
            }

            if (m_lastLive != text) {
                m_lastLive = text;
                m_liveLabel->setString(text);
            }
        }

    public:
        static RebornPopup* create() {
            auto* ret = new RebornPopup();
            if (ret->init()) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }
    };
}

namespace reborn {
    void ensureHud(unsigned int totalFrames) {
        // The old build created the overlay only after frame 300 *and* only if the overlay
        // setting was on at that moment, so turning it on later produced no overlay until
        // the next launch. Create it early and let applyLayout control visibility.
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

        float const width = 48.f;
        auto* holder = CCNode::create();
        holder->setContentSize({width, width});

        auto* bg = CCScale9Sprite::create(
            "reborn-button.png", {0.f, 0.f, 64.f, 64.f}, {20.f, 20.f, 20.f, 20.f}
        );
        if (bg) {
            bg->setContentSize({width, width});
            bg->setColor(ccc3(37, 43, 63));
            bg->setOpacity(235);
            bg->setPosition({width / 2.f, width / 2.f});
            holder->addChild(bg);
        }

        if (auto* icon = CCSprite::create("logo.png")) {
            float const h = icon->getContentSize().height;
            icon->setScale(h > 0.f ? 34.f / h : 1.f);
            icon->setPosition({width / 2.f, width / 2.f});
            holder->addChild(icon, 1);
        }

        auto* button = CCMenuItemSpriteExtra::create(
            holder, this, menu_selector(RebornPauseLayer::onOpenReborn)
        );
        button->setID("reborn-button"_spr);

        auto* menu = CCMenu::create();
        menu->setID("reborn-menu"_spr);
        menu->addChild(button);
        menu->setPosition({0.f, 0.f});

        auto const win = CCDirector::get()->getWinSize();
        button->setPosition({win.width - 32.f, win.height - 32.f});
        menu->setTouchPriority(-600);
        this->addChild(menu, 100);
    }

    void onOpenReborn(CCObject*) {
        if (auto* popup = RebornPopup::create()) {
            popup->show();
        }
    }
};

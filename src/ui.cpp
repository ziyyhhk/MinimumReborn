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
    // The popup is deliberately smaller than the 320-unit design height so it reads as a
    // window over the paused game instead of a full-screen dashboard. 470x284 design units
    // leave roughly a 100x37 pixel margin at 1176x664.
    constexpr float kPopupWidth = 470.f;
    constexpr float kPopupHeight = 284.f;
    constexpr float kPageWidth = 446.f;
    constexpr float kPageHeight = 168.f;
    constexpr float kButtonHeight = 22.f;

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

    // Native GD nine-slice. Custom PNGs are automatically downsized by the Geode packer
    // (a 96px card becomes a 24px base sprite), which makes hard-coded source rects and
    // cap insets unreliable. square02b_001.png is 80x80 in every GD install and is what
    // GD itself uses for menu buttons, so it always renders as a rounded control.
    CCScale9Sprite* makeNineSlice(float width, float height, float inset, bool selected) {
        auto* bg = CCScale9Sprite::create(
            "square02b_001.png", {0.f, 0.f, 80.f, 80.f}, {inset, inset, inset, inset}
        );
        if (!bg) return nullptr;
        bg->setAnchorPoint({0.5f, 0.5f});
        bg->setContentSize({width, height});
        bg->setColor(selected ? ccc3(88, 132, 245) : ccc3(52, 60, 82));
        bg->setOpacity(selected ? 255 : 225);
        return bg;
    }

    CCScale9Sprite* makeCard(float width, float height) {
        auto* card = makeNineSlice(width, height, 12.f, false);
        if (!card) return nullptr;
        card->setColor(ccc3(5, 8, 16));
        card->setOpacity(110);
        return card;
    }

    CCScale9Sprite* makeButtonBackground(float width, float height, bool selected) {
        return makeNineSlice(width, height, 10.f, selected);
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
            // Keep GD's own popup frame. It is opaque, native to the game and does not
            // depend on packed custom nine-slices, which the packer downsizes.
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
            auto* logo = CCSprite::create("logo-128.png"_spr);
            if (logo) {
                float const h = logo->getContentSize().height;
                logo->setScale(h > 0.f ? 30.f / h : 1.f);
                m_mainLayer->addChildAtPosition(logo, Anchor::Center, ccp(-196.f, 118.f));
            }

            if (auto* title = makeLabel("MINIMUM REBORN", "bigFont.fnt", 0.5f)) {
                title->setAnchorPoint({0.f, 0.5f});
                m_mainLayer->addChildAtPosition(title, Anchor::Center, ccp(-176.f, 121.f));
            }
            if (auto* sub = makeLabel("draw-only optimization  |  v1.3.2", "chatFont.fnt", 0.35f, 185, true)) {
                m_mainLayer->addChildAtPosition(sub, Anchor::Center, ccp(-175.f, 109.f));
            }

            if (auto* caption = makeLabel("ACTIVE PROFILE", "chatFont.fnt", 0.3f, 145, true)) {
                caption->setAnchorPoint({1.f, 0.5f});
                m_mainLayer->addChildAtPosition(caption, Anchor::Center, ccp(172.f, 122.f));
            }
            m_headerProfile = makeLabel("Balanced", "goldFont.fnt", 0.4f, 255, true);
            if (m_headerProfile) {
                m_headerProfile->setAnchorPoint({1.f, 0.5f});
                m_mainLayer->addChildAtPosition(m_headerProfile, Anchor::Center, ccp(172.f, 110.f));
            }
        }

        void buildTabs() {
            m_tabMenu = CCMenu::create();
            m_tabMenu->setPosition({kPopupWidth / 2.f, kPopupHeight / 2.f});
            m_mainLayer->addChild(m_tabMenu, 10);

            float const width = 68.f;
            float const gap = 3.f;
            float const total = 6.f * width + 5.f * gap;
            float x = -total / 2.f + width / 2.f;

            for (int i = 0; i < static_cast<int>(PageCount); ++i) {
                auto* holder = CCNode::create();
                holder->setContentSize({width, 22.f});
                auto* bg = makeButtonBackground(width, 22.f, i == static_cast<int>(m_page));
                if (bg) {
                    bg->setPosition({width / 2.f, 11.f});
                    holder->addChild(bg);
                }
                auto* label = makeLabel(kPageNames[i], "bigFont.fnt", 0.34f);
                if (label) {
                    fitLabel(label, 0.34f, width - 8.f);
                    label->setPosition({width / 2.f, 11.f});
                    holder->addChild(label);
                }
                auto* item = CCMenuItemSpriteExtra::create(holder, this, menu_selector(RebornPopup::onTab));
                item->setTag(i);
                m_tabMenu->addChild(item);
                item->setPosition({x + static_cast<float>(i) * (width + gap), 90.f});
                m_tabBackgrounds.push_back(bg);
                m_tabLabels.push_back(label);
            }
        }

        void buildFooter() {
            m_footerMenu = CCMenu::create();
            m_footerMenu->setPosition({kPopupWidth / 2.f, kPopupHeight / 2.f});
            m_mainLayer->addChild(m_footerMenu, 10);

            this->footerButton("Reset", -120.f, menu_selector(RebornPopup::onReset));
            this->footerButton("All Settings", 0.f, menu_selector(RebornPopup::onAllSettings));
            this->footerButton("Close", 120.f, menu_selector(RebornPopup::closePopup));
        }

        void footerButton(char const* text, float x, SEL_MenuHandler selector) {
            float const width = text[0] == 'A' ? 120.f : (text[0] == 'R' ? 92.f : 92.f);
            auto* holder = CCNode::create();
            holder->setContentSize({width, 22.f});
            if (auto* bg = makeButtonBackground(width, 22.f, false)) {
                bg->setPosition({width / 2.f, 11.f});
                holder->addChild(bg);
            }
            if (auto* label = makeLabel(text, "bigFont.fnt", 0.4f)) {
                fitLabel(label, 0.4f, width - 10.f);
                label->setPosition({width / 2.f, 11.f});
                holder->addChild(label);
            }
            auto* item = CCMenuItemSpriteExtra::create(holder, this, selector);
            m_footerMenu->addChild(item);
            item->setPosition({x, -118.f});
        }

        void onTab(CCObject* sender) {
            int const page = static_cast<CCNode*>(sender)->getTag();
            if (page < 0 || page >= static_cast<int>(PageCount) || page == static_cast<int>(m_page)) return;
            m_page = static_cast<Page>(page);
            this->refreshTabs();
            // The sender lives inside the page tree that buildPage() replaces. Rebuilding in
            // the same callback would free the menu item while CCMenu is still dispatching
            // its touch release. Defer to the next tick instead.
            this->scheduleOnce(schedule_selector(RebornPopup::applyTab), 0.f);
        }

        void applyTab(float) {
            this->buildPage();
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
            m_mainLayer->addChildAtPosition(m_pageRoot, Anchor::Center, ccp(0.f, -13.f));

            if (auto* card = makeCard(kPageWidth, kPageHeight)) {
                card->setPosition({kPageWidth / 2.f, kPageHeight / 2.f});
                m_pageRoot->addChild(card);
            }

            m_pageMenu = CCMenu::create();
            m_pageMenu->setPosition({kPageWidth / 2.f, kPageHeight / 2.f});
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
            if (label) {
                m_pageRoot->addChild(label);
                label->setPosition({kPageWidth / 2.f + x, kPageHeight / 2.f + y});
            }
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
                label->setPosition({kPageWidth / 2.f + x + 18.f, kPageHeight / 2.f + y});
            }
        }

        void buildOverview() {
            m_liveLabel = textAt("--", 0.f, 54.f, 0.42f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            if (auto* caption = textAt("PROFILE", 0.f, 24.f, 0.3f, "chatFont.fnt", 150)) {
                caption->setAlignment(kCCTextAlignmentCenter);
            }

            float const width = 80.f;
            float const gap = 5.f;
            float const total = 5.f * width + 4.f * gap;
            float x = -total / 2.f + width / 2.f;
            for (int i = 0; i < 5; ++i) {
                std::string name = kProfileNames[i];
                auto ref = this->buttonAt(
                    x + static_cast<float>(i) * (width + gap), 5.f, width, 20.f, name,
                    [this, name]() {
                        Mod::get()->setSettingValue<std::string>("profile", name);
                        reborn::refreshConfig();
                        this->refreshPage();
                    }, false, 0.33f
                );
                m_chips.push_back({ref.background, ref.label, name});
            }

            this->toggleAt(-148.f, -24.f, "enabled", "Mod enabled");
            this->toggleAt(46.f, -24.f, "show-overlay", "Performance overlay");

            auto bench = this->buttonAt(-76.f, -58.f, 100.f, 21.f, "Benchmark",
                [this]() { this->toggleBenchmark(); }, false, 0.36f);
            m_benchLabel = bench.label;
            this->buttonAt(54.f, -58.f, 112.f, 21.f, "Geode Settings",
                [this]() { this->onAllSettings(nullptr); }, false, 0.34f);

            textAt(
                "Physics, collision, input timing and the game's FPS cap are never changed.",
                0.f, -76.f, 0.3f, "chatFont.fnt", 160
            );
        }

        void buildRender() {
            this->cycleAt(-20.f, 60.f, 250.f, "Renderer", CycleKind::Renderer, "renderer-mode",
                this->makeCycleAction(CycleKind::Renderer, "renderer-mode"));

            this->toggleAt(-160.f, 34.f, "sprite-fast-path", "Sprite draw fast-out");
            this->toggleAt(-160.f, 10.f, "skip-idle-particles", "Skip idle particle draws");
            this->toggleAt(-160.f, -14.f, "cap-particles", "Cap particle pool size");
            this->toggleAt(-160.f, -38.f, "adaptive-cap", "Adaptive cap (experimental)");

            this->cycleAt(-76.f, -64.f, 170.f, "Particle cap", CycleKind::ParticleCap, "particle-cap",
                this->makeCycleAction(CycleKind::ParticleCap, "particle-cap"));
#ifdef GEODE_IS_WINDOWS
            this->toggleAt(102.f, -64.f, "render-gate", "Render gate");
#endif
            textAt(
                "Draw-only: order, blending, glow, clipping and shaders are untouched.",
                0.f, -79.f, 0.28f, "chatFont.fnt", 155
            );
        }

        void buildCulling() {
            this->cycleAt(-20.f, 58.f, 280.f, "Particles", CycleKind::Cull, "particle-cull-mode",
                this->makeCycleAction(CycleKind::Cull, "particle-cull-mode"));
            this->cycleAt(-20.f, 30.f, 280.f, "Scene sprites", CycleKind::Cull, "scene-cull",
                this->makeCycleAction(CycleKind::Cull, "scene-cull"));

            m_liveLabel = textAt("--", 0.f, -6.f, 0.4f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "Particle systems keep updating and emitting when their draw is skipped.\n"
                "Scene culling only decides whether an unbatched sprite is submitted;\n"
                "nodes, triggers, collisions and children are untouched.",
                -216.f, -46.f, 0.33f, "chatFont.fnt", 185, true
            );
            textAt(
                "GD batches level objects, so in-level culling mostly covers UI and effect sprites.",
                0.f, -78.f, 0.28f, "chatFont.fnt", 150
            );
        }

        void buildInput() {
            this->toggleAt(-160.f, 64.f, "no-input-delay", "No Additional Input Delay");

            textAt(
                "Moves mod bookkeeping out of the pre-update part of the frame: the scheduler\n"
                "count runs after the game update, and stats, benchmark sampling and the Win32\n"
                "focus probe run after the scene is drawn. No work is added before input.",
                -216.f, 30.f, 0.32f, "chatFont.fnt", 195, true
            );

#ifdef GEODE_IS_WINDOWS
            this->toggleAt(-160.f, -12.f, "timer-resolution", "1 ms timer resolution");
            this->toggleAt(-160.f, -36.f, "background-throttle", "Throttle drawing in background");
            this->cycleAt(-36.f, -58.f, 210.f, "Priority", CycleKind::Priority, "process-priority",
                this->makeCycleAction(CycleKind::Priority, "process-priority"));
#else
            textAt("Windows-only timing options are hidden on this platform.", -216.f, -20.f,
                0.34f, "chatFont.fnt", 160, true);
#endif

            m_liveLabel = textAt("--", 0.f, -78.f, 0.28f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);
        }

        void buildEditor() {
            this->toggleAt(-160.f, 64.f, "editor-culling", "Editor visibility and particle culling");
            this->cycleAt(-30.f, 36.f, 250.f, "Editor mode", CycleKind::EditorCull, "editor-cull-mode",
                this->makeCycleAction(CycleKind::EditorCull, "editor-cull-mode"));

            m_liveLabel = textAt("--", 0.f, 0.f, 0.36f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "Applies the same draw-only tests while the editor is open. Object data, selection,\n"
                "dragging, undo, copy/paste, triggers and saving are never touched. GD batches level\n"
                "objects, so only unbatched sprites and particle systems can actually be skipped.",
                -216.f, -44.f, 0.32f, "chatFont.fnt", 190, true
            );

            textAt(
                "Open the editor to populate these counters. Nothing is removed from the level.",
                0.f, -78.f, 0.28f, "chatFont.fnt", 150
            );
        }

        void buildDiagnostics() {
            this->toggleAt(-160.f, 62.f, "show-overlay", "Performance overlay");
            this->cycleAt(-36.f, 34.f, 240.f, "Detail", CycleKind::Detail, "overlay-detail",
                this->makeCycleAction(CycleKind::Detail, "overlay-detail"));
            this->cycleAt(-36.f, 8.f, 240.f, "Corner", CycleKind::Corner, "overlay-corner",
                this->makeCycleAction(CycleKind::Corner, "overlay-corner"));

            auto bench = this->buttonAt(-70.f, -20.f, 160.f, 21.f, "Benchmark",
                [this]() { this->toggleBenchmark(); }, false, 0.36f);
            m_benchLabel = bench.label;

            m_liveLabel = textAt("--", 0.f, -50.f, 0.32f, "chatFont.fnt");
            if (m_liveLabel) m_liveLabel->setAlignment(kCCTextAlignmentCenter);

            textAt(
                "draw ms is CPU time inside drawScene, not GPU time. No GPU timer is claimed.",
                0.f, -78.f, 0.28f, "chatFont.fnt", 160
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
                    "frame-end bookkeeping %llu frames   |   scheduler %llu   |   order: %s",
                    static_cast<unsigned long long>(counters.deferredBookkeeping.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(counters.schedulerUpdates.load(std::memory_order_relaxed)),
                    cfg.noInputDelay ? "game update first" : "counters first"
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
            "square02b_001.png", {0.f, 0.f, 80.f, 80.f}, {12.f, 12.f, 12.f, 12.f}
        );
        if (bg) {
            bg->setAnchorPoint({0.5f, 0.5f});
            bg->setContentSize({width, width});
            bg->setColor(ccc3(46, 54, 76));
            bg->setOpacity(235);
            bg->setPosition({width / 2.f, width / 2.f});
            holder->addChild(bg);
        }

        if (auto* icon = CCSprite::create("icon-button.png"_spr)) {
            float const h = icon->getContentSize().height;
            icon->setScale(h > 0.f ? 34.f / h : 1.f);
            icon->setPosition({width / 2.f, width / 2.f});
            holder->addChild(icon, 1);
        }
        else if (auto* fallback = makeLabel("MR", "bigFont.fnt", 0.5f)) {
            fallback->setPosition({width / 2.f, width / 2.f});
            holder->addChild(fallback, 1);
        }

        auto* button = CCMenuItemSpriteExtra::create(
            holder, this, menu_selector(RebornPauseLayer::onOpenReborn)
        );
        button->setID("reborn-button"_spr);

        auto* menu = CCMenu::create();
        menu->setID("reborn-menu"_spr);
        menu->addChild(button);
        menu->setPosition({0.f, 0.f});

        // Sits just past the native pause row (which ends around +158 design units from
        // centre) instead of in a corner. Eclipse's draggable menu button can occupy any
        // corner, so the row remains the one place it does not cover.
        auto const win = CCDirector::get()->getWinSize();
        button->setPosition({win.width / 2.f + 205.f, win.height / 2.f - 11.f});
        menu->setTouchPriority(-600);
        this->addChild(menu, 100);
    }

    void onOpenReborn(CCObject*) {
        if (auto* popup = RebornPopup::create()) {
            popup->show();
        }
    }
};

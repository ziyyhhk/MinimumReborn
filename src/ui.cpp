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

// User interface, v1.4.
//
//   * Header: the MK icon, the title, a subtitle, the active profile and the close button.
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
//   * Every control is built from one spacing scale (see the layout constants) and the
//     pill backgrounds use GD's own nine-slice with cap insets derived from the texture the
//     engine actually loads. No per-frame nodes and no animations.
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
    // One spacing system for the whole panel. Every row, tab and button is placed from
    // these numbers, so the header, nav, content and footer share margins and baselines.
    constexpr float kPopupWidth = 400.f;
    constexpr float kPopupHeight = 240.f;
    constexpr float kMargin = 18.f;
    constexpr float kHeaderH = 34.f;
    constexpr float kNavH = 26.f;
    constexpr float kFooterH = 28.f;
    constexpr float kRowPitch = 21.f;
    constexpr float kButtonHeight = 20.f;
    constexpr float kGap = 5.f;
    constexpr float kIconSize = 26.f;
    constexpr float kContentWidth = kPopupWidth - 2.f * kMargin;
    constexpr float kTabWidth = kContentWidth / 6.f;
    // The popup's own close button lives in the top-right corner; the header's right-hand
    // block stops short of it.
    constexpr float kProfileRightX = kPopupWidth / 2.f - kMargin - 26.f;

    ccColor3B const kTextPrimary = ccc3(242, 244, 248);
    ccColor3B const kTextMuted = ccc3(176, 186, 204);

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

    // Native GD nine-slice. Two traps, both learned from screenshots:
    //   * Custom PNGs are downsized by the Geode packer (a 96px card becomes a 24px base
    //     sprite), so hard-coded rects on our own assets are unreliable. square02b_001.png
    //     is a game sprite and is what GD itself uses for menu buttons.
    //   * The cap insets are in *texture pixels*, and the texture this GD build actually
    //     loads is not necessarily 80x80. Reading the size from the texture cache and taking
    //     a proportion of it keeps the corners intact; a fixed number produced a stretched
    //     right edge and a thin bar sticking out of every pill.
    // -----------------------------------------------------------------------------
    // Flat rectangles
    //
    // Two things were tried for these and both are wrong in practice:
    //   * stretching GD's square02b_001.png into a 20-unit row drags the trimmed
    //     transparent margin of its atlas frame into a sliver at one end, and a
    //     nine-slice with hard-coded cap insets lands on the wrong sub-rect (rounded left
    //     end, square right end);
    //   * CCLayerColor builds its quad relative to the layer position in a way that does
    //     not follow the anchor point here, so the bars and bodies ended up mirrored away
    //     from where they were placed.
    // A 16x16 solid white PNG owned by the mod has nothing to trim and nothing to flip:
    // set the size, set the color, done. Rounded ends are given up deliberately.
    // -----------------------------------------------------------------------------
    char const* const kRectSprite = "reborn-pixel.png"_spr;

    CCSprite* makeRect(float width, float height, ccColor3B color, std::uint8_t opacity) {
        auto* rect = CCSprite::create(kRectSprite);
        if (!rect) return nullptr;
        rect->setAnchorPoint({0.5f, 0.5f});
        // Scale rather than setContentSize: the sprite is a solid untrimmed square, so a
        // scale factor is exact, while contentSize on a sprite goes through the frame's
        // offset handling (which is what stretched the GD square into a sliver).
        CCSize const natural = rect->getContentSize();
        if (natural.width > 0.f && natural.height > 0.f) {
            rect->setScaleX(width / natural.width);
            rect->setScaleY(height / natural.height);
        }
        rect->setColor(color);
        rect->setOpacity(opacity);
        return rect;
    }

    class Pill : public CCNode {
    protected:
        CCSprite* m_body = nullptr;
        CCSprite* m_top = nullptr;

        bool init(float width, float height, ccColor3B color, std::uint8_t opacity) {
            if (!CCNode::init()) return false;
            this->setAnchorPoint({0.5f, 0.5f});
            this->setContentSize({width, height});

            m_body = makeRect(width, height, color, opacity);
            if (!m_body) return false;
            m_body->setPosition({width / 2.f, height / 2.f});
            this->addChild(m_body);

            m_top = makeRect(width, 1.f, ccc3(255, 255, 255), 30);
            if (m_top) {
                m_top->setPosition({width / 2.f, height - 0.5f});
                this->addChild(m_top);
            }
            return true;
        }

    public:
        static Pill* create(float width, float height, ccColor3B color, std::uint8_t opacity) {
            auto* ret = new Pill();
            if (ret && ret->init(width, height, color, opacity)) {
                ret->autorelease();
                return ret;
            }
            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        void tint(ccColor3B color, std::uint8_t opacity) {
            if (m_body) {
                m_body->setColor(color);
                m_body->setOpacity(opacity);
            }
        }
    };

    ccColor3B const kPillIdle = ccc3(46, 52, 68);
    ccColor3B const kPillSelected = ccc3(52, 132, 96);

    Pill* makeButtonBackground(float width, float height, bool selected) {
        return Pill::create(width, height,
                            selected ? kPillSelected : kPillIdle,
                            selected ? 255 : 235);
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
            Pill* background = nullptr;
        };

        struct ChipRef {
            Pill* bg = nullptr;
            CCLabelBMFont* label = nullptr;
            std::string name;
        };

        struct MetricRef {
            std::string id;
            CCLabelBMFont* label = nullptr;
            std::string last;
        };

        Page m_page = Overview;
        CCNode* m_pageRoot = nullptr;
        CCNode* m_navRoot = nullptr;
        CCMenu* m_pageMenu = nullptr;
        CCMenu* m_tabMenu = nullptr;
        CCMenu* m_footerMenu = nullptr;
        CCSprite* m_tabAccent = nullptr;
        std::vector<std::function<void()>> m_actions;
        std::vector<CycleRef> m_cycles;
        std::vector<ToggleRef> m_toggles;
        std::vector<ChipRef> m_chips;
        std::vector<CCLabelBMFont*> m_tabLabels;
        std::vector<MetricRef> m_metrics;
        CCLabelBMFont* m_headerProfile = nullptr;
        CCLabelBMFont* m_benchLabel = nullptr;

        static constexpr char const* kPageNames[] = {
            "Overview", "Render", "Culling", "Input", "Editor", "Diag"
        };

        static constexpr char const* kProfileNames[] = {
            "Custom", "Default", "Balanced", "Performance", "Extreme"
        };

        bool init() {
            // GD's own popup frame: opaque, native, and its nine-slice is built by the Popup
            // base class. GJ_square01.png (Geode's default) is the brown level-info panel,
            // which reads as a completely different UI language.
            if (!Popup::init(kPopupWidth, kPopupHeight, "square01_001.png")) return false;
            this->setID("minimum-reborn-popup"_spr);

            this->buildHeader();
            this->buildNav();
            this->buildFooter();
            this->buildPage();
            this->schedule(schedule_selector(RebornPopup::refreshLive), 0.25f);
            this->refreshLive(0.f);
            return true;
        }

        // -----------------------------------------------------------------------------
        // Layout primitives
        //
        // Everything is derived from the panel size and one spacing scale, so the header,
        // nav, content rows and footer share the same margins instead of each control
        // carrying its own hard-coded coordinate.
        // -----------------------------------------------------------------------------
        static constexpr float kLeft = -kPopupWidth / 2.f + kMargin;
        static constexpr float kRight = kPopupWidth / 2.f - kMargin;
        static constexpr float kHeaderCenterY = kPopupHeight / 2.f - kHeaderH / 2.f;
        static constexpr float kNavCenterY = kPopupHeight / 2.f - kHeaderH - kNavH / 2.f;
        static constexpr float kFooterY = -kPopupHeight / 2.f + kFooterH / 2.f + 2.f;
        static constexpr float kRowTop = kPopupHeight / 2.f - kHeaderH - kNavH - kRowPitch / 2.f;

        // m_mainLayer covers the whole panel with its origin in the bottom-left corner, so
        // the centered coordinates the layout produces are shifted here once, in the
        // placement helpers, rather than at every call site.
        static constexpr float kCX = kPopupWidth / 2.f;
        static constexpr float kCY = kPopupHeight / 2.f;

        static float rowY(int row) {
            return kRowTop - static_cast<float>(row) * kRowPitch;
        }

        static float tabCenter(int index) {
            return kLeft + kTabWidth * (static_cast<float>(index) + 0.5f);
        }

        CCLabelBMFont* addLabel(
            CCNode* parent, std::string const& text, char const* font, float scale,
            ccColor3B color, std::uint8_t opacity, float x, float y, int align
        ) {
            auto* label = CCLabelBMFont::create(text.c_str(), font);
            if (!label) return nullptr;
            label->setScale(scale);
            label->setColor(color);
            label->setOpacity(opacity);
            // align: 0 = centered, 1 = left anchored, 2 = right anchored
            if (align == 1) {
                label->setAnchorPoint({0.f, 0.5f});
                label->setAlignment(kCCTextAlignmentLeft);
            }
            else if (align == 2) {
                label->setAnchorPoint({1.f, 0.5f});
                label->setAlignment(kCCTextAlignmentRight);
            }
            label->setPosition({kCX + x, kCY + y});
            parent->addChild(label);
            return label;
        }

        void buildHeader() {
            CCNode* root = m_mainLayer;

            // icon-button.png is the MK badge that the pause button already loads from the
            // package, so this is the asset whose loading is proven at runtime.
            if (auto* icon = CCSprite::create("icon-button.png"_spr)) {
                float const h = icon->getContentSize().height;
                icon->setScale(h > 0.f ? kIconSize / h : 1.f);
                icon->setPosition({kCX + kLeft + kIconSize / 2.f, kCY + kHeaderCenterY - 5.f});
                root->addChild(icon);
            }

            float const textX = kLeft + kIconSize + 8.f;
            addLabel(root, "MINIMUM REBORN", "bigFont.fnt", 0.42f, kTextPrimary, 255,
                     textX, kHeaderCenterY - 1.f, 1);
            addLabel(root, "Performance and rendering", "chatFont.fnt", 0.3f, kTextMuted, 200,
                     textX, kHeaderCenterY - 11.f, 1);

            addLabel(root, "PROFILE", "chatFont.fnt", 0.26f, kTextMuted, 190,
                     kProfileRightX, kHeaderCenterY - 2.f, 2);
            m_headerProfile = addLabel(root, "Balanced", "goldFont.fnt", 0.42f, kTextPrimary,
                                       255, kProfileRightX, kHeaderCenterY - 12.f, 2);
        }

        void buildNav() {
            m_navRoot = CCNode::create();
            m_mainLayer->addChild(m_navRoot);

            // Baseline the tabs sit on, plus the accent bar that marks the selected tab.
            if (auto* base = makeRect(kContentWidth, 1.f, ccc3(255, 255, 255), 26)) {
                // The rect sprite is anchored at its centre, so the panel centre is its
                // position - adding half the width here pushed the rule off the right edge.
                base->setPosition({kCX, kCY + kNavCenterY - kNavH / 2.f});
                m_navRoot->addChild(base);
            }
            m_tabAccent = makeRect(kTabWidth - 16.f, 2.f, ccc3(96, 214, 141), 235);
            if (m_tabAccent) {
                m_tabAccent->setPosition({kCX, kCY});
                m_navRoot->addChild(m_tabAccent);
            }

            m_tabMenu = CCMenu::create();
            m_tabMenu->setPosition({kCX, kCY});
            m_navRoot->addChild(m_tabMenu, 2);

            for (int i = 0; i < static_cast<int>(PageCount); ++i) {
                auto* holder = CCNode::create();
                holder->setContentSize({kTabWidth, kNavH - 6.f});
                auto* label = makeLabel(kPageNames[i], "bigFont.fnt", 0.33f, 210, false);
                if (label) {
                    label->setColor(kTextMuted);
                    fitLabel(label, 0.33f, kTabWidth - 8.f);
                    label->setPosition({kTabWidth / 2.f, (kNavH - 6.f) / 2.f});
                    holder->addChild(label);
                }

                auto* item = CCMenuItemSpriteExtra::create(
                    holder, this, menu_selector(RebornPopup::onTab)
                );
                item->setTag(i);
                m_tabMenu->addChild(item);
                item->setPosition({tabCenter(i), kNavCenterY});
                m_tabLabels.push_back(label);
            }
            this->refreshTabs();
        }

        void buildFooter() {
            m_footerMenu = CCMenu::create();
            m_footerMenu->setPosition({kCX, kCY});
            m_mainLayer->addChild(m_footerMenu, 10);

            this->footerButton("Reset", kLeft + 48.f, 96.f, menu_selector(RebornPopup::onReset));
            this->footerButton("All Settings", 0.f, 118.f, menu_selector(RebornPopup::onAllSettings));
            this->footerButton("Close", kRight - 48.f, 96.f, menu_selector(RebornPopup::closePopup));
        }

        void footerButton(char const* text, float x, float width, SEL_MenuHandler selector) {
            auto* holder = CCNode::create();
            holder->setContentSize({width, kButtonHeight});
            if (auto* bg = makeButtonBackground(width, kButtonHeight, false)) {
                bg->setPosition({width / 2.f, kButtonHeight / 2.f});
                holder->addChild(bg);
            }
            if (auto* label = makeLabel(text, "bigFont.fnt", 0.36f)) {
                fitLabel(label, 0.36f, width - 12.f);
                label->setPosition({width / 2.f, kButtonHeight / 2.f});
                holder->addChild(label);
            }
            auto* item = CCMenuItemSpriteExtra::create(holder, this, selector);
            m_footerMenu->addChild(item);
            item->setPosition({x, kFooterY});
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
            for (int i = 0; i < static_cast<int>(m_tabLabels.size()); ++i) {
                bool const selected = i == static_cast<int>(m_page);
                auto* label = m_tabLabels[static_cast<std::size_t>(i)];
                if (!label) continue;
                label->setScale(1.f);
                label->setColor(selected ? kTextPrimary : kTextMuted);
                label->setOpacity(selected ? 255 : 200);
                fitLabel(label, selected ? 0.35f : 0.33f, kTabWidth - 8.f);
            }
            if (m_tabAccent) {
                m_tabAccent->setPosition({kCX + tabCenter(static_cast<int>(m_page)),
                                          kCY + kNavCenterY - kNavH / 2.f + 1.5f});
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
            m_metrics.clear();
            m_benchLabel = nullptr;

            m_pageRoot = CCNode::create();
            m_pageRoot->setContentSize({kPopupWidth, kPopupHeight});
            m_mainLayer->addChild(m_pageRoot, 5);

            m_pageMenu = CCMenu::create();
            m_pageMenu->setPosition({kCX, kCY});
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

        // Row builders. Each takes a row index, so a page reads as a list of rows and every
        // row shares the same pitch, margins and baselines.
        void headingRow(int row, char const* text) {
            addLabel(m_pageRoot, text, "chatFont.fnt", 0.34f, ccc3(122, 204, 158), 235,
                     kLeft + 2.f, rowY(row), 1);
        }

        void noteRow(int row, std::string const& text, float scale = 0.3f, std::uint8_t opacity = 165) {
            // CCLabelBMFont has no wrap width in this binding, so notes are written to fit:
            // at 0.3 chatFont a line of this length is roughly half the content width.
            addLabel(m_pageRoot, text, "chatFont.fnt", scale, kTextMuted, opacity,
                     kLeft + 2.f, rowY(row), 1);
        }

        void statRow(int row, char const* label, char const* metric) {
            addLabel(m_pageRoot, label, "chatFont.fnt", 0.34f, kTextMuted, 215,
                     kLeft + 2.f, rowY(row), 1);
            auto* value = addLabel(m_pageRoot, "--", "chatFont.fnt", 0.36f, kTextPrimary, 255,
                                   kRight - 2.f, rowY(row), 2);
            if (value) m_metrics.push_back({metric, value, {}});
        }

        // labelX lets two toggles share a row (the second one starts mid-panel).
        void toggleRow(int row, std::string key, char const* text, float labelX = kLeft + 28.f) {
            float const boxX = labelX - 17.f;
            auto* toggler = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(RebornPopup::onToggle), 0.5f
            );
            if (!toggler) return;
            toggler->setTag(static_cast<int>(m_toggles.size()));
            m_pageMenu->addChild(toggler);
            toggler->setPosition({boxX, rowY(row)});
            m_toggles.push_back({toggler, std::move(key)});

            addLabel(m_pageRoot, text, "bigFont.fnt", 0.36f, kTextPrimary, 235,
                     labelX, rowY(row), 1);
        }

        ButtonRef pillAt(
            float x, float y, float width, std::string const& text,
            std::function<void()> action, float fontScale = 0.36f
        ) {
            ButtonRef ref;
            auto* holder = CCNode::create();
            holder->setContentSize({width, kButtonHeight});
            if (auto* bg = makeButtonBackground(width, kButtonHeight, false)) {
                bg->setPosition({width / 2.f, kButtonHeight / 2.f});
                holder->addChild(bg);
                ref.background = bg;
            }
            auto* label = makeLabel(text, "bigFont.fnt", fontScale);
            if (label) {
                label->setTag(1);
                fitLabel(label, fontScale, width - 12.f);
                label->setPosition({width / 2.f, kButtonHeight / 2.f});
                holder->addChild(label);
            }
            auto* item = CCMenuItemSpriteExtra::create(
                holder, this, menu_selector(RebornPopup::onAction)
            );
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

        void cycleRow(
            int row, char const* prefix, CycleKind kind, std::string key,
            float width = kContentWidth, float x = 0.f
        ) {
            auto ref = this->pillAt(x, rowY(row), width, " ",
                                    std::function<void()>(), 0.34f);
            m_cycles.push_back({ref.label, kind, std::move(key), prefix, width});
            // pillAt registered the empty action above; this row's action is the cycle step.
            m_actions.back() = this->makeCycleAction(kind, key);
        }

        void buildOverview() {
            headingRow(0, "OVERVIEW - LIVE");
            statRow(1, "Frame", "frame");
            statRow(2, "Pipeline", "pipeline");

            float const chipW = (kContentWidth - 4.f * kGap) / 5.f;
            for (int i = 0; i < 5; ++i) {
                std::string name = kProfileNames[i];
                float const x = kLeft + chipW / 2.f + static_cast<float>(i) * (chipW + kGap);
                auto ref = this->pillAt(x, rowY(3), chipW, name,
                    [this, name]() {
                        Mod::get()->setSettingValue<std::string>("profile", name);
                        reborn::refreshConfig();
                        this->refreshPage();
                    }, 0.3f
                );
                m_chips.push_back({ref.background, ref.label, name});
            }

            this->toggleRow(4, "enabled", "Mod enabled");
            this->toggleRow(5, "show-overlay", "Performance overlay");

            float const half = (kContentWidth - kGap) / 2.f;
            auto bench = this->pillAt(kLeft + half / 2.f, rowY(6), half, "Benchmark",
                [this]() { this->toggleBenchmark(); }, 0.34f);
            m_benchLabel = bench.label;
            this->pillAt(kRight - half / 2.f, rowY(6), half, "Geode Settings",
                [this]() { this->onAllSettings(nullptr); }, 0.34f);
        }

        void buildRender() {
            cycleRow(0, "Renderer", CycleKind::Renderer, "renderer-mode");
            this->toggleRow(1, "sprite-fast-path", "Sprite draw fast-out");
            this->toggleRow(2, "skip-idle-particles", "Skip idle particle draws");
            this->toggleRow(3, "cap-particles", "Cap particle pool size");
            this->toggleRow(4, "adaptive-cap", "Adaptive cap (experimental)");

            float const half = (kContentWidth - kGap) / 2.f;
            this->cycleRow(5, "Particle cap", CycleKind::ParticleCap, "particle-cap", half,
                           kLeft + half / 2.f);
#ifdef GEODE_IS_WINDOWS
            this->toggleRow(5, "render-gate", "Render gate", kRight - half + 28.f);
#endif
            statRow(6, "Work avoided", "avoided");
        }

        void buildCulling() {
            cycleRow(0, "Particles", CycleKind::Cull, "particle-cull-mode");
            cycleRow(1, "Scene sprites", CycleKind::Cull, "scene-cull");
            headingRow(2, "PER SECOND");
            statRow(3, "Scene sprites", "scene");
            statRow(4, "Particle systems", "particles");
            statRow(5, "Batches", "batches");
            noteRow(6, "Draw-only. Simulation, physics, collisions and triggers are untouched.");
        }

        void buildInput() {
            this->toggleRow(0, "no-input-delay", "No Additional Input Delay");
            statRow(1, "Frame order", "input");
            noteRow(2, "Bookkeeping runs after the game update and after the scene draw. "
                       "Nothing is added before input.");

#ifdef GEODE_IS_WINDOWS
            this->toggleRow(3, "timer-resolution", "1 ms timer resolution");
            this->toggleRow(4, "background-throttle", "Throttle drawing in background");
            cycleRow(5, "Priority", CycleKind::Priority, "process-priority");
            noteRow(6, "Windows-only. Physics, input handling and the FPS cap never change.");
#else
            noteRow(3, "Windows-only timing options are hidden on this platform.");
#endif
        }

        void buildEditor() {
            this->toggleRow(0, "editor-culling", "Editor visibility and particle culling");
            cycleRow(1, "Editor mode", CycleKind::EditorCull, "editor-cull-mode");
            statRow(2, "State", "editor-state");
            statRow(3, "Sprite draws", "editor-sprites");
            statRow(4, "Particle draws", "editor-particles");
            noteRow(5, "Draw-only. Object data, selection, dragging, undo, triggers and saving "
                       "are untouched.");
            noteRow(6, "Open the editor to populate these counters.", 0.3f, 140);
        }

        void buildDiagnostics() {
            this->toggleRow(0, "show-overlay", "Performance overlay");
            cycleRow(1, "Overlay detail", CycleKind::Detail, "overlay-detail");
            cycleRow(2, "Overlay corner", CycleKind::Corner, "overlay-corner");
            headingRow(3, "DIAGNOSTICS - MEASURED, NOT ESTIMATED");
            statRow(4, "Frame", "diag-frame");
            statRow(5, "CPU draw time", "diag-cpu");
            auto bench = this->pillAt(0.f, rowY(6), 200.f, "Benchmark",
                [this]() { this->toggleBenchmark(); }, 0.34f);
            m_benchLabel = bench.label;
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
                && key != "timer-resolution" && key != "adaptive-cap") {
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
                fitLabel(c.label, 0.34f, c.width - 12.f);
            }

            for (auto& chip : m_chips) {
                bool const selected = chip.name == profile;
                if (chip.bg) {
                    chip.bg->tint(selected ? kPillSelected : kPillIdle, selected ? 255 : 235);
                }
                if (chip.label) chip.label->setColor(selected ? kTextPrimary : kTextMuted);
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

        // One labelled value per metric id. A metric with no samples answers "--" instead of
        // inventing a number.
        std::string metricValue(std::string const& id) {
            auto& counters = reborn::counters();
            auto& stats = reborn::frameStats();
            auto const& cfg = reborn::config();
            char text[256];

            auto const fps = counters.fps.load(std::memory_order_relaxed);
            auto const logic = counters.logicFps.load(std::memory_order_relaxed);
            auto const u64 = [](std::atomic<std::uint64_t>& v) {
                return static_cast<unsigned long long>(v.load(std::memory_order_relaxed));
            };
            auto const intOf = [](double value) {
                return value > 0.5 ? std::to_string(static_cast<int>(value)) : std::string("--");
            };

            if (id == "frame") {
                std::snprintf(text, sizeof(text), "%u FPS   %.1f ms avg   1%% low %s",
                              fps, stats.averageFrameMs(120),
                              intOf(stats.onePercentLowFps(240)).c_str());
            }
            else if (id == "pipeline") {
                std::snprintf(text, sizeof(text), "%u logic/s   %s renderer   %s", logic,
                              cfg.rendererOptimized ? "optimized" : "engine",
                              cfg.noInputDelay ? "game first" : "counters first");
            }
            else if (id == "avoided") {
                std::snprintf(text, sizeof(text), "%llu sprite   %llu particle   %llu batch",
                              u64(counters.invisibleSpriteDrawsSkipped)
                                  + u64(counters.sceneSpriteDrawsSkipped),
                              u64(counters.idleParticleDrawsSkipped)
                                  + u64(counters.offscreenParticleDrawsSkipped)
                                  + u64(counters.invisibleParticleDrawsSkipped),
                              u64(counters.emptyBatchDrawsSkipped));
            }
            else if (id == "scene") {
                std::snprintf(text, sizeof(text), "%u skipped / %u tested",
                              counters.sceneCulledPerSec.load(std::memory_order_relaxed),
                              counters.sceneExaminedPerSec.load(std::memory_order_relaxed));
            }
            else if (id == "particles") {
                std::snprintf(text, sizeof(text), "%u skipped / %u tested",
                              counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                              counters.particleExaminedPerSec.load(std::memory_order_relaxed));
            }
            else if (id == "batches") {
                std::snprintf(text, sizeof(text), "%llu empty skipped / %llu drawn",
                              u64(counters.emptyBatchDrawsSkipped), u64(counters.batchDraws));
            }
            else if (id == "input") {
                std::snprintf(text, sizeof(text), "%llu deferred / %llu scheduler",
                              u64(counters.deferredBookkeeping), u64(counters.schedulerUpdates));
            }
            else if (id == "editor-state") {
                std::snprintf(text, sizeof(text), "%s   %s",
                              reborn::inEditor() ? "editor active" : "not in the editor",
                              cfg.editorCulling ? cullName(cfg.editorCull) : "off");
            }
            else if (id == "editor-sprites") {
                std::snprintf(text, sizeof(text), "%llu skipped / %llu tested",
                              u64(counters.editorSpriteDrawsSkipped),
                              u64(counters.editorSpritesExamined));
            }
            else if (id == "editor-particles") {
                std::snprintf(text, sizeof(text), "%llu skipped / %llu tested",
                              u64(counters.editorParticleDrawsSkipped),
                              u64(counters.editorParticlesExamined));
            }
            else if (id == "diag-frame") {
                std::snprintf(text, sizeof(text), "%u FPS   avg %.1f   p95 %.1f ms   1%% low %s",
                              fps, stats.averageFrameMs(120), stats.percentileFrameMs(120, 0.95),
                              intOf(stats.onePercentLowFps(240)).c_str());
            }
            else if (id == "diag-cpu") {
                std::snprintf(text, sizeof(text), "%.2f ms in drawScene   jitter %.2f ms",
                              stats.averageWorkMs(60), stats.jitterMs(120));
            }
            else {
                std::snprintf(text, sizeof(text), "--");
            }
            return text;
        }

        void refreshLive(float) {
            for (auto& metric : m_metrics) {
                if (!metric.label) continue;
                std::string value = this->metricValue(metric.id);
                if (value != metric.last) {
                    metric.last = value;
                    metric.label->setString(value.c_str());
                }
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

    // -----------------------------------------------------------------------------
    // Entry point button
    //
    // Installed on the gameplay pause layer *and* the editor pause layer, because the
    // editor counters on the Editor page can only be read while the editor is the active
    // scene. Same badge, same position, same panel.
    // -----------------------------------------------------------------------------
    void openRebornPanel() {
        if (auto* popup = RebornPopup::create()) {
            popup->show();
        }
    }

    void installPanelButton(CCNode* layer, CCNode* target, SEL_MenuHandler selector) {
        float const width = 48.f;
        auto* holder = CCNode::create();
        holder->setContentSize({width, width});

        auto* bg = Pill::create(width, width, kPillIdle, 235);
        if (bg) {
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

        auto* button = CCMenuItemSpriteExtra::create(holder, target, selector);
        button->setID("reborn-button"_spr);

        auto* menu = CCMenu::create();
        menu->setID("reborn-menu"_spr);
        menu->addChild(button);
        menu->setPosition({0.f, 0.f});

        // Just past the native pause row (which ends around +158 design units from centre)
        // rather than in a corner: Eclipse's draggable menu button can occupy any corner, so
        // the row is the one place it does not cover.
        auto const win = CCDirector::get()->getWinSize();
        button->setPosition({win.width / 2.f + 205.f, win.height / 2.f - 11.f});
        menu->setTouchPriority(-600);
        layer->addChild(menu, 100);
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
        installPanelButton(this, this, menu_selector(RebornPauseLayer::onOpenReborn));
    }

    void onOpenReborn(CCObject*) {
        openRebornPanel();
    }
};

// The editor has its own pause layer, so the panel would otherwise be unreachable while the
// editor counters on the Editor page are the only ones worth looking at.
class $modify(RebornEditorPauseLayer, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();
        installPanelButton(this, this, menu_selector(RebornEditorPauseLayer::onOpenReborn));
    }

    void onOpenReborn(CCObject*) {
        openRebornPanel();
    }
};

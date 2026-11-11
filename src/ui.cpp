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

// Minimum Reborn panel. Design: DESIGN.md declares the system (palette roles, type,
// spacing, radii); this file implements it. Banner comments and ALL-CAPS section markers
// were removed in the rebuild; the reasons below are the ones a reader needs.
//
//   * Header: logo, sentence-case title, one sentence-case subtitle, one status pill with
//     the real profile. No caps micro-labels.
//   * Six tabs, equal measured columns, never clip; active tab keeps full opacity and an
//     accent underline.
//   * Profiles are one segmented control, not loose pills.
//   * Toggles are a custom checkbox component (accent fill + dark check on surface).
//   * Telemetry rows: label left, value right on a shared baseline, one hairline between
//     rows; every value comes from a real counter.
//   * Button roles: primary (accent fill) is Benchmark; secondary is surface fill; ghost is
//     transparent muted text. Every item press routes through CCMenuItemSpriteExtra, so the
//     game supplies the press animation; GD panels have no global focus model, so focus
//     rings are omitted rather than faked (Escape closes the panel instead).
//   * The popup reads and writes the same settings the native Geode page uses. It never
//     changes physics, input handling or the game's FPS cap.

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

    // Performance overlay. Numbers only; color carries no meaning except the "(OFF)" marker,
    // so the palette stays inside DESIGN.md's cap.
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
                std::snprintf(
                    text, sizeof(text),
                    "%s%u FPS%s | avg %.1f ms | p95 %.1f ms | 1%% low %s\n"
                    "logic %u/s | draw %.2f ms (worst %.2f ms) | %s | renderer %s\n"
                    "scene %u/%u/s | sprite fast-out %u/s | particles %u/%u/s | editor %u/s | capped %u/s | batch culls %llu\n"
                    "sims suppressed %llu | program binds skipped %llu | trace binds %llu",
                    cfg.enabled ? "" : "(OFF) ", fps,
                    counters.throttled.load(std::memory_order_relaxed) ? " (background)" : "",
                    avgMs, p95Ms, lowText,
                    logic, workMs, stats.worstWorkMs(240),
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
                        counters.particleSimsSuppressed.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        counters.programBindsSkipped.load(std::memory_order_relaxed)),
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

    // DESIGN.md tokens. The palette stays inside the 2-3 cores + 1 accent cap; semantic
    // green exists only for the "mod enabled" toggle state and appears nowhere else.
    ccColor3B const kSurface = ccc3(31, 35, 51);
    ccColor3B const kHairline = ccc3(43, 48, 70);
    ccColor3B const kText = ccc3(238, 241, 248);
    ccColor3B const kMuted = ccc3(166, 174, 199);
    ccColor3B const kAccent = ccc3(139, 92, 246);
    ccColor3B const kOnAccent = ccc3(18, 20, 28);
    ccColor3B const kGood = ccc3(52, 209, 123);

    constexpr float kPopupWidth = 680.f;
    constexpr float kPopupHeight = 400.f;
    constexpr float kMargin = 22.f;
    constexpr float kHeaderH = 52.f;
    constexpr float kNavH = 34.f;
    constexpr float kFooterH = 44.f;
    constexpr float kRowPitch = 40.f;
    constexpr float kButtonHeight = 30.f;
    constexpr float kIconSize = 30.f;
    constexpr float kGap = 8.f;
    constexpr float kContentWidth = kPopupWidth - 2.f * kMargin;
    constexpr float kTabWidth = kContentWidth / 6.f;

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

    // Flat rects from a mod-owned 16x16 solid sprite; GD's trimmed squares drag atlas
    // margins into every pill, so the mod owns the pixels it stretches.
    char const* const kRectSprite = "reborn-pixel.png"_spr;

    CCSprite* makeRect(float width, float height, ccColor3B color, std::uint8_t opacity) {
        auto* rect = CCSprite::create(kRectSprite);
        if (!rect) return nullptr;
        rect->setAnchorPoint({0.5f, 0.5f});
        CCSize const natural = rect->getContentSize();
        if (natural.width > 0.f && natural.height > 0.f) {
            rect->setScaleX(width / natural.width);
            rect->setScaleY(height / natural.height);
        }
        rect->setColor(color);
        rect->setOpacity(opacity);
        return rect;
    }

    // Checkbox component: 20x20 box, accent fill with dark check when on, surface fill when
    // off. The check is two rotated bars built from the same rect sprite, so no glyph set
    // dependency.
    struct Checkbox {
        CCMenuItemSpriteExtra* item = nullptr;
        CCSprite* box = nullptr;
        CCNode* check = nullptr;
        bool on = false;
    };

    void checkboxSet(Checkbox& cb, bool on, ccColor3B accent) {
        cb.on = on;
        if (cb.box) {
            cb.box->setColor(on ? accent : kSurface);
            cb.box->setOpacity(255);
        }
        if (cb.check) cb.check->setVisible(on);
    }

    enum class ButtonKind { Primary, Secondary, Ghost };

    struct ButtonParts {
        CCNode* holder = nullptr;
        CCSprite* bg = nullptr;
        CCLabelBMFont* label = nullptr;
    };

    ButtonParts makeButton(std::string const& text, ButtonKind kind, float width, float height) {
        ButtonParts parts;
        auto* holder = CCNode::create();
        holder->setContentSize({width, height});
        parts.holder = holder;

        if (kind != ButtonKind::Ghost) {
            if (auto* bg = makeRect(width, height,
                                    kind == ButtonKind::Primary ? kAccent : kSurface, 255)) {
                bg->setPosition({width / 2.f, height / 2.f});
                holder->addChild(bg);
                parts.bg = bg;
            }
        }
        if (auto* label = makeLabel(text, "chatFont.fnt", 0.26f)) {
            label->setColor(kind == ButtonKind::Primary ? kOnAccent : kText);
            if (kind == ButtonKind::Ghost) label->setColor(kMuted);
            fitLabel(label, 0.26f, width - 16.f);
            label->setPosition({width / 2.f, height / 2.f});
            holder->addChild(label);
            parts.label = label;
        }
        return parts;
    }

    // One panel at a time. Defined before the class because init() registers itself
    // and onExit() clears the entry.
    class RebornPopup;
    RebornPopup* g_openPanel = nullptr;

    class RebornPopup : public Popup, public CCKeyboardDelegate {
    protected:
        enum Page : int {
            Overview = 0, Render, Culling, Input, Editor, Diagnostics, PageCount
        };

        struct ToggleRef {
            CCMenuItemSpriteExtra* item = nullptr;
            std::string key;
            Checkbox box;             // component state (pointers into the live scene graph)
            ccColor3B accent = kAccent;
        };

        struct ButtonRef {
            CCMenuItemSpriteExtra* item = nullptr;
            CCLabelBMFont* label = nullptr;
            CCSprite* bg = nullptr;
            ButtonKind kind = ButtonKind::Secondary;
        };

        struct Segment {
            CCMenuItemSpriteExtra* item = nullptr;
            CCSprite* bg = nullptr;
            CCLabelBMFont* label = nullptr;
            std::string name;
        };

        enum class CycleKind {
            Renderer, Cull, EditorCull, Detail, Corner, Priority, ParticleCap, SpawnBudget
        };

        struct CycleRef {
            CCLabelBMFont* label = nullptr;
            CycleKind kind = CycleKind::Renderer;
            std::string key;
            std::string prefix;
            float width = 220.f;
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
        std::vector<Segment> m_segments;
        std::vector<CCLabelBMFont*> m_tabLabels;
        std::vector<MetricRef> m_metrics;
        CCLabelBMFont* m_headerProfile = nullptr;
        CCLabelBMFont* m_benchLabel = nullptr;

        static constexpr char const* kPageNames[] = {
            "Overview", "Render", "Culling", "Input", "Editor", "Diagnostics"
        };

        static constexpr char const* kProfileNames[] = {
            "Custom", "Default", "Balanced", "Performance", "Extreme"
        };
        bool init() override {
            // GD's own popup frame keeps the panel in the game's visual language; Geode's
            // default brown frame reads as a different app.
            if (!Popup::init(kPopupWidth, kPopupHeight, "square01_001.png")) return false;
            this->setID("minimum-reborn-popup"_spr);

            // Only the content layer is scaled down on small windows; scaling the frame
            // would shrink the dimmer too and leave bright bands.
            auto* director = CCDirector::get();
            if (director && m_mainLayer) {
                CCSize const visible = director->getVisibleSize();
                float const fitW = visible.width > 1.f ? (visible.width - 24.f) / kPopupWidth : 1.f;
                float const fitH = visible.height > 1.f ? (visible.height - 24.f) / kPopupHeight : 1.f;
                float const fit = std::min({1.f, fitW, fitH});
                if (fit > 0.01f && fit < 1.f) {
                    m_mainLayer->setScale(fit);
                }
            }

            // Escape closes. The game's panels have no global focus chain, so focus rings
            // are omitted rather than faked; Tab order is not implemented for the same reason.
            if (auto* dispatcher = CCKeyboardDispatcher::sharedDispatcher()) {
                dispatcher->addDelegate(this);
            }

            this->buildHeader();
            this->buildNav();
            this->buildFooter();
            this->buildPage();
            this->schedule(schedule_selector(RebornPopup::refreshLive), 0.25f);
            this->refreshLive(0.f);
            g_openPanel = this;
            return true;
        }

        static constexpr float kLeft = -kPopupWidth / 2.f + kMargin;
        static constexpr float kRight = kPopupWidth / 2.f - kMargin;
        static constexpr float kHeaderCenterY = kPopupHeight / 2.f - kHeaderH / 2.f;
        static constexpr float kNavCenterY = kPopupHeight / 2.f - kHeaderH - kNavH / 2.f;
        static constexpr float kFooterY = -kPopupHeight / 2.f + kFooterH / 2.f - 2.f;
        static constexpr float kRowTop = kPopupHeight / 2.f - kHeaderH - kNavH - kRowPitch / 2.f;

        static constexpr float kCX = kPopupWidth / 2.f;
        static constexpr float kCY = kPopupHeight / 2.f;

        static float rowY(int row) {
            return kRowTop - static_cast<float>(row) * kRowPitch;
        }

        static float tabCenter(int index) {
            return kLeft + kTabWidth * (static_cast<float>(index) + 0.5f);
        }

        void keyDown(enumKeyCodes key, double) override {
            if (key == KEY_Escape) this->closePopup(nullptr);
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

            if (auto* icon = CCSprite::create("icon-button.png"_spr)) {
                float const h = icon->getContentSize().height;
                icon->setScale(h > 0.f ? kIconSize / h : 1.f);
                icon->setPosition({kCX + kLeft + kIconSize / 2.f, kCY + kHeaderCenterY});
                root->addChild(icon);
            }

            float const textX = kLeft + kIconSize + 10.f;
            addLabel(root, "Minimum Reborn", "bigFont.fnt", 0.30f, kText, 255,
                     textX, kHeaderCenterY + 7.f, 1);
            addLabel(root, "Performance first.", "chatFont.fnt", 0.24f, kMuted, 255,
                     textX, kHeaderCenterY - 9.f, 1);

            ButtonParts pill = makeButton("Balanced", ButtonKind::Secondary, 112.f, 24.f);
            if (pill.holder) {
                pill.holder->setPosition({kCX + kRight - 56.f, kCY + kHeaderCenterY});
                root->addChild(pill.holder);
                m_headerProfile = pill.label;
            }
        }

        void buildNav() {
            m_navRoot = CCNode::create();
            m_mainLayer->addChild(m_navRoot);

            if (auto* base = makeRect(kContentWidth, 1.f, kHairline, 255)) {
                base->setPosition({kCX, kCY + kNavCenterY - kNavH / 2.f});
                m_navRoot->addChild(base);
            }
            m_tabAccent = makeRect(kTabWidth - 24.f, 2.f, kAccent, 255);
            if (m_tabAccent) {
                m_tabAccent->setPosition({kCX, kCY});
                m_navRoot->addChild(m_tabAccent);
            }

            m_tabMenu = CCMenu::create();
            m_tabMenu->setPosition({kCX, kCY});
            m_navRoot->addChild(m_tabMenu, 2);

            for (int i = 0; i < static_cast<int>(PageCount); ++i) {
                auto* holder = CCNode::create();
                holder->setContentSize({kTabWidth - 4.f, kNavH - 6.f});
                auto* label = makeLabel(kPageNames[i], "bigFont.fnt", 0.27f, 200, false);
                if (label) {
                    label->setColor(kMuted);
                    fitLabel(label, 0.27f, kTabWidth - 12.f);
                    label->setPosition({(kTabWidth - 4.f) / 2.f, (kNavH - 6.f) / 2.f});
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

            this->footerButton("Reset", kLeft + 52.f, 104.f, ButtonKind::Ghost,
                               menu_selector(RebornPopup::onReset));
            this->footerButton("All Settings", 0.f, 150.f, ButtonKind::Secondary,
                               menu_selector(RebornPopup::onAllSettings));
            this->footerButton("Close", kRight - 52.f, 104.f, ButtonKind::Secondary,
                               menu_selector(RebornPopup::closePopup));
        }

        void footerButton(char const* text, float x, float width, ButtonKind kind,
                          SEL_MenuHandler selector) {
            ButtonParts parts = makeButton(text, kind, width, kButtonHeight);
            auto* item = CCMenuItemSpriteExtra::create(parts.holder, this, selector);
            m_footerMenu->addChild(item);
            item->setPosition({x, kFooterY});
        }

        void onTab(CCObject* sender) {
            int const page = static_cast<CCNode*>(sender)->getTag();
            if (page < 0 || page >= static_cast<int>(PageCount) || page == static_cast<int>(m_page)) return;
            m_page = static_cast<Page>(page);
            this->refreshTabs();
            // The tab menu rebuilds the page tree in applyTab; swapping it out from inside
            // the dispatch callback would free the sender while CCMenu still runs.
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
                label->setColor(selected ? kText : kMuted);
                label->setOpacity(selected ? 255 : 190);
                fitLabel(label, 0.27f, kTabWidth - 12.f);
            }
            if (m_tabAccent) {
                m_tabAccent->setPosition({kCX + tabCenter(static_cast<int>(m_page)),
                                          kCY + kNavCenterY - kNavH / 2.f + 1.5f});
            }
        }
        void buildPage() {
            if (m_pageRoot) {
                m_pageRoot->removeFromParentAndCleanup(true);
                m_pageRoot = nullptr;
            }
            m_pageMenu = nullptr;
            m_actions.clear();
            m_cycles.clear();
            m_toggles.clear();
            m_segments.clear();
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

        void noteRow(int row, std::string const& text, float scale = 0.24f, std::uint8_t opacity = 200) {
            addLabel(m_pageRoot, text, "chatFont.fnt", scale, kMuted, opacity,
                     kLeft + 2.f, rowY(row), 1);
        }

        void ledger(int row) {
            // One hairline under a telemetry row; the rule the eye follows across the row.
            if (auto* line = makeRect(kContentWidth, 1.f, kHairline, 120)) {
                line->setPosition({kCX, kCY + (rowY(row) - kRowPitch / 2.f + 3.f)});
                m_pageRoot->addChild(line);
            }
        }

        void statRow(int row, char const* label, char const* metric) {
            addLabel(m_pageRoot, label, "chatFont.fnt", 0.26f, kMuted, 255,
                     kLeft + 2.f, rowY(row), 1);
            auto* value = addLabel(m_pageRoot, "--", "chatFont.fnt", 0.28f, kText, 255,
                                   kRight - 2.f, rowY(row), 2);
            if (value) m_metrics.push_back({metric, value, {}});
            this->ledger(row);
        }

        // One checkbox per row. labelX shifts the second column when two share a row.
        void toggleRow(int row, std::string key, char const* text, float labelX = kLeft + 30.f,
                       ccColor3B accent = kAccent) {
            float const boxX = labelX - 15.f;
            auto* holder = CCNode::create();
            holder->setContentSize({22.f, 22.f});

            auto* box = makeRect(22.f, 22.f, kSurface, 255);
            if (box) {
                box->setPosition({11.f, 11.f});
                holder->addChild(box);
            }

            auto* check = CCNode::create();
            check->setPosition({11.f, 11.f});
            CCSprite* bar1 = makeRect(10.f, 3.f, kOnAccent, 255);
            CCSprite* bar2 = makeRect(15.f, 3.f, kOnAccent, 255);
            if (bar1 && bar2) {
                bar1->setRotation(-42.f);
                bar2->setRotation(45.f);
                bar1->setPosition({-3.4f, -2.1f});
                bar2->setPosition({1.3f, 0.6f});
                check->addChild(bar1);
                check->addChild(bar2);
            }
            check->setVisible(false);
            holder->addChild(check);

            auto* item = CCMenuItemSpriteExtra::create(
                holder, this, menu_selector(RebornPopup::onToggle)
            );
            if (!item) return;
            item->setTag(static_cast<int>(m_toggles.size()));
            m_pageMenu->addChild(item);
            item->setPosition({boxX, rowY(row)});

            Checkbox state{item, box, check, false};
            m_toggles.push_back({item, std::move(key), state, accent});

            addLabel(m_pageRoot, text, "bigFont.fnt", 0.27f, kText, 235,
                     labelX, rowY(row), 1);
        }

        ButtonRef pillAt(
            float x, float y, float width, std::string const& text,
            std::function<void()> action, ButtonKind kind = ButtonKind::Secondary
        ) {
            ButtonRef ref;
            ButtonParts parts = makeButton(text, kind, width, kButtonHeight);
            ref.bg = parts.bg;
            ref.label = parts.label;
            auto* item = CCMenuItemSpriteExtra::create(
                parts.holder, this, menu_selector(RebornPopup::onAction)
            );
            item->setTag(static_cast<int>(m_actions.size()));
            m_actions.push_back(std::move(action));
            if (m_pageMenu) {
                m_pageMenu->addChild(item);
                item->setPosition({x, y});
            }
            ref.item = item;
            return ref;
        }

        void cycleRow(
            int row, char const* prefix, CycleKind kind, std::string key,
            float width = kContentWidth, float x = 0.f
        ) {
            auto ref = this->pillAt(x, rowY(row), width, " ",
                                    std::function<void()>(), ButtonKind::Secondary);
            m_cycles.push_back({ref.label, kind, std::move(key), prefix, width});
            // The key was moved into the row above, so the action reads it back from there.
            m_actions.back() = this->makeCycleAction(kind, m_cycles.back().key);
        }

        void buildProfileRow(int row) {
            float const segW = kContentWidth / 5.f;
            for (int i = 0; i < 5; ++i) {
                std::string name = kProfileNames[i];
                auto* holder = CCNode::create();
                holder->setContentSize({segW, 30.f});

                auto* bg = makeRect(segW, 30.f, kSurface, 255);
                if (bg) {
                    bg->setPosition({segW / 2.f, 15.f});
                    holder->addChild(bg);
                }
                auto* label = makeLabel(name, "bigFont.fnt", 0.25f, 255, false);
                if (label) {
                    label->setColor(kText);
                    fitLabel(label, 0.25f, segW - 12.f);
                    label->setPosition({segW / 2.f, 15.f});
                    holder->addChild(label);
                }
                auto* item = CCMenuItemSpriteExtra::create(
                    holder, this, menu_selector(RebornPopup::onSegment)
                );
                item->setTag(i);
                m_pageMenu->addChild(item);
                item->setPosition({kLeft + segW * (static_cast<float>(i) + 0.5f), rowY(row)});
                m_segments.push_back({item, bg, label, std::move(name)});
            }
        }
        void buildOverview() {
            statRow(0, "Frame", "frame");
            statRow(1, "Pipeline", "pipeline");
            this->buildProfileRow(2);

            this->toggleRow(3, "enabled", "Mod enabled", kLeft + 30.f, kGood);
            this->toggleRow(4, "show-overlay", "Performance overlay");

            float const half = (kContentWidth - kGap) / 2.f;
            auto bench = this->pillAt(kLeft + half / 2.f, rowY(5), half, "Benchmark",
                [this]() { this->toggleBenchmark(); }, ButtonKind::Primary);
            m_benchLabel = bench.label;
            this->pillAt(kRight - half / 2.f, rowY(5), half, "Geode Settings",
                [this]() { this->onAllSettings(nullptr); }, ButtonKind::Secondary);
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
            this->toggleRow(5, "render-gate", "Render gate", kRight - half + 30.f);
#endif
            statRow(6, "Work avoided", "avoided");
        }

        void buildCulling() {
            cycleRow(0, "Particles", CycleKind::Cull, "particle-cull-mode");
            cycleRow(1, "Scene sprites", CycleKind::Cull, "scene-cull");
            cycleRow(2, "Spawn budget", CycleKind::SpawnBudget, "spawn-budget", 220.f, 60.f);

            float const half = (kContentWidth - kGap) / 2.f;
            this->toggleRow(3, "cull-offscreen-particles", "Off-screen particles");
            this->toggleRow(3, "batch-culling", "Batch culling", kRight - half + 30.f);
            statRow(4, "Sprites and particles", "cull-rate");
            statRow(5, "Batches (session)", "batches");
            noteRow(6, "Draw-only. Simulation, physics and triggers are untouched.");
        }

        void buildInput() {
            this->toggleRow(0, "no-input-delay", "No Additional Input Delay");
#ifdef GEODE_IS_WINDOWS
            this->toggleRow(1, "low-latency", "Low Latency (drain GPU queue)");
#endif
            statRow(2, "Frame order", "input");

#ifdef GEODE_IS_WINDOWS
            this->toggleRow(3, "timer-resolution", "1 ms timer resolution");
            this->toggleRow(4, "background-throttle", "Throttle drawing in background");
            cycleRow(5, "Priority", CycleKind::Priority, "process-priority");
#endif
            noteRow(6, "Neither switch touches physics, input or the FPS cap. "
                       "Low Latency costs frame rate to shorten the queue.");
        }

        void buildEditor() {
            this->toggleRow(0, "editor-culling", "Editor visibility and particle culling");
            cycleRow(1, "Editor mode", CycleKind::EditorCull, "editor-cull-mode");
            statRow(2, "State", "editor-state");
            statRow(3, "Sprite draws", "editor-sprites");
            statRow(4, "Particle draws", "editor-particles");
            noteRow(5, "Draw-only. Object data, selection, triggers and saving are untouched.");
            noteRow(6, "Open the editor to populate these counters.", 0.24f, 150);
        }

        void buildDiagnostics() {
            this->toggleRow(0, "show-overlay", "Performance overlay");
            cycleRow(1, "Overlay detail", CycleKind::Detail, "overlay-detail");
            cycleRow(2, "Overlay corner", CycleKind::Corner, "overlay-corner");
            statRow(3, "Last benchmark", "benchmark");
            statRow(4, "Frame", "diag-frame");
            statRow(5, "Spikes and draw cost", "diag-spikes");
            auto bench = this->pillAt(0.f, rowY(6), 200.f, "Benchmark",
                [this]() { this->toggleBenchmark(); }, ButtonKind::Primary);
            m_benchLabel = bench.label;
        }

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
                else if (kind == CycleKind::SpawnBudget) {
                    int64_t const budget = mod->getSettingValue<int64_t>("spawn-budget");
                    int64_t const next = budget == 0 ? 8 : budget == 8 ? 16 : budget == 16 ? 32 : 0;
                    mod->setSettingValue<int64_t>("spawn-budget", next);
                }
                else if (kind == CycleKind::ParticleCap) {
                    int64_t const cap = mod->getSettingValue<int64_t>("particle-cap");
                    int64_t const next = cap == 256 ? 128 : cap == 128 ? 64 : cap == 64 ? 32 : 256;
                    mod->setSettingValue<int64_t>("particle-cap", next);
                }
                if (kind != CycleKind::Detail && kind != CycleKind::Corner) {
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

        void onSegment(CCObject* sender) {
            int const index = static_cast<CCNode*>(sender)->getTag();
            if (index < 0 || index > 4) return;  // five segments, 0..4
            Mod::get()->setSettingValue<std::string>("profile", kProfileNames[index]);
            reborn::refreshConfig();
            this->refreshPage();
        }

        void onAction(CCObject* sender) {
            int const index = static_cast<CCNode*>(sender)->getTag();
            if (index >= 0 && index < static_cast<int>(m_actions.size())) {
                m_actions[static_cast<std::size_t>(index)]();
            }
        }

        void onToggle(CCObject* sender) {
            auto* toggler = static_cast<CCMenuItemSpriteExtra*>(sender);
            int const index = toggler ? toggler->getTag() : -1;
            if (index < 0 || index >= static_cast<int>(m_toggles.size())) return;
            auto& toggle = m_toggles[static_cast<std::size_t>(index)];
            auto* mod = Mod::get();
            bool const next = !mod->getSettingValue<bool>(toggle.key);
            // Toggles that no profile owns must not silently rewrite the profile to Custom.
            static char const* const kProfileNeutral[] = {
                "show-overlay", "no-input-delay", "editor-culling", "timer-resolution",
                "adaptive-cap", "low-latency", "enabled", "render-gate",
                "background-throttle", "cull-offscreen-particles",
                "shader-state-cache", "particle-suppression", "shader-trace",
            };
            bool neutral = false;
            for (auto const* name : kProfileNeutral) {
                if (toggle.key == name) {
                    neutral = true;
                    break;
                }
            }
            if (!neutral) {
                this->markCustom();
            }
            mod->setSettingValue<bool>(toggle.key, next);
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
            this->retain();
            createQuickPopup(
                "Reset Minimum Reborn",
                "Restore every setting to the <cy>Balanced</c> defaults?\n"
                "The current profile and all switches will be replaced.",
                "Cancel", "Reset",
                // Hold a reference for the popup's lifetime so a panel closed while the
                // confirmation is up cannot be called through a dangling pointer.
                [this](FLAlertLayer*, bool confirmed) {
                    if (confirmed) {
                        reborn::resetToDefaults();
                        this->refreshPage();
                    }
                    this->release();
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

        void refreshPage() {
            auto* mod = Mod::get();
            std::string const profile = mod->getSettingValue<std::string>("profile");
            if (m_headerProfile) m_headerProfile->setString(profile.c_str());

            for (auto& toggle : m_toggles) {
                bool const on = mod->getSettingValue<bool>(toggle.key);
                checkboxSet(toggle.box, on, toggle.accent);
            }

            for (auto& c : m_cycles) {
                if (!c.label) continue;
                std::string text = c.prefix + ": " + this->cycleValue(c, mod);
                c.label->setString(text.c_str());
                fitLabel(c.label, 0.26f, c.width - 16.f);
            }

            for (auto& segment : m_segments) {
                bool const selected = segment.name == profile;
                if (segment.bg) segment.bg->setColor(selected ? kAccent : kSurface);
                if (segment.label) segment.label->setColor(selected ? kOnAccent : kText);
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
                case CycleKind::SpawnBudget: {
                    int64_t const budget = mod->getSettingValue<int64_t>("spawn-budget");
                    return budget == 0 ? std::string("Off") : std::to_string(budget);
                }
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
                std::snprintf(text, sizeof(text), "%u logic/s   %s renderer   %s%s", logic,
                              cfg.rendererOptimized ? "optimized" : "engine",
                              cfg.noInputDelay ? "game first" : "counters first",
                              cfg.lowLatency ? "   low latency" : "");
            }
            else if (id == "avoided") {
                std::snprintf(text, sizeof(text), "%llu sprite   %llu particle   %llu batch",
                              u64(counters.invisibleSpriteDrawsSkipped)
                                  + u64(counters.sceneSpriteDrawsSkipped),
                              u64(counters.idleParticleDrawsSkipped)
                                  + u64(counters.offscreenParticleDrawsSkipped)
                                  + u64(counters.invisibleParticleDrawsSkipped),
                              u64(counters.emptyBatchDrawsSkipped)
                                  + u64(counters.batchCulls));
            }
            else if (id == "cull-rate") {
                std::snprintf(text, sizeof(text), "sprites %u/%u   particles %u/%u per s",
                              counters.sceneCulledPerSec.load(std::memory_order_relaxed),
                              counters.sceneExaminedPerSec.load(std::memory_order_relaxed),
                              counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                              counters.particleExaminedPerSec.load(std::memory_order_relaxed));
            }
            else if (id == "particles") {
                std::snprintf(text, sizeof(text), "%u skipped / %u tested",
                              counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                              counters.particleExaminedPerSec.load(std::memory_order_relaxed));
            }
            else if (id == "batches") {
                std::snprintf(text, sizeof(text), "%llu empty / %llu culled / %llu drawn",
                              u64(counters.emptyBatchDrawsSkipped),
                              u64(counters.batchCulls),
                              u64(counters.batchDraws));
            }
            else if (id == "input") {
                std::snprintf(text, sizeof(text), "%llu deferred / %llu scheduler / %llu drains",
                              u64(counters.deferredBookkeeping), u64(counters.schedulerUpdates),
                              u64(counters.lowLatencyDrains));
            }
            else if (id == "editor-state") {
                std::snprintf(text, sizeof(text), "%s   %s",
                              reborn::inEditor() ? "editor active" : "not in the editor",
                              cfg.editorCulling ? cullName(cfg.editorCull) : "Off");
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
            else if (id == "benchmark") {
                char const* summary = reborn::benchmarkSummary();
                return summary && summary[0] ? std::string(summary) : std::string("no run yet");
            }
            else if (id == "diag-frame") {
                std::snprintf(text, sizeof(text), "%u FPS   avg %.1f   p95 %.1f   worst %.1f ms",
                              fps, stats.averageFrameMs(120), stats.percentileFrameMs(120, 0.95),
                              stats.worstFrameMs(240));
            }
            else if (id == "diag-spikes") {
                // A spike is only attributable to the draw path if drawScene itself got long.
                std::snprintf(text, sizeof(text), "%zu over 25 ms   draw avg %.2f   worst %.2f ms",
                              stats.spikeCount(240, 25.0), stats.averageWorkMs(120),
                              stats.worstWorkMs(240));
            }
            else {
                std::snprintf(text, sizeof(text), "--");
            }
            return text;
        }

        void onExit() override {
            if (g_openPanel == this) g_openPanel = nullptr;
            if (auto* dispatcher = CCKeyboardDispatcher::sharedDispatcher()) {
                dispatcher->removeDelegate(this);
            }
            Popup::onExit();
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

void openRebornPanel() {
    if (g_openPanel) return;
    if (auto* popup = RebornPopup::create()) {
        popup->show();
    }
}

// Installed on the gameplay pause layer. The editor pause layer entry was reverted in
// v1.4.2 (the menu crashed on its own), so the Editor page counters are read from a level
// pause menu, not inside the editor.
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

    // Just past the native pause row, clamped back inside the visible rect on narrow windows.
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
        openRebornPanel();
    }
};

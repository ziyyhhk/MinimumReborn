#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "config.hpp"
#include "frame_stats.hpp"
#include "hud.hpp"

using namespace geode::prelude;

// User interface
//
//   * Performance overlay: one CCLabelBMFont in Geode's OverlayManager, rebuilt at most
//     four times a second and only when the text actually changed. It creates no nodes per
//     frame and does not touch the input path.
//   * Pause menu button + popup: live numbers, profiles, the quick toggles that correspond
//     to features that exist, reset, and a link to the full native Geode settings page.
//
// The popup deliberately uses plain Geode/cocos controls (toggles, scale-9 panels, BMFont
// labels) so it looks like part of the game. No emoji, no animations beyond the popup's own
// show animation, no per-frame rebuilding.

namespace {

    char const* bottleneckLabel(reborn::Bottleneck hint) {
        switch (hint) {
            case reborn::Bottleneck::DrawBound: return "draw path busy";
            case reborn::Bottleneck::GpuOrOther: return "GPU or wait";
            default: return "not measured";
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

            char text[512];
            if (cfg.overlayDetailed) {
                char lowText[32];
                if (lowFps > 0.5) std::snprintf(lowText, sizeof(lowText), "%.0f", lowFps);
                else std::snprintf(lowText, sizeof(lowText), "--");
                std::snprintf(
                    text, sizeof(text),
                    "%s%u FPS%s | avg %.1f ms | p95 %.1f ms | 1%% low %s\n"
                    "logic %u/s | draw %.2f ms | %s\n"
                    "sprite skips %u/s | particle skips %u/s | pools capped %u/s | logic-only %u/s",
                    cfg.enabled ? "" : "(OFF) ", fps,
                    counters.throttled.load(std::memory_order_relaxed) ? " (background)" : "",
                    avgMs, p95Ms, lowText,
                    logic, workMs,
                    bottleneckLabel(static_cast<reborn::Bottleneck>(counters.bottleneck.load(std::memory_order_relaxed))),
                    counters.spriteSkipsPerSec.load(std::memory_order_relaxed),
                    counters.particleSkipsPerSec.load(std::memory_order_relaxed),
                    counters.cappedPerSec.load(std::memory_order_relaxed),
                    counters.logicOnlyPerSec.load(std::memory_order_relaxed)
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

            // Green when it holds the target, yellow close, red otherwise.
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
    // Popup
    // ---------------------------------------------------------------------------------
    constexpr float kPopupWidth = 380.f;
    constexpr float kPopupHeight = 300.f;

    CCScale9Sprite* makePanel(float width, float height, std::uint8_t opacity) {
        auto* panel = CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
        if (!panel) return nullptr;
        panel->setColor(ccc3(255, 255, 255));
        panel->setOpacity(opacity);
        panel->setContentSize({width, height});
        return panel;
    }

    void fitLabel(CCLabelBMFont* label, float maxScale, float room) {
        float const natural = label->getContentSize().width;
        label->setScale(natural > 0.f ? std::min(maxScale, room / natural) : maxScale);
    }

    class RebornPopup : public Popup {
    protected:
        struct ToggleDef {
            char const* key;
            char const* label;
        };

        std::vector<ToggleDef> m_toggles;
        std::vector<CCMenuItemToggler*> m_togglers;
        std::vector<std::pair<CCScale9Sprite*, char const*>> m_profileButtons;
        CCLabelBMFont* mLive = nullptr;
        std::string mLastLive;

        static constexpr char const* kProfileNames[] = {
            "Custom", "Default", "Balanced", "Performance", "Extreme"
        };

        bool init() {
            if (!Popup::init(kPopupWidth, kPopupHeight, "square01_001.png")) return false;
            this->setTitle("Minimum Reborn");

            m_toggles = {
                {"enabled", "Mod enabled"},
                {"sprite-fast-path", "Sprite draw fast-out"},
                {"skip-idle-particles", "Skip idle particle draws"},
                {"cap-particles", "Cap particle pool size"},
                {"show-overlay", "Performance overlay"},
            };
#ifdef GEODE_IS_WINDOWS
            m_toggles.push_back({"render-gate", "Render gate (experimental)"});
#endif

            this->buildLive();
            this->buildProfiles();
            this->buildToggles();
            this->buildFooter();
            this->refreshLive(0.f);
            this->schedule(schedule_selector(RebornPopup::refreshLive), 0.25f);

            this->syncProfiles();
            this->syncTogglers();
            return true;
        }

        void buildLive() {
            auto* panel = makePanel(kPopupWidth - 28.f, 46.f, 44);
            if (panel) {
                m_buttonMenu->addChildAtPosition(panel, Anchor::Center, ccp(0.f, 110.f));
            }
            mLive = CCLabelBMFont::create("--", "chatFont.fnt");
            mLive->setAlignment(kCCTextAlignmentCenter);
            mLive->setScale(0.8f);
            m_buttonMenu->addChildAtPosition(mLive, Anchor::Center, ccp(0.f, 110.f));
        }

        void buildProfiles() {
            float const width = 66.f;
            float const height = 26.f;
            float const spacing = 70.f;
            float const startX = -2.f * spacing;

            auto* caption = CCLabelBMFont::create("Profile", "goldFont.fnt");
            caption->setScale(0.5f);
            m_buttonMenu->addChildAtPosition(caption, Anchor::Center, ccp(0.f, 80.f));

            for (std::size_t i = 0; i < 5; ++i) {
                auto* holder = CCNode::create();
                holder->setContentSize({width, height});

                auto* background = makePanel(width, height, 26);
                if (background) {
                    background->setPosition({width / 2.f, height / 2.f});
                    holder->addChild(background);
                }
                auto* label = CCLabelBMFont::create(kProfileNames[i], "bigFont.fnt");
                fitLabel(label, 0.36f, width - 8.f);
                label->setPosition({width / 2.f, height / 2.f + 1.f});
                holder->addChild(label);

                auto* button = CCMenuItemSpriteExtra::create(holder, this, menu_selector(RebornPopup::onProfile));
                button->setTag(static_cast<int>(i));
                m_buttonMenu->addChildAtPosition(
                    button, Anchor::Center, ccp(startX + static_cast<float>(i) * spacing, 58.f)
                );
                m_profileButtons.push_back({background, kProfileNames[i]});
            }
        }

        void buildToggles() {
            float y = 34.f;
            for (std::size_t i = 0; i < m_toggles.size(); ++i) {
                auto* panel = makePanel(kPopupWidth - 44.f, 24.f, 20);
                if (panel) {
                    m_buttonMenu->addChildAtPosition(panel, Anchor::Center, ccp(0.f, y));
                }

                auto* toggler = CCMenuItemToggler::createWithStandardSprites(
                    this, menu_selector(RebornPopup::onToggle), 0.55f
                );
                toggler->setTag(static_cast<int>(i));
                m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, ccp(-kPopupWidth / 2.f + 36.f, y));
                m_togglers.push_back(toggler);

                auto* label = CCLabelBMFont::create(m_toggles[i].label, "bigFont.fnt");
                fitLabel(label, 0.42f, kPopupWidth - 110.f);
                m_buttonMenu->addChildAtPosition(
                    label, Anchor::Center, ccp(-kPopupWidth / 2.f + 58.f, y), ccp(0.f, 0.5f)
                );

                y -= 25.f;
            }
        }

        void buildFooter() {
            float const buttonY = -112.f;

            {
                auto* sprite = ButtonSprite::create("Reset", "goldFont.fnt", "GJ_button_05.png", 0.7f);
                sprite->setScale(0.8f);
                auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RebornPopup::onReset));
                m_buttonMenu->addChildAtPosition(button, Anchor::Center, ccp(-96.f, buttonY));
            }
            {
                auto* sprite = ButtonSprite::create("All Settings", "goldFont.fnt", "GJ_button_01.png", 0.7f);
                sprite->setScale(0.8f);
                auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RebornPopup::onAllSettings));
                m_buttonMenu->addChildAtPosition(button, Anchor::Center, ccp(96.f, buttonY));
            }
            {
                auto* info = CCLabelBMFont::create(
                    "Visual only: physics, input timing and the FPS cap are never changed.",
                    "chatFont.fnt"
                );
                fitLabel(info, 0.62f, kPopupWidth - 40.f);
                info->setOpacity(185);
                m_buttonMenu->addChildAtPosition(info, Anchor::Center, ccp(0.f, -132.f));
            }
        }

        void refreshLive(float) {
            if (!mLive) return;
            auto& counters = reborn::counters();
            auto& stats = reborn::frameStats();
            char text[192];
            std::snprintf(
                text, sizeof(text), "%u FPS  |  avg %.1f ms  |  1%% low %.0f  |  logic %u/s",
                counters.fps.load(std::memory_order_relaxed),
                stats.averageFrameMs(120),
                stats.onePercentLowFps(240),
                counters.logicFps.load(std::memory_order_relaxed)
            );
            if (mLastLive != text) {
                mLastLive = text;
                mLive->setString(text);
            }
        }

        void syncProfiles() {
            auto const current = Mod::get()->getSettingValue<std::string>("profile");
            for (auto const& entry : m_profileButtons) {
                if (!entry.first) continue;
                bool const selected = current == entry.second;
                entry.first->setColor(selected ? ccc3(255, 200, 60) : ccc3(255, 255, 255));
                entry.first->setOpacity(selected ? 150 : 26);
            }
        }

        void syncTogglers() {
            for (std::size_t i = 0; i < m_togglers.size(); ++i) {
                m_togglers[i]->toggle(Mod::get()->getSettingValue<bool>(m_toggles[i].key));
            }
        }

        void onProfile(CCObject* sender) {
            int const index = static_cast<CCNode*>(sender)->getTag();
            if (index < 0 || index >= 5) return;
            Mod::get()->setSettingValue<std::string>("profile", kProfileNames[index]);
            reborn::refreshConfig();
            this->syncProfiles();
            this->syncTogglers();
        }

        void onToggle(CCObject* sender) {
            auto* toggler = static_cast<CCMenuItemToggler*>(sender);
            std::size_t const index = static_cast<std::size_t>(toggler->getTag());
            if (index >= m_toggles.size()) return;

            // The named profile owns these settings; step out of it first so the click sticks.
            if (Mod::get()->getSettingValue<std::string>("profile") != "Custom") {
                Mod::get()->setSettingValue<std::string>("profile", "Custom");
            }
            auto* mod = Mod::get();
            bool const next = !mod->getSettingValue<bool>(m_toggles[index].key);
            mod->setSettingValue<bool>(m_toggles[index].key, next);
            reborn::refreshConfig();
            this->syncProfiles();
            this->syncTogglers();
        }

        void onReset(CCObject*) {
            reborn::resetToDefaults();
            this->syncProfiles();
            this->syncTogglers();
        }

        void onAllSettings(CCObject*) {
            this->onClose(nullptr);
            openSettingsPopup(Mod::get(), true);
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
        if (g_hudCreated || totalFrames < 300 || !config().showOverlay) return;
        auto* overlay = OverlayManager::get();
        if (!overlay) return;
        if (auto* hud = StatsHud::create()) {
            overlay->addChild(hud, 99999);
            g_hudCreated = true;
        }
    }
}

class $modify(RebornPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto* sprite = ButtonSprite::create("MR", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        auto* button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(RebornPauseLayer::onOpenReborn)
        );
        button->setID("reborn-button"_spr);

        auto* menu = CCMenu::create();
        menu->setID("reborn-menu"_spr);
        menu->addChild(button);
        menu->setPosition({0.f, 0.f});

        auto const win = CCDirector::get()->getWinSize();
        button->setPosition({win.width - 34.f, win.height - 34.f});
        // Checked before the pause menu's own menus so the button stays clickable when another
        // mod's button lands on the same spot.
        menu->setTouchPriority(-600);
        this->addChild(menu, 100);
    }

    void onOpenReborn(CCObject*) {
        if (auto* popup = RebornPopup::create()) {
            popup->show();
        }
    }
};

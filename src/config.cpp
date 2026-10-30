#include <Geode/Geode.hpp>
#include <algorithm>
#include <cstring>
#include "config.hpp"

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// Settings snapshot.
//
// The keys below must match mod.json exactly. Types follow Geode's mapping:
//   bool  -> getSettingValue<bool>
//   int   -> getSettingValue<int64_t>
//   float -> getSettingValue<float>
//   string-> getSettingValue<std::string>
//
// Profiles are write-through: selecting a named profile writes the profile's values into
// the individual settings once (so the setting store, the native settings page and the
// running config all agree), then fills the snapshot. Selecting Custom changes nothing.
// Editing any individual setting from the popup switches the profile to Custom first.

namespace reborn {

    namespace {

        Config g_config;
        Counters g_counters;
        std::chrono::steady_clock::time_point g_lastRefresh{};
        std::chrono::steady_clock::time_point g_lastFocusCheck{};
        bool g_focused = true;
        int g_unfocusedPolls = 0;
        std::string g_lastAppliedProfile;

        // Adaptive particle cap state. 0 = inactive (the configured cap is used). Only ever
        // changed from the drawScene hook, i.e. the main thread.
        unsigned int g_adaptiveCap = 0;
        int g_slowWindows = 0;
        int g_fastWindows = 0;

        constexpr unsigned int kAdaptiveFloor = 24;
        constexpr int kSlowWindowsToStep = 2;
        constexpr int kFastWindowsToStep = 6;

        constexpr unsigned int kDefaultParticleCap = 128;
        constexpr int kDefaultBackgroundFps = 20;

        struct ProfileValues {
            char const* name;
            unsigned int particleCap;
            int backgroundFps;
            bool rendererOptimized;
            char const* sceneCull;
            char const* particleCull;
        };

        // Only features that exist are listed. No profile claims a feature the build lacks.
        // Default takes the game's draw path unchanged, so the original renderer is what you
        // get; the other profiles turn on the optimized draw path with increasing culling.
        //
        // Balanced leaves the scene cull off. Runtime telemetry (phase 3) showed that GD draws
        // level objects through CCSpriteBatchNode, so the per-sprite cull examined ~19k sprites
        // and skipped none; keeping it on by default would add checks with no measured saving.
        // It stays available as an opt-in for unbatched content and for the stronger profiles.
        constexpr ProfileValues kProfiles[] = {
            {"Default",     256, 20, false, "Off",          "Off"},
            {"Balanced",    128, 20, true,  "Off",          "Conservative"},
            {"Performance",  64, 15, true,  "Conservative", "Conservative"},
            {"Extreme",      32, 10, true,  "Aggressive",   "Aggressive"},
        };

        char const* profileName(Profile profile) {
            switch (profile) {
                case Profile::Default: return "Default";
                case Profile::Balanced: return "Balanced";
                case Profile::Performance: return "Performance";
                case Profile::Extreme: return "Extreme";
                default: return "Custom";
            }
        }

        Profile parseProfile(std::string const& value) {
            if (value == "Default") return Profile::Default;
            if (value == "Performance") return Profile::Performance;
            if (value == "Extreme") return Profile::Extreme;
            if (value == "Balanced") return Profile::Balanced;
            return Profile::Custom;
        }

        ProfileValues const* valuesFor(Profile profile) {
            for (auto const& entry : kProfiles) {
                if (std::strcmp(entry.name, profileName(profile)) == 0) return &entry;
            }
            return nullptr;
        }

        Corner parseCorner(std::string const& value) {
            if (value == "Top Right") return Corner::TopRight;
            if (value == "Bottom Left") return Corner::BottomLeft;
            if (value == "Bottom Right") return Corner::BottomRight;
            return Corner::TopLeft;
        }

        Priority parsePriority(std::string const& value) {
            if (value == "Above Normal") return Priority::AboveNormal;
            if (value == "High") return Priority::High;
            return Priority::Normal;
        }

        CullMode parseCullMode(std::string const& value) {
            if (value == "Aggressive") return CullMode::Aggressive;
            if (value == "Conservative") return CullMode::Conservative;
            return CullMode::Off;
        }

        // Margins are in world units (pixels at 1x zoom). Conservative keeps enough room that
        // a full frame of camera movement at any normal scroll speed cannot reach an object
        // that was rejected; Aggressive trades that buffer for a smaller test.
        double marginFor(CullMode mode, double conservative, double aggressive) {
            switch (mode) {
                case CullMode::Aggressive: return aggressive;
                case CullMode::Conservative: return conservative;
                default: return 0.0;
            }
        }

        // Writes a named profile into the individual settings. Only runs when the profile
        // setting itself changed, so it never fights the user's edits.
        void applyProfileValues(Profile profile) {
            auto const* values = valuesFor(profile);
            if (!values) return;

            auto* mod = Mod::get();
            mod->setSettingValue<std::string>(
                "renderer-mode", values->rendererOptimized ? "Optimized" : "Engine"
            );
            mod->setSettingValue<bool>("sprite-fast-path", values->rendererOptimized);
            mod->setSettingValue<bool>("skip-idle-particles", values->rendererOptimized);
            mod->setSettingValue<bool>("cull-offscreen-particles", std::strcmp(values->particleCull, "Off") != 0);
            mod->setSettingValue<std::string>("particle-cull-mode", values->particleCull);
            mod->setSettingValue<std::string>("scene-cull", values->sceneCull);
            mod->setSettingValue<bool>("cap-particles", true);
            mod->setSettingValue<int64_t>("particle-cap", static_cast<int64_t>(values->particleCap));
#ifdef GEODE_IS_WINDOWS
            mod->setSettingValue<bool>("background-throttle", true);
            mod->setSettingValue<int64_t>("background-fps", values->backgroundFps);
#endif
            log::info("Minimum Reborn: applied profile '{}' (particle cap {})", values->name, values->particleCap);
        }

    }

    Config const& config() {
        return g_config;
    }

    Counters& counters() {
        return g_counters;
    }

    unsigned int effectiveParticleCap() {
        auto const& cfg = config();
        if (!cfg.enabled || !cfg.adaptiveCap || g_adaptiveCap == 0) return cfg.particleCap;
        return std::min(cfg.particleCap, g_adaptiveCap);
    }

    void noteFrameHealth(double avgFrameMs, double targetFps, bool inLevel) {
        auto const& cfg = config();
        unsigned int const configured = cfg.particleCap;

        if (!cfg.enabled || !cfg.adaptiveCap || !inLevel || avgFrameMs <= 0.0 || targetFps <= 0.0) {
            if (g_adaptiveCap != 0) {
                g_adaptiveCap = 0;
                g_slowWindows = 0;
                g_fastWindows = 0;
                g_counters.capStep.store(0);
            }
            return;
        }

        double const targetMs = 1000.0 / targetFps;
        if (avgFrameMs > targetMs * 1.35) {
            g_fastWindows = 0;
            if (++g_slowWindows < kSlowWindowsToStep) return;
            g_slowWindows = 0;

            unsigned int base = g_adaptiveCap == 0 ? configured : g_adaptiveCap;
            unsigned int const next = std::max(base * 3u / 4u, kAdaptiveFloor);
            if (next < configured && next != g_adaptiveCap) {
                g_adaptiveCap = next;
                g_counters.capStep.store(next);
                log::info(
                    "Adaptive cap: particle pool lowered to {} (avg {:.2f} ms, target {:.2f} ms)",
                    next, avgFrameMs, targetMs
                );
            }
        }
        else if (avgFrameMs < targetMs * 1.05) {
            g_slowWindows = 0;
            if (++g_fastWindows < kFastWindowsToStep) return;
            g_fastWindows = 0;

            if (g_adaptiveCap == 0) return;
            unsigned int const next = g_adaptiveCap * 5u / 4u;
            if (next >= configured) {
                g_adaptiveCap = 0;
                g_counters.capStep.store(0);
                log::info("Adaptive cap: recovered, using the configured {} particles", configured);
            }
            else {
                g_adaptiveCap = next;
                g_counters.capStep.store(next);
            }
        }
        else {
            g_slowWindows = 0;
            g_fastWindows = 0;
        }
    }

    void refreshConfig() {
        auto* mod = Mod::get();

        Config c;

        // Profiles apply before the individual values are read, so a profile change takes
        // effect on the same refresh. Switching to Custom clears the marker, so picking the
        // same named profile again later writes its values again.
        c.profile = parseProfile(mod->getSettingValue<std::string>("profile"));
        std::string const profileKey = profileName(c.profile);
        if (c.profile == Profile::Custom) {
            g_lastAppliedProfile.clear();
        }
        else if (profileKey != g_lastAppliedProfile) {
            g_lastAppliedProfile = profileKey;
            applyProfileValues(c.profile);
        }

        c.enabled = mod->getSettingValue<bool>("enabled");
        c.rendererOptimized = mod->getSettingValue<std::string>("renderer-mode") != "Engine";
        c.spriteFastPath = mod->getSettingValue<bool>("sprite-fast-path");
        c.skipIdleParticles = mod->getSettingValue<bool>("skip-idle-particles");
        c.cullOffscreenParticles = mod->getSettingValue<bool>("cull-offscreen-particles");
        c.particleCull = parseCullMode(mod->getSettingValue<std::string>("particle-cull-mode"));
        c.sceneCull = parseCullMode(mod->getSettingValue<std::string>("scene-cull"));
        c.particleCullMargin = marginFor(c.particleCull, 128.0, 8.0);
        c.sceneCullMargin = marginFor(c.sceneCull, 160.0, 16.0);
        c.capParticles = mod->getSettingValue<bool>("cap-particles");
        c.adaptiveCap = mod->getSettingValue<bool>("adaptive-cap");

        int64_t cap = mod->getSettingValue<int64_t>("particle-cap");
        c.particleCap = static_cast<unsigned int>(std::clamp<int64_t>(cap, 4, 1000));
        int64_t budget = mod->getSettingValue<int64_t>("spawn-budget");
        c.spawnBudget = static_cast<unsigned int>(std::clamp<int64_t>(budget, 0, 200));

        c.editorCulling = mod->getSettingValue<bool>("editor-culling");
        c.editorCull = parseCullMode(mod->getSettingValue<std::string>("editor-cull-mode"));
        c.editorCullMargin = marginFor(c.editorCull, 160.0, 16.0);
        c.noInputDelay = mod->getSettingValue<bool>("no-input-delay");

#ifdef GEODE_IS_WINDOWS
        c.renderGate = mod->getSettingValue<bool>("render-gate");
        int64_t gateFps = mod->getSettingValue<int64_t>("render-gate-fps");
        c.renderGateFps = static_cast<double>(std::clamp<int64_t>(gateFps, 30, 360));

        c.backgroundThrottle = mod->getSettingValue<bool>("background-throttle");
        int64_t bgFps = mod->getSettingValue<int64_t>("background-fps");
        c.backgroundFps = static_cast<double>(std::clamp<int64_t>(bgFps, 1, 60));

        c.timerResolution = mod->getSettingValue<bool>("timer-resolution");
        c.disablePowerThrottling = mod->getSettingValue<bool>("disable-power-throttling");
        c.priority = parsePriority(mod->getSettingValue<std::string>("process-priority"));
#else
        c.renderGate = false;
        c.backgroundThrottle = false;
        c.timerResolution = false;
        c.disablePowerThrottling = false;
        c.priority = Priority::Normal;
#endif

        c.showOverlay = mod->getSettingValue<bool>("show-overlay");
        c.overlayDetailed = mod->getSettingValue<std::string>("overlay-detail") == "Detailed";
        c.overlayCorner = parseCorner(mod->getSettingValue<std::string>("overlay-corner"));
        c.overlayScale = std::clamp(mod->getSettingValue<float>("overlay-scale"), 0.2f, 2.0f);
        int64_t opacity = mod->getSettingValue<int64_t>("overlay-opacity");
        c.overlayOpacity = static_cast<std::uint8_t>(std::clamp<int64_t>(opacity, 20, 255));
        int64_t targetFps = mod->getSettingValue<int64_t>("target-fps");
        c.targetFps = static_cast<double>(std::clamp<int64_t>(targetFps, 20, 360));

        g_config = c;
        applySystemTuning();
    }

    void refreshConfigIfDue(std::chrono::steady_clock::time_point now) {
        using namespace std::chrono_literals;
        if (now - g_lastRefresh >= 500ms) {
            g_lastRefresh = now;
            refreshConfig();
        }
    }

    void resetToDefaults() {
        auto* mod = Mod::get();
        mod->setSettingValue<bool>("enabled", true);
        mod->setSettingValue<std::string>("renderer-mode", "Optimized");
        mod->setSettingValue<bool>("sprite-fast-path", true);
        mod->setSettingValue<bool>("skip-idle-particles", true);
        mod->setSettingValue<bool>("cull-offscreen-particles", true);
        mod->setSettingValue<std::string>("particle-cull-mode", "Conservative");
        mod->setSettingValue<std::string>("scene-cull", "Off");
        mod->setSettingValue<bool>("cap-particles", true);
        mod->setSettingValue<int64_t>("particle-cap", static_cast<int64_t>(kDefaultParticleCap));
        mod->setSettingValue<bool>("adaptive-cap", false);
        mod->setSettingValue<int64_t>("spawn-budget", 0);
        mod->setSettingValue<bool>("no-input-delay", true);
        mod->setSettingValue<bool>("editor-culling", true);
        mod->setSettingValue<std::string>("editor-cull-mode", "Conservative");
#ifdef GEODE_IS_WINDOWS
        mod->setSettingValue<bool>("render-gate", false);
        mod->setSettingValue<int64_t>("render-gate-fps", 60);
        mod->setSettingValue<bool>("background-throttle", true);
        mod->setSettingValue<int64_t>("background-fps", kDefaultBackgroundFps);
        mod->setSettingValue<bool>("timer-resolution", true);
        mod->setSettingValue<bool>("disable-power-throttling", true);
        mod->setSettingValue<std::string>("process-priority", "Normal");
#endif
        mod->setSettingValue<bool>("show-overlay", true);
        mod->setSettingValue<int64_t>("benchmark-seconds", 10);
        mod->setSettingValue<std::string>("overlay-detail", "Compact");
        mod->setSettingValue<std::string>("overlay-corner", "Top Right");
        mod->setSettingValue<float>("overlay-scale", 0.5f);
        mod->setSettingValue<int64_t>("overlay-opacity", 200);
        mod->setSettingValue<int64_t>("target-fps", 60);

        g_lastAppliedProfile.clear();
        mod->setSettingValue<std::string>("profile", "Balanced");
        refreshConfig();
        log::info("Minimum Reborn: settings reset to defaults");
    }

    bool processHasForeground(std::chrono::steady_clock::time_point now) {
#ifdef GEODE_IS_WINDOWS
        using namespace std::chrono_literals;
        if (now - g_lastFocusCheck >= 250ms) {
            g_lastFocusCheck = now;

            // Two independent checks; focused if either says so. Unfocused is reported only
            // after two unfocused polls in a row (~0.5 s), so a one-off wrong answer can
            // never throttle drawing.
            bool const ownsKeyboardFocus = GetFocus() != nullptr;
            bool foregroundIsOurs = false;
            if (HWND foreground = GetForegroundWindow()) {
                DWORD ownerPid = 0;
                GetWindowThreadProcessId(foreground, &ownerPid);
                foregroundIsOurs = ownerPid == GetCurrentProcessId();
            }

            if (ownsKeyboardFocus || foregroundIsOurs) {
                g_unfocusedPolls = 0;
                g_focused = true;
            }
            else if (++g_unfocusedPolls >= 2) {
                g_focused = false;
            }
        }
        return g_focused;
#else
        (void)now;
        return true;
#endif
    }

    bool cachedForeground() {
        return g_focused;
    }

    bool backgroundThrottled(std::chrono::steady_clock::time_point now) {
        return g_config.enabled && g_config.backgroundThrottle && !processHasForeground(now);
    }
}

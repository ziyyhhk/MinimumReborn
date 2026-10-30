#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// Minimum Reborn: shared, cached state.
//
// The mod hooks functions that run thousands of times per frame (CCSprite::draw and
// CCParticleSystemQuad::draw). Mod::getSettingValue does a string-keyed lookup and may
// touch the setting store, so the hooks never read settings directly: they read this
// plain snapshot. It is refreshed on load and about twice a second from the
// CCDirector::drawScene hook, and immediately when the mod's own UI changes a setting.
//
// All writes happen on the main thread. The counters are atomics only so that a stray
// log/read path can never observe a torn value.

namespace reborn {

    enum class Profile : std::uint8_t { Custom = 0, Default, Balanced, Performance, Extreme };

    enum class Corner : std::uint8_t { TopLeft = 0, TopRight, BottomLeft, BottomRight };

    // 0 = Normal, 1 = Above Normal, 2 = High (Windows only).
    enum class Priority : std::uint8_t { Normal = 0, AboveNormal = 1, High = 2 };

    struct Config {
        bool enabled = true;

        // Rendering fast paths.
        bool spriteFastPath = true;      // skip draw() for sprites that cannot produce pixels
        bool skipIdleParticles = true;   // skip draw() for particle systems with no live particles
        bool cullOffscreenParticles = true;
        bool capParticles = true;
        unsigned int particleCap = 128;
        unsigned int spawnBudget = 0;    // particle systems per frame that get a full pool; 0 = off

        // Render gate. Off by default. Draws at a fixed rate while the scheduler keeps
        // running at the game's normal rate (or 20 fps while the window is unfocused).
        bool renderGate = false;
        double renderGateFps = 60.0;
        bool backgroundThrottle = true;
        double backgroundFps = 20.0;

        // Monitor.
        bool showOverlay = true;
        bool overlayDetailed = false;
        Corner overlayCorner = Corner::TopLeft;
        float overlayScale = 0.5f;
        std::uint8_t overlayOpacity = 200;
        double targetFps = 60.0;

        // System (Windows).
        bool timerResolution = true;
        bool disablePowerThrottling = true;
        Priority priority = Priority::Normal;

        Profile profile = Profile::Balanced;
    };

    // Proof-of-work counters. Monotonic; the overlay shows deltas per second plus totals.
    struct Counters {
        std::atomic<std::uint64_t> invisibleSpriteDrawsSkipped{0};
        std::atomic<std::uint64_t> idleParticleDrawsSkipped{0};
        std::atomic<std::uint64_t> offscreenParticleDrawsSkipped{0};
        std::atomic<std::uint64_t> particlePoolsCapped{0};
        std::atomic<std::uint64_t> particleSystemsCreated{0};
        std::atomic<std::uint64_t> framesDrawn{0};
        std::atomic<std::uint64_t> framesLogicOnly{0};
        std::atomic<std::uint64_t> schedulerUpdates{0};

        std::atomic<std::uint32_t> fps{0};        // drawn frames per second, wall clock
        std::atomic<std::uint32_t> logicFps{0};   // scheduler updates per second
        std::atomic<std::uint8_t> bottleneck{0};  // Bottleneck enum below
        std::atomic<bool> throttled{false};

        // Deltas for the last measurement window (0.5 s), filled by the frame hook.
        std::atomic<std::uint32_t> spriteSkipsPerSec{0};
        std::atomic<std::uint32_t> particleSkipsPerSec{0};
        std::atomic<std::uint32_t> logicOnlyPerSec{0};
        std::atomic<std::uint32_t> cappedPerSec{0};
    };

    // 0 = unknown, 1 = main-thread/draw bound hint, 2 = GPU or outside-draw hint.
    enum class Bottleneck : std::uint8_t { Unknown = 0, DrawBound = 1, GpuOrOther = 2 };

    Config const& config();
    Counters& counters();

    void refreshConfig();
    void refreshConfigIfDue(std::chrono::steady_clock::time_point now);

    // Writes every setting back to its documented default (profile Balanced), then refreshes.
    void resetToDefaults();

    // Windows only; no-op elsewhere. Cheap when nothing changed.
    void applySystemTuning();

    // Windows only: true unless the game window is positively known to be in the background.
    // Polls at most every 250 ms; needs two unfocused polls in a row.
    bool processHasForeground(std::chrono::steady_clock::time_point now);

    // True while drawing is being limited because the window is not focused.
    bool backgroundThrottled(std::chrono::steady_clock::time_point now);
}

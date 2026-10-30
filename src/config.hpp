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

    // How far outside the viewport an object has to be before its draw is skipped.
    // Conservative uses a generous margin, Aggressive a small one. Off disables the test.
    enum class CullMode : std::uint8_t { Off = 0, Conservative, Aggressive };

    struct Config {
        bool enabled = true;

        // Renderer mode. False = the game's draw path is taken unchanged for every node;
        // true = Minimum Reborn's draw path (fast-outs + culling) is active.
        bool rendererOptimized = true;

        // Rendering fast paths.
        bool spriteFastPath = true;      // skip draw() for sprites that cannot produce pixels
        bool skipIdleParticles = true;   // skip draw() for particle systems with no live particles
        bool cullOffscreenParticles = true;
        CullMode particleCull = CullMode::Conservative;
        CullMode sceneCull = CullMode::Conservative;
        double particleCullMargin = 96.0;   // world units kept around the viewport
        double sceneCullMargin = 128.0;
        bool capParticles = true;
        bool adaptiveCap = false;        // lowers the cap while drawn frames stay slow
        unsigned int particleCap = 128;
        unsigned int spawnBudget = 0;    // particle systems per frame that get a full pool; 0 = off

        // Editor-only culling. Applies the same draw-only visibility tests while the level
        // editor is open, independently of the gameplay scene/particle settings. Object data,
        // selection, serialization and editing callbacks are never touched.
        bool editorCulling = true;
        CullMode editorCull = CullMode::Conservative;
        double editorCullMargin = 160.0;

        // No Additional Input Delay: moves every piece of mod bookkeeping out of the part of
        // the frame that runs before the game's scheduler update and scene draw. The mod then
        // executes no instrumentation, statistics or Win32 probe before the game processes
        // input and simulation. It does not change the game's input code, timing or FPS cap.
        bool noInputDelay = true;

        // Render gate. Off by default. Draws at a fixed rate while the scheduler keeps
        // running at the game's normal rate (or 20 fps while the window is unfocused).
        bool renderGate = false;
        double renderGateFps = 60.0;
        bool backgroundThrottle = true;
        double backgroundFps = 20.0;

        // Monitor.
        bool showOverlay = true;
        bool overlayDetailed = false;
        Corner overlayCorner = Corner::TopRight;
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

        // Scene visibility culling. "Examined" counts viewport tests actually performed,
        // "culled" counts tests that returned "skip this draw".
        std::atomic<std::uint64_t> sceneSpritesExamined{0};
        std::atomic<std::uint64_t> sceneSpriteDrawsSkipped{0};
        std::atomic<std::uint64_t> particleSystemsExamined{0};

        // Batch-node telemetry. GD renders level objects through CCSpriteBatchNode, so these
        // counts show how much of the scene actually reaches the per-sprite hook.
        std::atomic<std::uint64_t> batchDraws{0};
        std::atomic<std::uint64_t> batchQuads{0};
        std::atomic<std::uint64_t> emptyBatchDrawsSkipped{0};

        // Editor-only culling. Kept apart from the gameplay counters so a claim about editor
        // behavior can be checked on its own.
        std::atomic<std::uint64_t> editorSpritesExamined{0};
        std::atomic<std::uint64_t> editorSpriteDrawsSkipped{0};
        std::atomic<std::uint64_t> editorParticlesExamined{0};
        std::atomic<std::uint64_t> editorParticleDrawsSkipped{0};
        std::atomic<std::uint64_t> deferredBookkeeping{0};

        std::atomic<std::uint32_t> fps{0};        // drawn frames per second, wall clock
        std::atomic<std::uint32_t> logicFps{0};   // scheduler updates per second
        std::atomic<std::uint8_t> bottleneck{0};  // Bottleneck enum below
        std::atomic<bool> throttled{false};

        // Deltas for the last measurement window (0.5 s), filled by the frame hook.
        std::atomic<std::uint32_t> spriteSkipsPerSec{0};
        std::atomic<std::uint32_t> particleSkipsPerSec{0};
        std::atomic<std::uint32_t> logicOnlyPerSec{0};
        std::atomic<std::uint32_t> cappedPerSec{0};
        std::atomic<std::uint32_t> sceneCulledPerSec{0};
        std::atomic<std::uint32_t> sceneExaminedPerSec{0};
        std::atomic<std::uint32_t> particleExaminedPerSec{0};
        std::atomic<std::uint32_t> editorCulledPerSec{0};
        std::atomic<std::uint32_t> capStep{0};   // 0 = configured cap, >0 = adaptive cap in effect
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

    // Last focus answer without performing a Win32 probe. Used when the input-delay setting
    // defers the probe to after the scene has been drawn.
    bool cachedForeground();

    // Adaptive particle cap (opt-in). Called about twice a second with the average frame time
    // of drawn frames. Lowers the cap in steps while frames stay slow, relaxes back towards
    // the configured cap when they recover. Never touches physics, timing or input.
    void noteFrameHealth(double avgFrameMs, double targetFps, bool inLevel);

    // True while drawing is being limited because the window is not focused.
    bool backgroundThrottled(std::chrono::steady_clock::time_point now);
}

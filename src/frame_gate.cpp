#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <algorithm>
#include <chrono>
#include "config.hpp"
#include "benchmark.hpp"
#include "frame_stats.hpp"
#include "hud.hpp"
#include "rendering.hpp"

using namespace geode::prelude;

// Frame hook: measurement, the render gate and the per-second counters.
//
// Measurement
//   Every drawn frame is timed with std::chrono::steady_clock (wall clock), not with the
//   game's delta time, so the numbers do not depend on how the game paces its loop. The
//   rolling window holds the last 240 drawn frames. Nothing here allocates.
//
// Render gate (opt-in, off by default)
//   Replaces "draw every frame" with "draw at Render FPS while the scheduler keeps its
//   normal rate". A skipped frame runs exactly what the proven Draw Divide implementation
//   runs: one scheduler update with the delta the main loop already computed. It does NOT
//   call calculateDeltaTime(), so simulation speed, physics and input processing are
//   untouched; the game keeps reading input on the same schedule it always did. Only the
//   number of rendered frames changes.
//
//   While the window is unfocused the gate draws at Background FPS (default 20) instead.
//   Nobody is looking, so the GPU and driver are freed for other work.
//
//   The gate never applies during the first 300 frames (loading), while a scene switch is
//   pending, or when it is disabled. It is never on by default.
//
// Input latency
//   The mod adds no input hook, no queue, no buffer, no lock and no thread. Input is read
//   by the game on the main thread exactly as before. Skipped render frames do not postpone
//   input processing; the scheduler still runs on those frames. See README for the
//   distinction between input processing, render frame time and display latency.

namespace reborn {

    namespace {
        FrameStats g_frameStats;
        constexpr int kBottleneckWindow = 60;
    }

    FrameStats& frameStats() {
        return g_frameStats;
    }
}

namespace {

    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t kSettleFrames = 300;

    struct GateState {
        Clock::time_point last = Clock::now();
        Clock::time_point lastDrawn = Clock::now();
        Clock::time_point windowStart = Clock::now();
        Clock::time_point fpsStart = Clock::now();
        double accum = 0.0;
        std::uint32_t windowDrawn = 0;
        std::uint32_t windowAll = 0;
        std::uint64_t seenScheduler = 0;
        std::uint64_t seenSpriteSkips = 0;
        std::uint64_t seenParticleSkips = 0;
        std::uint64_t seenCapped = 0;
        std::uint64_t seenLogicOnly = 0;
        std::uint64_t seenSceneCulled = 0;
        std::uint64_t seenSceneExamined = 0;
        std::uint64_t seenParticleExamined = 0;
    };

    GateState& state() {
        static GateState s;
        return s;
    }

    // Refreshes fps, logic fps, bottleneck hint and the per-second proof counters.
    // Runs twice a second; the values in between are plain loads by the overlay.
    void closeWindow(Clock::time_point now, bool measurable) {
        auto& s = state();
        auto& counters = reborn::counters();

        double const span = std::chrono::duration<double>(now - s.fpsStart).count();
        if (span < 0.5) return;

        std::uint32_t const drawn = s.windowDrawn;
        std::uint32_t const fps = static_cast<std::uint32_t>(static_cast<double>(drawn) / span + 0.5);
        counters.fps.store(fps);

        std::uint64_t const updates = counters.schedulerUpdates.load(std::memory_order_relaxed);
        std::uint64_t const updateDelta = updates >= s.seenScheduler ? updates - s.seenScheduler : 0;
        s.seenScheduler = updates;
        counters.logicFps.store(static_cast<std::uint32_t>(
            updateDelta > 0 ? static_cast<double>(updateDelta) / span : static_cast<double>(s.windowAll) / span
        ));

        auto const spriteSkips = counters.invisibleSpriteDrawsSkipped.load(std::memory_order_relaxed);
        auto const idleSkips = counters.idleParticleDrawsSkipped.load(std::memory_order_relaxed);
        auto const offscreenSkips = counters.offscreenParticleDrawsSkipped.load(std::memory_order_relaxed);
        auto const capped = counters.particlePoolsCapped.load(std::memory_order_relaxed);
        auto const logicOnly = counters.framesLogicOnly.load(std::memory_order_relaxed);

        counters.spriteSkipsPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(spriteSkips - s.seenSpriteSkips) / span + 0.5
        ));
        counters.particleSkipsPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(idleSkips + offscreenSkips - s.seenParticleSkips) / span + 0.5
        ));
        counters.cappedPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(capped - s.seenCapped) / span + 0.5
        ));
        counters.logicOnlyPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(logicOnly - s.seenLogicOnly) / span + 0.5
        ));
        auto const sceneCulled = counters.sceneSpriteDrawsSkipped.load(std::memory_order_relaxed);
        auto const sceneExamined = counters.sceneSpritesExamined.load(std::memory_order_relaxed);
        auto const particleExamined = counters.particleSystemsExamined.load(std::memory_order_relaxed);
        counters.sceneCulledPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(sceneCulled - s.seenSceneCulled) / span + 0.5
        ));
        counters.sceneExaminedPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(sceneExamined - s.seenSceneExamined) / span + 0.5
        ));
        counters.particleExaminedPerSec.store(static_cast<std::uint32_t>(
            static_cast<double>(particleExamined - s.seenParticleExamined) / span + 0.5
        ));
        s.seenSpriteSkips = spriteSkips;
        s.seenParticleSkips = idleSkips + offscreenSkips;
        s.seenCapped = capped;
        s.seenLogicOnly = logicOnly;
        s.seenSceneCulled = sceneCulled;
        s.seenSceneExamined = sceneExamined;
        s.seenParticleExamined = particleExamined;

        // Bottleneck hint from this mod's own numbers. workMs is the CPU time of the
        // drawScene call (traversal + GL submission), frameMs is the wall time between
        // drawn frames. If most of a frame is spent submitting, the draw path is the
        // limiter; if very little is spent there, the time goes to the GPU or to waiting.
        // It is a hint, not a GPU measurement: there is deliberately no GPU timer.
        auto const& stats = reborn::frameStats();
        double const frameMs = stats.averageFrameMs(reborn::kBottleneckWindow);
        double const workMs = stats.averageWorkMs(reborn::kBottleneckWindow);
        if (!measurable || stats.size() < 30 || frameMs <= 0.05 || workMs <= 0.0) {
            counters.bottleneck.store(static_cast<std::uint8_t>(reborn::Bottleneck::Unknown));
        }
        else {
            double const ratio = workMs / frameMs;
            auto const hint = ratio >= 0.7 ? reborn::Bottleneck::DrawBound
                            : ratio <= 0.4 ? reborn::Bottleneck::GpuOrOther
                            : reborn::Bottleneck::Unknown;
            counters.bottleneck.store(static_cast<std::uint8_t>(hint));
        }

        // Adaptive particle cap (opt-in): looks at the same window, steps the pool size down
        // while drawn frames stay slow and relaxes it when they recover. Never touches timing.
        if (measurable) {
            reborn::noteFrameHealth(frameMs, reborn::config().targetFps, PlayLayer::get() != nullptr);
        }
        s.windowDrawn = 0;
        s.windowAll = 0;
        s.fpsStart = now;
    }

}

struct RebornSchedulerTick : Modify<RebornSchedulerTick, CCScheduler> {
    void update(float dt) {
        ++reborn::counters().schedulerUpdates;
        CCScheduler::update(dt);
    }
};

struct RebornFrameGate : Modify<RebornFrameGate, CCDirector> {
    void drawScene() {
        using namespace std::chrono_literals;
        auto& s = state();
        auto& counters = reborn::counters();

        // Starts the per-frame viewport snapshot, the transform cache generation and the
        // particle spawn budget before anything can create or draw a node.
        reborn::renderFrameBegin();

        Clock::time_point const now = Clock::now();
        double const rawDelta = std::chrono::duration<double>(now - s.last).count();
        s.last = now;
        double const wallDelta = std::min(rawDelta, 0.25);

        reborn::refreshConfigIfDue(now);
        auto const& cfg = reborn::config();

        auto const totalFrames = this->getTotalFrames();
        bool const focused = reborn::processHasForeground(now);
        bool const resumed = rawDelta > 1.0;

        if (resumed) {
            s.fpsStart = now;
            s.windowStart = now;
            s.windowDrawn = 0;
            s.windowAll = 0;
            s.lastDrawn = now;
            s.seenScheduler = counters.schedulerUpdates.load(std::memory_order_relaxed);
            reborn::frameStats().clear();
        }
        bool const measurable = totalFrames >= kSettleFrames && focused && !resumed;

        ++s.windowAll;
        closeWindow(now, measurable);

        // The gate is decided per frame. interval 0 means "draw every frame".
        double interval = 0.0;
        if (cfg.enabled) {
            if (cfg.backgroundThrottle && !focused) {
                interval = 1.0 / cfg.backgroundFps;
            }
            else if (cfg.renderGate && PlayLayer::get() != nullptr) {
                interval = 1.0 / cfg.renderGateFps;
            }
        }
        counters.throttled.store(cfg.enabled && cfg.backgroundThrottle && !focused);

        bool const mustDraw = interval <= 0.0
                           || totalFrames < kSettleFrames
                           || this->getNextScene() != nullptr;

        if (!mustDraw) {
            s.accum += wallDelta;
            if (s.accum < interval) {
                // Logic-only frame: no clear, no scene visit, no swap. Physics and input
                // processing still run on the scheduler's normal schedule.
                if (!this->isPaused()) {
                    this->getScheduler()->update(this->getDeltaTime());
                }
                ++counters.framesLogicOnly;
                return;
            }
            s.accum -= interval;
            // Fell more than one interval behind: drop the backlog instead of drawing in a
            // burst (a burst would look like a stutter and would not help anyone).
            if (s.accum > interval) s.accum = 0.0;
        }
        else {
            s.accum = 0.0;
        }

        Clock::time_point const drawStart = Clock::now();
        CCDirector::drawScene();
        Clock::time_point const drawEnd = Clock::now();

        ++counters.framesDrawn;
        ++s.windowDrawn;

        if (measurable) {
            double const frameMs = std::chrono::duration<double, std::milli>(drawStart - s.lastDrawn).count();
            double const workMs = std::chrono::duration<double, std::milli>(drawEnd - drawStart).count();
            reborn::frameStats().push(std::min(frameMs, 1000.0), workMs);
            // Feeds the in-game benchmark with the same number the overlay uses. No-op
            // unless a run is active. Paused frames are excluded: the pause layer still
            // draws at full rate, and counting it would turn "start the benchmark, resume
            // the level" into a measurement of a static pause screen.
            if (!this->isPaused()) {
                reborn::benchmarkNoteFrame(drawEnd, frameMs, workMs);
            }
        }
        s.lastDrawn = drawStart;

        reborn::ensureHud(totalFrames);
    }
};

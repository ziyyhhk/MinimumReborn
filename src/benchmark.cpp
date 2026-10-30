#include <Geode/Geode.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "benchmark.hpp"
#include "config.hpp"

using namespace geode::prelude;

// See benchmark.hpp. Main thread only; no locks because every caller is the draw hook or the
// popup, both on the main thread.

namespace reborn {

    namespace {

        constexpr int kMaxSamples = 16384;

        float g_samples[kMaxSamples];
        int g_count = 0;
        bool g_active = false;
        double g_window = 10.0;
        double g_drawSum = 0.0;
        std::chrono::steady_clock::time_point g_start{};
        BenchmarkResult g_result;
        char g_summary[224] = {};
        std::uint64_t g_baseSceneCulled = 0;
        std::uint64_t g_baseParticleSkipped = 0;
        std::uint64_t g_baseSpriteSkipped = 0;
        std::uint64_t g_baseSceneExamined = 0;
        std::uint64_t g_baseParticleExamined = 0;
        std::uint64_t g_baseFramesDrawn = 0;
        std::uint64_t g_baseLogicOnly = 0;
        std::uint64_t g_baseBatchDraws = 0;
        std::uint64_t g_baseBatchQuads = 0;

        char const* cullShort(CullMode mode) {
            switch (mode) {
                case CullMode::Conservative: return "con";
                case CullMode::Aggressive: return "agg";
                default: return "off";
            }
        }

    }

    void benchmarkStart(std::chrono::steady_clock::time_point now, double seconds) {
        if (g_active) return;

        auto const& cfg = config();
        g_window = std::clamp(seconds, 3.0, 60.0);
        g_active = true;
        g_count = 0;
        g_drawSum = 0.0;
        g_start = now;
        g_result = BenchmarkResult{};

        std::snprintf(
            g_result.mode, sizeof(g_result.mode),
            "renderer %s, scene %s, particles %s",
            cfg.rendererOptimized ? "optimized" : "engine",
            cullShort(cfg.sceneCull), cullShort(cfg.particleCull)
        );

        auto& counters = reborn::counters();
        g_baseSceneCulled = counters.sceneSpriteDrawsSkipped.load(std::memory_order_relaxed);
        g_baseParticleSkipped = counters.idleParticleDrawsSkipped.load(std::memory_order_relaxed)
                              + counters.offscreenParticleDrawsSkipped.load(std::memory_order_relaxed);
        g_baseSpriteSkipped = counters.invisibleSpriteDrawsSkipped.load(std::memory_order_relaxed);
        g_baseSceneExamined = counters.sceneSpritesExamined.load(std::memory_order_relaxed);
        g_baseParticleExamined = counters.particleSystemsExamined.load(std::memory_order_relaxed);
        g_baseFramesDrawn = counters.framesDrawn.load(std::memory_order_relaxed);
        g_baseLogicOnly = counters.framesLogicOnly.load(std::memory_order_relaxed);
        g_baseBatchDraws = counters.batchDraws.load(std::memory_order_relaxed);
        g_baseBatchQuads = counters.batchQuads.load(std::memory_order_relaxed);

        g_summary[0] = '\0';
        log::info("Minimum Reborn: benchmark started ({:.0f} s, {})", g_window, g_result.mode);
    }

    void benchmarkNoteFrame(std::chrono::steady_clock::time_point now, double frameMs, double drawMs) {
        if (!g_active) return;
        if (!std::isfinite(frameMs) || frameMs <= 0.0) return;
        if (std::isfinite(drawMs) && drawMs >= 0.0) g_drawSum += drawMs;

        if (g_count < kMaxSamples) {
            g_samples[g_count++] = static_cast<float>(std::min(frameMs, 1000.0));
        }
        if (std::chrono::duration<double>(now - g_start).count() >= g_window || g_count >= kMaxSamples) {
            benchmarkStop(now);
        }
    }

    void benchmarkStop(std::chrono::steady_clock::time_point now) {
        if (!g_active) return;
        g_active = false;

        double const seconds = std::chrono::duration<double>(now - g_start).count();
        if (g_count < 30 || seconds <= 0.1) {
            log::warn(
                "Minimum Reborn: benchmark discarded ({} drawn frames in {:.1f} s - play a level "
                "with the game focused)", g_count, seconds
            );
            return;
        }

        // One sort at the end, never while recording.
        float* sorted = g_samples;
        std::sort(sorted, sorted + g_count);

        auto const percentile = [&](double p) {
            int const index = std::clamp(
                static_cast<int>(std::lround(p * static_cast<double>(g_count - 1))), 0, g_count - 1
            );
            return static_cast<double>(sorted[index]);
        };

        double sum = 0.0;
        for (int i = 0; i < g_count; ++i) sum += sorted[i];

        auto& counters = reborn::counters();
        std::uint64_t const sceneCulled = counters.sceneSpriteDrawsSkipped.load(std::memory_order_relaxed) - g_baseSceneCulled;
        std::uint64_t const sceneExamined = counters.sceneSpritesExamined.load(std::memory_order_relaxed) - g_baseSceneExamined;
        std::uint64_t const particleExamined = counters.particleSystemsExamined.load(std::memory_order_relaxed) - g_baseParticleExamined;
        std::uint64_t const particleSkipped = counters.idleParticleDrawsSkipped.load(std::memory_order_relaxed)
                                            + counters.offscreenParticleDrawsSkipped.load(std::memory_order_relaxed)
                                            - g_baseParticleSkipped;
        std::uint64_t const spriteSkipped = counters.invisibleSpriteDrawsSkipped.load(std::memory_order_relaxed) - g_baseSpriteSkipped;

        g_result.valid = true;
        g_result.seconds = seconds;
        g_result.frames = static_cast<unsigned int>(g_count);
        g_result.avgFrameMs = sum / static_cast<double>(g_count);
        g_result.avgFps = 1000.0 / std::max(0.001, g_result.avgFrameMs);
        g_result.p50FrameMs = percentile(0.50);
        g_result.p95FrameMs = percentile(0.95);
        g_result.variabilityMs = g_result.p95FrameMs - g_result.p50FrameMs;
        // 1% low: average frame time of the slowest 1% of frames, expressed as FPS.
        int const slowCount = std::max(1, g_count / 100);
        double slowSum = 0.0;
        for (int i = g_count - slowCount; i < g_count; ++i) slowSum += sorted[i];
        double const slowAvg = slowSum / static_cast<double>(slowCount);
        g_result.onePercentLowFps = slowAvg > 0.0 ? 1000.0 / slowAvg : 0.0;
        g_result.avgDrawMs = g_drawSum / static_cast<double>(g_count);
        g_result.batchDraws = counters.batchDraws.load(std::memory_order_relaxed) - g_baseBatchDraws;
        g_result.batchQuads = counters.batchQuads.load(std::memory_order_relaxed) - g_baseBatchQuads;
        g_result.sceneCulled = sceneCulled;
        g_result.particleSkipped = particleSkipped;
        g_result.spriteSkipped = spriteSkipped;
        g_result.scenesExamined = sceneExamined;
        g_result.particlesExamined = particleExamined;
        g_result.framesDrawn = counters.framesDrawn.load(std::memory_order_relaxed) - g_baseFramesDrawn;
        g_result.framesLogicOnly = counters.framesLogicOnly.load(std::memory_order_relaxed) - g_baseLogicOnly;

        std::snprintf(
            g_summary, sizeof(g_summary),
            "%.1fs: %.0f fps | avg %.2f ms | p95 %.2f | 1%% low %.0f | draw %.2f ms | scene %llu/%llu | particles %llu/%llu",
            seconds, g_result.avgFps, g_result.avgFrameMs, g_result.p95FrameMs, g_result.onePercentLowFps,
            g_result.avgDrawMs,
            static_cast<unsigned long long>(sceneCulled), static_cast<unsigned long long>(sceneExamined),
            static_cast<unsigned long long>(particleSkipped), static_cast<unsigned long long>(particleExamined)
        );

        log::info(
            "Minimum Reborn: benchmark {} - {} drawn frames in {:.1f} s, avg {:.2f} ms (p50 {:.2f}, p95 {:.2f}), "
            "1% low {:.0f} fps, variability {:.2f} ms, draw {:.2f} ms | scene culled {}/{} | particles skipped {}/{} | "
            "batches {} ({} quads) | {}",
            g_result.mode, g_result.frames, seconds, g_result.avgFrameMs, g_result.p50FrameMs,
            g_result.p95FrameMs, g_result.onePercentLowFps, g_result.variabilityMs, g_result.avgDrawMs,
            static_cast<unsigned long long>(sceneCulled), static_cast<unsigned long long>(sceneExamined),
            static_cast<unsigned long long>(particleSkipped), static_cast<unsigned long long>(particleExamined),
            static_cast<unsigned long long>(g_result.batchDraws), static_cast<unsigned long long>(g_result.batchQuads),
            g_summary
        );
    }

    bool benchmarkActive() {
        return g_active;
    }

    double benchmarkProgress(std::chrono::steady_clock::time_point now) {
        if (!g_active) return 1.0;
        return std::clamp(std::chrono::duration<double>(now - g_start).count() / g_window, 0.0, 1.0);
    }

    BenchmarkResult const& benchmarkLatest() {
        return g_result;
    }

    char const* benchmarkSummary() {
        return g_summary;
    }
}

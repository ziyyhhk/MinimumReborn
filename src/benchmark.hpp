#pragma once

#include <chrono>
#include <cstdint>

// In-game benchmark recorder.
//
// Records the wall-clock frame time of drawn frames for a fixed window (default 10 s) and
// reports average FPS, average frame time, p95 frame time, 1% low FPS and the per-second
// skip/cull counters accumulated over that window. Only frames that are drawn and measurable
// (in a level, focused, game settled) are recorded, so the result describes gameplay.
//
// No allocation happens while recording: the samples live in a fixed 16384-entry array and
// recording stops when it is full. Sorting happens once, at stop, on the main thread. The
// numbers are the same wall clock the overlay uses; nothing here is a GPU measurement.

namespace reborn {

    struct BenchmarkResult {
        bool valid = false;
        double seconds = 0.0;
        unsigned int frames = 0;
        double avgFps = 0.0;
        double avgFrameMs = 0.0;
        double p50FrameMs = 0.0;
        double p95FrameMs = 0.0;
        double onePercentLowFps = 0.0;
        double variabilityMs = 0.0;     // p95 - p50, same window
        std::uint64_t sceneCulled = 0;
        std::uint64_t particleSkipped = 0;
        std::uint64_t spriteSkipped = 0;
        std::uint64_t scenesExamined = 0;
        std::uint64_t particlesExamined = 0;
        std::uint64_t framesDrawn = 0;
        std::uint64_t framesLogicOnly = 0;
        char mode[72] = {};
    };

    // Starts a run. Ignored while one is already running.
    void benchmarkStart(std::chrono::steady_clock::time_point now, double seconds);

    // Records one drawn frame. Stops the run automatically when the window is over.
    void benchmarkNoteFrame(std::chrono::steady_clock::time_point now, double frameMs);

    // Stops early. A run that recorded nothing is discarded rather than reported.
    void benchmarkStop(std::chrono::steady_clock::time_point now);

    bool benchmarkActive();
    double benchmarkProgress(std::chrono::steady_clock::time_point now);

    // Last finished run; valid() is false until one completes.
    BenchmarkResult const& benchmarkLatest();

    // One-line description of the last run, for the pause panel and the log. Empty if none.
    char const* benchmarkSummary();
}

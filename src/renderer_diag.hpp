#pragma once

// Phase 1 of the GPU renderer (experimental, opt-in, off by default, Windows).
//
// This is the measurement half of a Bismuth-style renderer, deliberately separated
// from any behavior change: it counts the level's object and sprite population once
// at level entry, counts sprites that cannot produce a pixel (zero opacity or zero
// scale on an axis), and samples the size of GD's per-frame visibility set
// (GJBaseGameLayer::m_visibleObjectsCount) inside the original updateVisibility hook.
//
// Nothing here skips work, rewrites state or changes rendering; the only output is
// log lines and four counters that the Detailed overlay can show. It exists so the
// design of the GPU visibility pass (Phase 2) is driven by this level's real numbers
// instead of guesses.

#include <cstdint>

namespace reborn {

    // One scan per level entry, called after the original PlayLayer::init succeeds.
    void rendererDiagScan();

    // One sample per updateVisibility call, taken after the original runs. Cheap when
    // diagnostics are off (one cached bool and a return).
    void rendererDiagVisibility();

    std::uint64_t diagObjectCount();
    std::uint64_t diagSpriteCount();
    std::uint64_t diagHiddenCount();
    // Rolling average / peak of m_visibleObjectsCount (samples in groups of 300, then
    // reset); samples is the total window count since load.
    std::uint64_t diagVisibleAvg();
    std::uint64_t diagVisibleMax();
    std::uint64_t diagVisibleSamples();

}

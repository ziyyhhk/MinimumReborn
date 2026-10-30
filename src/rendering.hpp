#pragma once

// Rendering fast paths implemented by the mod. See rendering.cpp for the rationale and
// the exact conditions under which each draw is skipped.

namespace reborn {

    // Called once at the start of every CCDirector::drawScene call. Particle systems
    // created after this point count against the per-frame spawn budget.
    void particleFrameBegin();

    // Particle pool size that is actually applied right now (config cap, lowered by the
    // adaptive step if one is ever enabled; currently the configured cap).
    unsigned int effectiveParticleCap();
}

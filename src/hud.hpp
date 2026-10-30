#pragma once

namespace reborn {

    // Creates the performance overlay once the game has been running for a while. Does
    // nothing if it already exists. Called from the drawScene hook.
    void ensureHud(unsigned int totalFrames);
}

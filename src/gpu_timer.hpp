#pragma once

// GPU frame-time measurement via GL_TIME_ELAPSED. Windows only; no-op elsewhere.
// Begin/End bracket the engine's draw call; the result is read back the following frame
// with a non-blocking availability check, so the frame path never waits on the GPU.
// Never changes GL state beyond the query lifecycle.
namespace reborn {
    void gpuTimerBegin();
    void gpuTimerEnd();
}

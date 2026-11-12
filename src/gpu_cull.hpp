#pragma once

// GPU per-quad batch culling (experimental, opt-in, Windows only).
//
// The CPU batch filter (rendering.cpp, "Batch Object Filtering") scans every quad of a
// batched level draw on the main thread to decide which quads cannot produce a pixel.
// This module moves exactly the same decision to the GPU with a GL 4.3 compute shader:
//
//   * the atlas vertex VBO is bound as a read-only shader storage buffer - the same
//     vertices the original draw would upload, never a copy;
//   * each invocation projects one quad, drops it when it has zero area or when its four
//     projected corners lie fully outside the clip volume plus the same margin the CPU
//     filter uses, and otherwise appends its six indices to an output buffer while
//     atomically adding 6 to the index count inside an indirect-command buffer;
//   * the batch is then submitted with glDrawElementsIndirect, so the CPU never reads
//     the result back - if every quad is culled the command carries count 0 and the draw
//     is a no-op.
//
// Correctness rules are the same fail-open contract as the CPU filter: the caller runs
// the child updateTransform pass and all eligibility gates first; this module adds no
// visibility model of its own. Any doubt - GL below 4.3, a missing entry point, a shader
// that fails to build, or a non-screen-space projection - returns Unavailable and the
// caller falls back to the CPU filter (or the untouched draw). The level is drawn
// exactly once either way.

#include <Geode/Geode.hpp>
#include <Geode/cocos/textures/CCTextureAtlas.h>
#include <Geode/cocos/include/ccTypes.h>
#include <cstdint>

namespace reborn {

    enum class GpuCullResult {
        // Could not use the compute path (GL too old, missing entry points, shader
        // build failed, or the projection is not screen-space). Nothing was drawn and
        // no GL state is left bound; the caller must fall back to the CPU path. The
        // first sticky failure is latched for the process lifetime.
        Unavailable,
        // The indirect draw was submitted. kept may be zero - that is a valid,
        // complete cull, not an error.
        Submitted,
    };

    // True once the GL 4.3 path has been resolved and the compute program built.
    // Cached after the first call; cheap. Always false on non-Windows builds.
    bool gpuBatchCullAvailable();

    // Number of batch draws submitted through the compute path this session, and the
    // number of quads the GPU tested. Exposed for the overlay.
    std::uint64_t gpuCullBatchesSubmitted();
    std::uint64_t gpuCullQuadsScanned();

    // Draws the atlas's surviving quads through the indirect compute path.
    // `batchProgram` is the program the caller bound via CC_NODE_DRAW_SETUP(); it is
    // restored before the draw. `total` must be <= 16383 (the cap the CPU filter uses).
    // The atlas must already be uploaded - the caller shares the upload helper for both
    // paths. If Unavailable is returned, no GL state is left bound and nothing was
    // drawn; the caller should fall back to the CPU path.
    GpuCullResult drawBatchGpuCull(cocos2d::CCTextureAtlas* atlas, unsigned int total, GLuint batchProgram);

}

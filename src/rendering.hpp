#pragma once

#include <Geode/cocos/textures/CCTextureAtlas.h>

// Rendering fast paths implemented by the mod.
//
// This is the "optimized draw path": every function here can skip a draw submission, but
// none of them changes game state, node transforms, updates, physics or input. When the
// renderer mode is set to Engine (or the master switch is off) every hook falls straight
// through to the original cocos2d function.
//
// See rendering.cpp for the rationale, the exact skip conditions and the safety margins.

namespace reborn {

    // Called once at the start of every CCDirector::drawScene call, before any child is
    // visited. Takes the viewport snapshot used by all visibility tests, starts a new frame
    // id for the transform cache and starts this frame's particle spawn budget.
    void renderFrameBegin();

    // Called once at the end of every frame (drawn or skipped). Publishes the hot-path
    // counters that the draw hooks accumulate locally, so the per-object cost is an ordinary
    // integer increment and the atomic traffic is bounded to one pass per frame.
    void renderFrameEnd();

    // True when the current frame belongs to the level editor scene. Set once per frame.
    bool inEditor();

    // Particle pool size that is applied right now (the configured cap).
    unsigned int effectiveParticleCap();

    // True when at least one visibility test is active (renderer optimized + a cull mode).
    bool visibilityCullingActive();

    // Uploads the atlas vertex buffer (the exact range the original draw would upload) if
    // it is dirty, then clears the dirty flag. Shared by the CPU and GPU batch-filter paths.
    void uploadAtlasVerticesIfDirty(CCTextureAtlas* atlas, unsigned int total);

    // Enables PosColorTex vertex attributes and binds the interleaved ccV3F_C4B_T2F pointers
    // against the atlas vertex VBO. Shared by the CPU and GPU batch-filter paths.
    void setupAtlasVertexAttribs(CCTextureAtlas* atlas);

}

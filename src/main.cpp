#include <Geode/Geode.hpp>
#include "config.hpp"

using namespace geode::prelude;

// Minimum Reborn entry point.
//
// Everything the mod does is installed by Geode hooks (CCSprite, CCParticleSystemQuad,
// CCDirector, CCScheduler, PauseLayer). No hook is installed repeatedly and there is no
// background thread. Loading only snapshots the settings and applies the Windows system
// tuning; a failure in system tuning is harmless because every call is checked.

$on_mod(Loaded) {
    reborn::refreshConfig();
    auto const& cfg = reborn::config();
    log::info(
        "Minimum Reborn loaded: enabled={}, spriteFastPath={}, particles(idle={}, cull={}, cap={}/{}, budget={}), "
        "renderGate={} ({} fps), overlay={}, profile={}",
        cfg.enabled,
        cfg.spriteFastPath,
        cfg.skipIdleParticles,
        cfg.cullOffscreenParticles,
        cfg.capParticles,
        cfg.particleCap,
        cfg.spawnBudget,
        cfg.renderGate,
        static_cast<int>(cfg.renderGateFps),
        cfg.showOverlay,
        static_cast<int>(cfg.profile)
    );
}

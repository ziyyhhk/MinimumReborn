#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include "config.hpp"
#include "gpu_timer.hpp"

namespace reborn {

    namespace {
        // glew32.dll exports GLEW's function-pointer tables (double-underscore data symbols),
        // not an import library this toolchain can link, so the entry points come from
        // GetProcAddress at runtime - the same pattern ModImpl uses for the game's own
        // symbols. GD initializes GLEW at startup, which fills these in.
        using GenQueries = void (APIENTRY*)(unsigned int n, unsigned int* ids);
        using BeginQuery = void (APIENTRY*)(unsigned int target, unsigned int id);
        using EndQuery = void (APIENTRY*)(unsigned int target);
        using GetQueryObjectuiv = void (APIENTRY*)(unsigned int id, unsigned int pname, unsigned int* params);

        GenQueries p_genQueries = nullptr;
        BeginQuery p_beginQuery = nullptr;
        EndQuery p_endQuery = nullptr;
        GetQueryObjectuiv p_getQueryObjectuiv = nullptr;

        // cocos OGLES headers #define these GL constants, so the mod's own are renamed.
        unsigned int const kTimeElapsedQueryTarget = 0x88BF;
        unsigned int const kQueryResult = 0x87CB;
        unsigned int const kQueryResultAvailable = 0x8866;

        bool resolveGl() {
            if (p_genQueries) return true;
            HMODULE mod = GetModuleHandleA("glew32.dll");
            if (!mod) mod = LoadLibraryA("glew32.dll");
            if (!mod) return false;
            auto const genVar = reinterpret_cast<GenQueries*>(GetProcAddress(mod, "__glewGenQueries"));
            auto const beginVar = reinterpret_cast<BeginQuery*>(GetProcAddress(mod, "__glewBeginQuery"));
            auto const endVar = reinterpret_cast<EndQuery*>(GetProcAddress(mod, "__glewEndQuery"));
            auto const getObjVar = reinterpret_cast<GetQueryObjectuiv*>(GetProcAddress(mod, "__glewGetQueryObjectuiv"));
            if (!genVar || !beginVar || !endVar || !getObjVar) return false;
            // GLEW leaves these null when the context lacks the feature (anything below
            // GL 1.5). Null here means no GL query support: the timer stays off.
            if (!*genVar || !*beginVar || !*endVar || !*getObjVar) return false;
            p_genQueries = *genVar;
            p_beginQuery = *beginVar;
            p_endQuery = *endVar;
            p_getQueryObjectuiv = *getObjVar;
            return true;
        }

        unsigned int g_query = 0;
        bool g_initialized = false;
        bool g_queryInFlight = false;

        bool timerReady() {
            if (g_initialized) return g_query != 0;
            g_initialized = true;
            if (!resolveGl()) return false;
            p_genQueries(1, &g_query);
            return g_query != 0;
        }

        void collectResult() {
            if (!g_queryInFlight) return;
            // GL_QUERY_RESULT is a blocking read; ask for availability first so the frame
            // path never waits on the GPU.
            unsigned int available = 0;
            p_getQueryObjectuiv(g_query, kQueryResultAvailable, &available);
            if (!available) return;
            unsigned int nanoseconds = 0;
            p_getQueryObjectuiv(g_query, kQueryResult, &nanoseconds);
            g_queryInFlight = false;
            // A driver answer of zero or garbage never reaches the overlay.
            double const ms = static_cast<double>(nanoseconds) / 1e6;
            if (ms > 0.0 && ms < 1000.0) {
                reborn::counters().gpuFrameMs.store(static_cast<float>(ms));
            }
        }
    }

    void gpuTimerBegin() {
        auto const& cfg = config();
        if (!cfg.enabled || !cfg.gpuFrameTime) return;
        collectResult();
        if (!timerReady()) return;
        p_beginQuery(kTimeElapsedQueryTarget, g_query);
        g_queryInFlight = true;
    }

    void gpuTimerEnd() {
        if (!g_queryInFlight) return;
        p_endQuery(kTimeElapsedQueryTarget);
    }

}

#else

namespace reborn {
    void gpuTimerBegin() {}
    void gpuTimerEnd() {}
}

#endif

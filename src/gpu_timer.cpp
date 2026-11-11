#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
// The cocos OGLES header ships the same GLEW layout GD was built with: the entry points are
// plain calls through GLEW's function pointers, resolved by the driver layer.
#include <Geode/cocos/platform/third_party/win32/OGLES/GL/glew.h>
#include <algorithm>
#include "config.hpp"
#include "gpu_timer.hpp"

namespace reborn {

    namespace {
        // One reusable query. GL_TIME_ELAPSED is core GL 3.3, which GD 2.2081 requires on
        // Windows; if it is ever unavailable no query exists and every call is a no-op.
        GLuint g_query = 0;
        bool g_initialized = false;
        bool g_queryInFlight = false;

        bool timerReady() {
            if (g_initialized) return g_query != 0;
            g_initialized = true;
            glewInit();
            if (glewIsSupported("GL_ARB_timer_query") || GLEW_VERSION_3_3) {
                glGenQueries(1, &g_query);
            }
            return g_query != 0;
        }

        void collectResult() {
            if (!g_queryInFlight) return;
            // GL_QUERY_RESULT is a blocking read; ask for availability first so the frame
            // path never waits on the GPU.
            GLuint available = 0;
            glGetQueryObjectuiv(g_query, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) return;
            GLuint nanoseconds = 0;
            glGetQueryObjectuiv(g_query, GL_QUERY_RESULT, &nanoseconds);
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
        glBeginQuery(GL_TIME_ELAPSED, g_query);
        g_queryInFlight = true;
    }

    void gpuTimerEnd() {
        if (!g_queryInFlight) return;
        glEndQuery(GL_TIME_ELAPSED);
    }

}

#else

namespace reborn {
    void gpuTimerBegin() {}
    void gpuTimerEnd() {}
}

#endif

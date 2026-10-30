#include <Geode/Geode.hpp>
#include "config.hpp"

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

using namespace geode::prelude;

// System tuning (Windows only; no-op elsewhere).
//
// These are frame-pacing fixes that are not the game's fault:
//   * 1 ms timer resolution: by default Windows wakes sleeping threads every ~15.6 ms, so a
//     frame that waits for the next vblank can oversleep. timeBeginPeriod(1) asks for 1 ms
//     wakeups (per process on Windows 10 2004+). Undone when the setting is switched off.
//   * Power throttling opt-out: Windows can silently put a background-flagged process on
//     efficiency cores / lower clocks (EcoQoS). The process opts out.
//   * Process priority: optional. Normal by default; High can starve OBS/Discord, so it is
//     never applied automatically.
//
// Every call is only made when the wanted state differs from the applied state, so this is
// safe to call after each config refresh.

namespace reborn {

#ifdef GEODE_IS_WINDOWS

    namespace {

        // Same layout as PROCESS_POWER_THROTTLING_STATE, declared locally so the build does
        // not depend on the Windows SDK version the runner happens to have.
        struct PowerThrottlingState {
            ULONG version;
            ULONG controlMask;
            ULONG stateMask;
        };

        constexpr ULONG kPowerThrottlingVersion = 1;
        constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;
        constexpr int kProcessPowerThrottling = 4; // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling

        using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

        bool g_timerApplied = false;
        bool g_throttleOptOutApplied = false;
        Priority g_priorityApplied = Priority::Normal;

        void setPowerThrottlingOptOut(bool optOut) {
            HMODULE kernel = GetModuleHandleA("kernel32.dll");
            if (!kernel) return;
            auto fn = reinterpret_cast<SetProcessInformationFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation"))
            );
            if (!fn) return; // Older than Windows 8: nothing to do.

            PowerThrottlingState state{};
            state.version = kPowerThrottlingVersion;
            if (optOut) {
                state.controlMask = kPowerThrottlingExecutionSpeed;
                state.stateMask = 0;
            }
            fn(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
        }

        DWORD priorityClassFor(Priority p) {
            switch (p) {
                case Priority::AboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
                case Priority::High: return HIGH_PRIORITY_CLASS;
                default: return NORMAL_PRIORITY_CLASS;
            }
        }
    }

    void applySystemTuning() {
        auto const& cfg = config();

        bool const wantTimer = cfg.enabled && cfg.timerResolution;
        if (wantTimer != g_timerApplied) {
            if (wantTimer) {
                if (timeBeginPeriod(1) == TIMERR_NOERROR) g_timerApplied = true;
            }
            else {
                timeEndPeriod(1);
                g_timerApplied = false;
            }
        }

        bool const wantOptOut = cfg.enabled && cfg.disablePowerThrottling;
        if (wantOptOut != g_throttleOptOutApplied) {
            setPowerThrottlingOptOut(wantOptOut);
            g_throttleOptOutApplied = wantOptOut;
        }

        Priority const wantPriority = cfg.enabled ? cfg.priority : Priority::Normal;
        if (wantPriority != g_priorityApplied) {
            if (SetPriorityClass(GetCurrentProcess(), priorityClassFor(wantPriority))) {
                g_priorityApplied = wantPriority;
            }
        }
    }

#else

    void applySystemTuning() {}

#endif
}

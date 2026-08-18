#pragma once

#include "clipture/MediaClock.hpp"

#include <Windows.h>
#include <timeapi.h>
#include <immintrin.h>

#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stop_token>

namespace clipture {

// High-resolution precision sleep matching OBS Studio's os_sleepto_ns on Windows.
// Combines CREATE_WAITABLE_TIMER_HIGH_RESOLUTION with a short spin/yield loop
// to eliminate the 1.5ms - 4.0ms oversleep jitter inherent in standard Windows sleep.
class PrecisionTimer {
public:
    PrecisionTimer() {
        timeBeginPeriod(1);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!timer_) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
    }

    ~PrecisionTimer() {
        if (timer_) {
            CloseHandle(timer_);
            timer_ = nullptr;
        }
        timeEndPeriod(1);
    }

    PrecisionTimer(const PrecisionTimer&) = delete;
    PrecisionTimer& operator=(const PrecisionTimer&) = delete;

    // Sleep until target monotonic QPC timestamp (in 100-nanosecond units)
    bool sleepTo100ns(int64_t targetTime100ns, std::stop_token stopToken = {}) {
        int64_t now100ns = monotonicNow100ns();
        int64_t remaining100ns = targetTime100ns - now100ns;
        if (remaining100ns <= 0) return !stopToken.stop_requested();

        // 1.5 milliseconds in 100-nanosecond units
        constexpr int64_t kSpinLead100ns = 15'000LL;

        if (remaining100ns > kSpinLead100ns && timer_) {
            // Wake up kSpinLead100ns before the target deadline
            const int64_t coarseWait100ns = remaining100ns - kSpinLead100ns;
            LARGE_INTEGER dueTime {};
            // Negative value indicates relative time in 100-nanosecond units
            dueTime.QuadPart = -coarseWait100ns;
            if (SetWaitableTimer(timer_, &dueTime, 0, nullptr, nullptr, FALSE)) {
                constexpr DWORD kSliceMs = 20;
                while (coarseWait100ns > 0 && !stopToken.stop_requested()) {
                    const DWORD waitResult = WaitForSingleObject(timer_, kSliceMs);
                    if (waitResult == WAIT_OBJECT_0) break;
                    if (waitResult == WAIT_TIMEOUT) {
                        now100ns = monotonicNow100ns();
                        if (targetTime100ns - now100ns <= kSpinLead100ns) break;
                    } else {
                        break;
                    }
                }
            }
        }

        if (stopToken.stop_requested()) return false;

        // Finish the final sub-1.5ms with a precision spin-yield loop
        while (!stopToken.stop_requested()) {
            now100ns = monotonicNow100ns();
            if (now100ns >= targetTime100ns) break;
            const int64_t gap100ns = targetTime100ns - now100ns;
            if (gap100ns > 5'000LL) {
                // If more than 0.5ms remains, yield time slice to pending threads
                SwitchToThread();
            } else {
                // Final 500 microseconds: CPU pause instructions
                _mm_pause();
            }
        }

        return !stopToken.stop_requested();
    }

    // Sleep until a std::chrono::steady_clock time point
    bool sleepUntil(std::chrono::steady_clock::time_point targetTime, std::stop_token stopToken = {}) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= targetTime) return !stopToken.stop_requested();
        const auto remainingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(targetTime - now).count();
        const int64_t targetTime100ns = monotonicNow100ns() + (remainingNs / 100);
        return sleepTo100ns(targetTime100ns, stopToken);
    }

private:
    HANDLE timer_ = nullptr;
};

}  // namespace clipture

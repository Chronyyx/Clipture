#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

// Desktop Duplication reports every DWM update, often 2-4x the recording rate.
// Each acquired update costs a full-frame GPU copy (FP16 when HDR is on), and
// all but the newest before an encoder tick are discarded. Acquiring only in a
// short window before the tick returns the same newest image, since DXGI
// accumulates updates while nothing is acquired, without the discarded copies.
inline constexpr int64_t kTickAlignedAcquireLead100ns = 20'000;     // 2 ms
inline constexpr int64_t kTickAlignedAcquireMaxWait100ns = 500'000;  // re-check cadence changes

// Returns the monotonic time to wait for before acquiring, or 0 to acquire now.
inline int64_t tickAlignedAcquireNotBefore(int64_t now100ns, int64_t nextDeadline100ns, int64_t lead100ns) {
    if (nextDeadline100ns <= 0) return 0;
    const int64_t windowStart = nextDeadline100ns - lead100ns;
    if (now100ns >= windowStart) return 0;
    return std::min(windowStart, now100ns + kTickAlignedAcquireMaxWait100ns);
}

}  // namespace clipture

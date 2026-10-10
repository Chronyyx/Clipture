#pragma once

#include <cstdint>

namespace clipture {

// A late cadence wake encodes every due tick back to back. Taking the newest
// frame for each repeats it on the following ticks and pulls newer footage
// early; a late tick instead takes the newest frame at or before its own
// deadline. On-time ticks (scheduler jitter only) keep taking the newest frame.
inline constexpr int64_t kTimedSelectionLateness100ns = 20'000; // 2 ms

inline constexpr bool selectByDeadline(bool moreTicksDue, int64_t lateness100ns) {
    return moreTicksDue || lateness100ns > kTimedSelectionLateness100ns;
}

}  // namespace clipture

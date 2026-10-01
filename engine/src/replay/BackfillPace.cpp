#include "clipture/replay/BackfillPace.hpp"
#include <algorithm>

namespace clipture::replay {
void BackfillPace::recordSave(Clock::time_point now) {
    if (lastSave_ && now > *lastSave_) {
        const double interval = std::chrono::duration<double>(now - *lastSave_).count();
        // Lean toward the latest gap so a quick re-save speeds up the very next
        // backfill, while one long pause does not fully forget a rapid habit.
        expectedInterval_ = expectedInterval_ ? (*expectedInterval_ + interval) / 2 : interval;
    }
    lastSave_ = now;
}

double BackfillPace::multiplier(double windowSeconds) const {
    if (!expectedInterval_ || windowSeconds <= 0) return baseMultiplier;
    // Copying newest-first while the oldest footage expires, a window W at k x
    // live rate finishes after W / (k + 1). Aim to finish at 80% of the gap.
    const double target = std::max(0.001, *expectedInterval_ * 0.8);
    return std::clamp(windowSeconds / target - 1.0, baseMultiplier, maximumMultiplier);
}
}

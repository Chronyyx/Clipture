#pragma once
#include <chrono>
#include <optional>

namespace clipture::replay {
// Overlap backfill speed, as a multiple of the rate footage arrives. It starts
// just above live (a passive trickle) and speeds up only for people who save
// again soon after a clip, so the leftover finishes before their usual next save.
class BackfillPace {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr double baseMultiplier = 1.1;
    static constexpr double maximumMultiplier = 8.0;
    // A save that arrives before backfill finishes copies the rest itself,
    // faster than backfill (someone is waiting) but never at full drive speed.
    static constexpr double saveCopyMultiplier = 16.0;

    void recordSave(Clock::time_point now);
    // windowSeconds: retained footage the saved clip still holds.
    double multiplier(double windowSeconds) const;
    std::optional<double> expectedIntervalSeconds() const { return expectedInterval_; }

private:
    std::optional<Clock::time_point> lastSave_;
    std::optional<double> expectedInterval_;
};
}

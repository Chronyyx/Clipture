#pragma once

#include <optional>
#include <string>
#include <vector>

namespace clipture::stress {

struct StressPhase {
    std::string name;
    double seconds = 0;
    // Extra GPU work per frame, in milliseconds of calibrated load.
    double loadMs = 0;
};

// Ordered phases parsed from "name:seconds:loadMs,..." (see README.md).
class StressTimeline {
public:
    static std::optional<StressTimeline> parse(const std::string& text, std::string& error);

    const std::vector<StressPhase>& phases() const { return phases_; }
    double totalSeconds() const { return totalSeconds_; }
    // Index of the phase active at `elapsed` seconds, or -1 once the timeline ended.
    int phaseAt(double elapsed) const;

private:
    std::vector<StressPhase> phases_;
    double totalSeconds_ = 0;
};

inline constexpr const char* kDefaultTimeline =
    "clear:4:0,smoke:6:6,clear:3:0,molly:4:10,clear:2:0,spike:1:20,clear:3:0";

}  // namespace clipture::stress

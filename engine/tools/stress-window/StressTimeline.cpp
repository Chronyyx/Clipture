#include "StressTimeline.hpp"

#include <cstdlib>
#include <sstream>

namespace clipture::stress {

namespace {

bool parseNumber(const std::string& text, double& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

}  // namespace

std::optional<StressTimeline> StressTimeline::parse(const std::string& text, std::string& error) {
    StressTimeline timeline;
    std::stringstream entries(text);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        const auto first = entry.find(':');
        const auto second = first == std::string::npos ? first : entry.find(':', first + 1);
        StressPhase phase;
        if (second == std::string::npos || first == 0 ||
            !parseNumber(entry.substr(first + 1, second - first - 1), phase.seconds) ||
            !parseNumber(entry.substr(second + 1), phase.loadMs)) {
            error = "Expected name:seconds:loadMs, got '" + entry + "'";
            return std::nullopt;
        }
        phase.name = entry.substr(0, first);
        if (phase.seconds <= 0 || phase.seconds > 120 || phase.loadMs < 0 || phase.loadMs > 100) {
            error = "Phase '" + phase.name + "' needs 0-120 seconds and 0-100 loadMs";
            return std::nullopt;
        }
        timeline.totalSeconds_ += phase.seconds;
        timeline.phases_.push_back(std::move(phase));
    }
    if (timeline.phases_.empty() || timeline.totalSeconds_ > 300) {
        error = "Timeline needs at least one phase and at most 300 seconds";
        return std::nullopt;
    }
    return timeline;
}

int StressTimeline::phaseAt(double elapsed) const {
    double end = 0;
    for (size_t i = 0; i < phases_.size(); ++i) {
        end += phases_[i].seconds;
        if (elapsed < end) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace clipture::stress

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace clipture {

struct VideoTimelineStep {
    int64_t pts100ns = 0;
    int64_t dueTicks = 1;
    int64_t skippedTicks = 0;
};

inline uint32_t finalVideoSampleDuration100ns(int64_t packetDuration100ns, int fps) {
    const int64_t duration = packetDuration100ns > 0
        ? packetDuration100ns
        : 10'000'000LL / std::max(1, fps);
    return static_cast<uint32_t>(std::clamp<int64_t>(
        duration,
        1,
        std::numeric_limits<uint32_t>::max()));
}

class VideoTimeline {
public:
    VideoTimeline(int64_t firstPts100ns, int fps) {
        reset(firstPts100ns, fps);
    }

    void reset(int64_t firstPts100ns, int fps) {
        frameSpacing100ns_ = 10'000'000LL / std::max(1, fps);
        nextPts100ns_ = firstPts100ns;
    }

    VideoTimelineStep advance(int64_t lateness100ns, int64_t maximumCatchUpTicks = 8) {
        const int64_t elapsedTicks = 1 + std::max<int64_t>(0, lateness100ns) /
            std::max<int64_t>(1, frameSpacing100ns_);
        const int64_t dueTicks = std::clamp<int64_t>(elapsedTicks, 1, std::max<int64_t>(1, maximumCatchUpTicks));
        const int64_t skippedTicks = elapsedTicks - dueTicks;
        const int64_t firstOutputPts100ns = nextPts100ns_ + skippedTicks * frameSpacing100ns_;
        nextPts100ns_ += elapsedTicks * frameSpacing100ns_;
        return { firstOutputPts100ns, dueTicks, skippedTicks };
    }

    int64_t frameSpacing100ns() const {
        return frameSpacing100ns_;
    }

private:
    int64_t frameSpacing100ns_ = 10'000'000LL / 30;
    int64_t nextPts100ns_ = 0;
};

}  // namespace clipture

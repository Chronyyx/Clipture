#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace clipture {

inline uint32_t videoSampleDuration100ns(int64_t pts100ns, int64_t nextPts100ns) {
    const int64_t duration = nextPts100ns > pts100ns
        ? nextPts100ns - pts100ns
        : 1;
    return static_cast<uint32_t>(std::clamp<int64_t>(
        duration,
        1,
        std::numeric_limits<uint32_t>::max()));
}

inline uint32_t finalVideoSampleDuration100ns(int64_t packetDuration100ns, int fps) {
    const int64_t duration = packetDuration100ns > 0
        ? packetDuration100ns
        : 10'000'000LL / std::max(1, fps);
    return static_cast<uint32_t>(std::clamp<int64_t>(
        duration,
        1,
        std::numeric_limits<uint32_t>::max()));
}

}  // namespace clipture

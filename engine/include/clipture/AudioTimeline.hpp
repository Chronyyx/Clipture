#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

inline void alignAudioPtsForwardOnly(
    int64_t capturedPts100ns,
    int64_t packetDuration100ns,
    bool& clockAnchored,
    int64_t& nextPts100ns) {
    if (capturedPts100ns <= 0) return;
    if (!clockAnchored) {
        nextPts100ns = std::max(nextPts100ns, capturedPts100ns);
        clockAnchored = true;
        return;
    }

    const int64_t correctionThreshold100ns = std::max<int64_t>(200'000, packetDuration100ns * 2);
    if (capturedPts100ns > nextPts100ns + correctionThreshold100ns) {
        nextPts100ns = capturedPts100ns;
    }
}

}  // namespace clipture

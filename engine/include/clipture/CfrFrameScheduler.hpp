#pragma once

#include <cstdint>
#include <limits>

namespace clipture {

struct CfrFrameRun {
    uint64_t sourceSequence = 0;
    uint64_t captureEpoch = 0;
    int64_t firstPts100ns = 0;
    int64_t frameSpacing100ns = 0;
    uint32_t tickCount = 1;
    bool beginsWithFreshSource = true;
};

constexpr bool cfrRunCanAppend(
    const CfrFrameRun& run,
    uint64_t sourceSequence,
    uint64_t captureEpoch,
    int64_t pts100ns,
    int64_t frameSpacing100ns) {
    if (run.tickCount == 0 || run.tickCount == std::numeric_limits<uint32_t>::max()) return false;
    if (sourceSequence == 0 || run.sourceSequence != sourceSequence || run.captureEpoch != captureEpoch) {
        return false;
    }
    if (run.frameSpacing100ns <= 0 || run.frameSpacing100ns != frameSpacing100ns) return false;
    return pts100ns == run.firstPts100ns +
        static_cast<int64_t>(run.tickCount) * run.frameSpacing100ns;
}

constexpr uint32_t cfrFreshTickCount(const CfrFrameRun& run) {
    return run.tickCount > 0 && run.beginsWithFreshSource ? 1u : 0u;
}

constexpr uint32_t cfrRepeatTickCount(const CfrFrameRun& run) {
    return run.tickCount - cfrFreshTickCount(run);
}

constexpr bool shouldScheduleCfrTick(
    uint64_t sourceSequence,
    uint64_t lastScheduledSourceSequence,
    bool enableStillFrameDuplication) {
    return enableStillFrameDuplication ||
        sourceSequence == 0 ||
        sourceSequence != lastScheduledSourceSequence;
}

}  // namespace clipture

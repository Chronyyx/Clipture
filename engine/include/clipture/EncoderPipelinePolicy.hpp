#pragma once

#include "clipture/CfrFrameScheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

namespace clipture {

constexpr std::size_t boundedNvencSyncOutputDelay(
    std::size_t outputSlotCount,
    std::size_t preparedSubmissionDepth,
    std::size_t preferredDelay,
    std::size_t reservedSlotCount = 2) {
    if (outputSlotCount == 0) return 0;

    const std::size_t boundedPreparedDepth =
        std::min(preparedSubmissionDepth, outputSlotCount - 1);
    const std::size_t unavailableSlots = boundedPreparedDepth + reservedSlotCount;
    if (unavailableSlots >= outputSlotCount) return 1;

    const std::size_t maximumSafeDelay = outputSlotCount - unavailableSlots;
    return std::clamp(preferredDelay, std::size_t { 1 }, maximumSafeDelay);
}

constexpr bool shouldDrainNvencSyncOutput(
    std::size_t inFlightCount,
    std::size_t configuredDelay) {
    return inFlightCount > 0 &&
        (configuredDelay == 0 || inFlightCount >= configuredDelay);
}

inline constexpr std::size_t noEncoderQueueEviction =
    std::numeric_limits<std::size_t>::max();

constexpr std::size_t preferredEncoderQueueEvictionIndex(
    std::span<const CfrFrameRun> pendingRuns) {
    if (pendingRuns.empty()) return noEncoderQueueEviction;

    for (std::size_t index = 0; index < pendingRuns.size(); ++index) {
        if (cfrFreshTickCount(pendingRuns[index]) == 0) return index;
    }
    return 0;
}

}  // namespace clipture

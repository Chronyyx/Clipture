#pragma once

#include <algorithm>
#include <cstddef>

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

// When true, the encoder ticks at fixed CFR intervals and holds/duplicates the last frame on idle ticks.
// When false, the encoder only encodes when a fresh, unique frame arrives from capture (zero duplication).
inline constexpr bool kEnableFrameDuplication = true;

}  // namespace clipture

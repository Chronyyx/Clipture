#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace clipture {

enum class EncoderQueueAdmission {
    Enqueue,
    CoalesceRepeat,
    WaitForRoom,
};

constexpr EncoderQueueAdmission encoderQueueAdmission(
    std::size_t pendingJobs,
    std::size_t capacity,
    bool repeatsSourceFrame,
    bool matchingSourceAlreadyQueued) {
    if (capacity == 0) return EncoderQueueAdmission::WaitForRoom;
    constexpr std::size_t freshFrameReserve = 2;
    const std::size_t repeatLimit = capacity > freshFrameReserve
        ? capacity - freshFrameReserve
        : 0;
    if (repeatsSourceFrame &&
        (matchingSourceAlreadyQueued || pendingJobs >= repeatLimit)) {
        return EncoderQueueAdmission::CoalesceRepeat;
    }
    return pendingJobs < capacity
        ? EncoderQueueAdmission::Enqueue
        : EncoderQueueAdmission::WaitForRoom;
}

inline std::chrono::milliseconds encoderQueueWaitBudget() {
    return std::chrono::milliseconds(1);
}

}  // namespace clipture

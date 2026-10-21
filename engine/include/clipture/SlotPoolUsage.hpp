#pragma once

#include <atomic>
#include <cstdint>

namespace clipture {

// Size of a GPU slot pool that grows under load and frees idle slots, for
// diagnostics: how many slots exist now, the most that existed at once, and
// how many were freed for sitting idle.
struct SlotPoolUsage {
    std::atomic<int> allocated = 0;
    std::atomic<int> peak = 0;
    std::atomic<uint64_t> idleReleases = 0;

    void observe(int count) {
        allocated.store(count, std::memory_order_relaxed);
        int previous = peak.load(std::memory_order_relaxed);
        while (count > previous && !peak.compare_exchange_weak(previous, count, std::memory_order_relaxed)) {}
    }
};

}  // namespace clipture

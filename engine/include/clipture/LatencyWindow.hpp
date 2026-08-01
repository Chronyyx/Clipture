#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace clipture {

struct LatencyWindowSnapshot {
    std::size_t samples = 0;
    int64_t average100ns = 0;
    int64_t p50_100ns = 0;
    int64_t p95_100ns = 0;
    int64_t maximum100ns = 0;
};

// A bounded recent-sample window. Recording is allocation-free; allocations
// only occur when diagnostics request percentile data.
template <std::size_t Capacity = 512>
class LatencyWindow {
public:
    static_assert(Capacity > 0);

    void record(int64_t timestamp100ns, int64_t latency100ns) {
        if (timestamp100ns <= 0 || latency100ns < 0) return;
        std::lock_guard lock(mutex_);
        samples_[next_] = { timestamp100ns, latency100ns };
        next_ = (next_ + 1) % Capacity;
        count_ = std::min(count_ + 1, Capacity);
    }

    LatencyWindowSnapshot snapshot(
        int64_t now100ns,
        int64_t horizon100ns = 50'000'000LL) const {
        std::vector<int64_t> values;
        values.reserve(Capacity);
        const int64_t oldestAllowed100ns = now100ns > horizon100ns
            ? now100ns - horizon100ns
            : 0;
        {
            std::lock_guard lock(mutex_);
            for (std::size_t index = 0; index < count_; ++index) {
                const auto& sample = samples_[index];
                if (sample.timestamp100ns < oldestAllowed100ns ||
                    sample.timestamp100ns > now100ns) {
                    continue;
                }
                values.push_back(sample.latency100ns);
            }
        }
        if (values.empty()) return {};

        std::sort(values.begin(), values.end());
        int64_t total100ns = 0;
        for (const int64_t value : values) total100ns += value;
        const auto percentile = [&values](std::size_t numerator) {
            const std::size_t index = std::min(
                values.size() - 1,
                ((values.size() - 1) * numerator + 50) / 100);
            return values[index];
        };
        return {
            values.size(),
            total100ns / static_cast<int64_t>(values.size()),
            percentile(50),
            percentile(95),
            values.back(),
        };
    }

    void clear() {
        std::lock_guard lock(mutex_);
        next_ = 0;
        count_ = 0;
    }

private:
    struct Sample {
        int64_t timestamp100ns = 0;
        int64_t latency100ns = 0;
    };

    mutable std::mutex mutex_;
    std::array<Sample, Capacity> samples_ {};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

}  // namespace clipture

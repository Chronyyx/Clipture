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

// A bounded recent-sample window. Neither recording nor reading allocates,
// and a read holds the lock the recording threads share only while it walks
// the samples inside its horizon, not the whole window.
template <std::size_t Capacity = 2048>
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
        int64_t horizon100ns = 10'000'000LL) const {
        std::array<int64_t, Capacity> values;
        std::size_t count = 0;
        visitRecent(now100ns, horizon100ns, [&](int64_t latency100ns) { values[count++] = latency100ns; });
        if (count == 0) return {};

        const auto end = values.begin() + static_cast<std::ptrdiff_t>(count);
        std::sort(values.begin(), end);
        int64_t total100ns = 0;
        for (auto value = values.begin(); value != end; ++value) total100ns += *value;
        const auto percentile = [&values, count](std::size_t numerator) {
            const std::size_t index = std::min(
                count - 1,
                ((count - 1) * numerator + 50) / 100);
            return values[index];
        };
        return {
            count,
            total100ns / static_cast<int64_t>(count),
            percentile(50),
            percentile(95),
            values[count - 1],
        };
    }

    // How many samples lie inside the horizon; for windows that only count
    // events.
    std::size_t recentCount(
        int64_t now100ns,
        int64_t horizon100ns = 10'000'000LL) const {
        std::size_t count = 0;
        visitRecent(now100ns, horizon100ns, [&](int64_t) { ++count; });
        return count;
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

    // Newest first. Samples are recorded in time order, give or take the
    // threads that record them, so one further horizon back is far enough to
    // stop looking.
    template <typename Visitor>
    void visitRecent(int64_t now100ns, int64_t horizon100ns, Visitor&& visit) const {
        const int64_t oldestAllowed100ns = now100ns > horizon100ns
            ? now100ns - horizon100ns
            : 0;
        const int64_t stopBefore100ns = oldestAllowed100ns > horizon100ns
            ? oldestAllowed100ns - horizon100ns
            : 0;
        std::lock_guard lock(mutex_);
        for (std::size_t back = 1; back <= count_; ++back) {
            const auto& sample = samples_[(next_ + Capacity - back) % Capacity];
            if (sample.timestamp100ns < stopBefore100ns) break;
            if (sample.timestamp100ns < oldestAllowed100ns ||
                sample.timestamp100ns > now100ns) {
                continue;
            }
            visit(sample.latency100ns);
        }
    }

    mutable std::mutex mutex_;
    std::array<Sample, Capacity> samples_ {};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

}  // namespace clipture

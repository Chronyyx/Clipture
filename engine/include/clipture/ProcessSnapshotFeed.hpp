#pragma once

#include "clipture/ProcessSnapshot.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace clipture {

// The newest list of running processes, taken once and shared. Listing every
// process costs milliseconds; the detection loop publishes the list it takes
// each pass, and audio capture threads waiting for their process to start read
// that list instead of each taking their own.
class ProcessSnapshotFeed {
public:
    using Snapshot = std::shared_ptr<const RunningProcessSnapshot>;

    Snapshot publish(RunningProcessSnapshot snapshot) {
        auto shared = std::make_shared<const RunningProcessSnapshot>(std::move(snapshot));
        {
            std::lock_guard lock(mutex_);
            latest_ = shared;
            ++generation_;
        }
        changed_.notify_all();
        return shared;
    }

    // Counts published lists; pass it to waitForNewer() to get only later ones.
    std::uint64_t generation() const {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    // Waits for a list published after `generation` and returns it, taken
    // moments ago. Null when none came within `timeout` or `cancelled` turned
    // true (see wake()).
    Snapshot waitForNewer(
        std::uint64_t& generation,
        std::chrono::milliseconds timeout,
        const std::function<bool()>& cancelled) const {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return generation_ > generation || cancelled(); });
        if (generation_ <= generation || cancelled()) return nullptr;
        generation = generation_;
        return latest_;
    }

    // Makes waiters test `cancelled` again.
    void wake() const {
        { std::lock_guard lock(mutex_); }
        changed_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    Snapshot latest_;
    std::uint64_t generation_ = 0;
};

}  // namespace clipture

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace clipture {

// Where encoded video ends: the presentation end (pts + duration) of the
// newest frame out of the encoder. A save waits on it briefly, so the frames
// still inside the encoder when the hotkey was pressed make it into the clip
// instead of the last finished frame being held until that moment.
class EncodedFrontier {
public:
    void advance(int64_t end100ns) {
        {
            std::lock_guard lock(mutex_);
            if (end100ns <= end100ns_) return;
            end100ns_ = end100ns;
        }
        changed_.notify_all();
    }

    // True once a frame reaching `end100ns` is encoded; false if `limit`
    // passed first (a stalled or stopped encoder never blocks a save longer).
    bool waitUntil(int64_t end100ns, std::chrono::milliseconds limit) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return end100ns_ >= end100ns; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    int64_t end100ns_ = 0;
};

}  // namespace clipture

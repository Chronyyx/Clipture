#include "clipture/FrameQueue.hpp"
#include "clipture/MediaClock.hpp"

#include <algorithm>
#include <climits>

namespace clipture {

FrameQueue::FrameQueue(std::size_t capacity)
    : capacity_(capacity) {}

void FrameQueue::push(CapturedFrame frame) {
    if (frame.queuedAtSteady100ns <= 0) frame.queuedAtSteady100ns = monotonicNow100ns();
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        ++stats_.pushedFrames;
        while (frames_.size() >= capacity_) {
            frames_.pop_front();
            ++stats_.overflowDrops;
        }
        frames_.push_back(std::move(frame));
        stats_.maximumDepth = std::max(stats_.maximumDepth, frames_.size());
    }
    cv_.notify_one();
}

std::optional<CapturedFrame> FrameQueue::waitPop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return stopped_ || !frames_.empty(); });
    if (frames_.empty()) return std::nullopt;
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}
std::optional<CapturedFrame> FrameQueue::waitPopFor(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return stopped_ || !frames_.empty(); });
    if (frames_.empty()) return std::nullopt;
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

std::optional<CapturedFrame> FrameQueue::consumeAllAndGetLatest() {
    std::lock_guard lock(mutex_);
    if (frames_.empty()) return std::nullopt;
    if (frames_.size() > 1) stats_.coalescedDrops += frames_.size() - 1;
    auto frame = std::move(frames_.back());
    frames_.clear();
    return frame;
}

std::optional<CapturedFrame> FrameQueue::consumeLatestAtOrBefore(int64_t pts100ns) {
    std::lock_guard lock(mutex_);
    std::optional<CapturedFrame> latest;
    std::size_t consumed = 0;
    while (!frames_.empty() && frames_.front().pts100ns <= pts100ns) {
        latest = std::move(frames_.front());
        frames_.pop_front();
        ++consumed;
    }
    if (consumed > 1) stats_.coalescedDrops += consumed - 1;
    return latest;
}

void FrameQueue::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
}

void FrameQueue::clear() {
    std::lock_guard lock(mutex_);
    stats_.clearedFrames += frames_.size();
    frames_.clear();
}

std::size_t FrameQueue::size() const {
    std::lock_guard lock(mutex_);
    return frames_.size();
}

int FrameQueue::droppedFrames() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(std::min<uint64_t>(
        stats_.overflowDrops + stats_.coalescedDrops,
        static_cast<uint64_t>(INT_MAX)));
}

int64_t FrameQueue::oldestFrameAge100ns() const {
    std::lock_guard lock(mutex_);
    if (frames_.empty() || frames_.front().queuedAtSteady100ns <= 0) return 0;
    const int64_t now100ns = monotonicNow100ns();
    return std::max<int64_t>(0, now100ns - frames_.front().queuedAtSteady100ns);
}

FrameQueueStats FrameQueue::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace clipture

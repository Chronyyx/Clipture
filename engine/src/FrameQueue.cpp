#include "clipture/FrameQueue.hpp"

#include <chrono>

namespace clipture {

FrameQueue::FrameQueue(std::size_t capacity)
    : capacity_(capacity) {}

void FrameQueue::push(CapturedFrame frame) {
    frame.queuedAtSteady100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        while (frames_.size() >= capacity_) {
            frames_.pop_front();
            ++droppedFrames_;
        }
        frames_.push_back(std::move(frame));
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

std::optional<CapturedFrame> FrameQueue::consumeAllAndGetLatest() {
    std::lock_guard lock(mutex_);
    if (frames_.empty()) return std::nullopt;
    auto frame = std::move(frames_.back());
    frames_.clear();
    return frame;
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
    frames_.clear();
}

std::size_t FrameQueue::size() const {
    std::lock_guard lock(mutex_);
    return frames_.size();
}

int FrameQueue::droppedFrames() const {
    std::lock_guard lock(mutex_);
    return droppedFrames_;
}

int64_t FrameQueue::oldestFrameAge100ns() const {
    std::lock_guard lock(mutex_);
    if (frames_.empty() || frames_.front().queuedAtSteady100ns <= 0) return 0;
    const int64_t now100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    return std::max<int64_t>(0, now100ns - frames_.front().queuedAtSteady100ns);
}

}  // namespace clipture

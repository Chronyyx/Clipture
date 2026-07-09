#include "clipture/FrameQueue.hpp"

namespace clipture {

FrameQueue::FrameQueue(std::size_t capacity)
    : capacity_(capacity) {}

void FrameQueue::push(CapturedFrame frame) {
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

}  // namespace clipture

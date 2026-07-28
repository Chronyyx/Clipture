#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace clipture {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<void> textureLease;
    int64_t pts100ns = 0;
    int width = 0;
    int height = 0;
    int64_t queuedAtSteady100ns = 0;
    uint64_t captureEpoch = 0;
    uint64_t sequence = 0;
};

struct FrameQueueStats {
    uint64_t pushedFrames = 0;
    uint64_t overflowDrops = 0;
    uint64_t coalescedDrops = 0;
    uint64_t clearedFrames = 0;
    std::size_t maximumDepth = 0;
};

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity = 8);

    void push(CapturedFrame frame);
    std::optional<CapturedFrame> waitPop();
    std::optional<CapturedFrame> consumeAllAndGetLatest();
    std::optional<CapturedFrame> consumeLatestAtOrBefore(int64_t pts100ns);
    void stop();
    void clear();

    std::size_t size() const;
    int droppedFrames() const;
    int64_t oldestFrameAge100ns() const;
    FrameQueueStats stats() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CapturedFrame> frames_;
    bool stopped_ = false;
    FrameQueueStats stats_;
};

}  // namespace clipture

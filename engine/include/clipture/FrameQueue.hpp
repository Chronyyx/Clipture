#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace clipture {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    int64_t pts100ns = 0;
    int width = 0;
    int height = 0;
};

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity = 8);

    void push(CapturedFrame frame);
    std::optional<CapturedFrame> waitPop();
    std::optional<CapturedFrame> consumeAllAndGetLatest();
    void stop();
    void clear();

    std::size_t size() const;
    int droppedFrames() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CapturedFrame> frames_;
    bool stopped_ = false;
    int droppedFrames_ = 0;
};

}  // namespace clipture

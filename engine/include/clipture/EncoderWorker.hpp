#pragma once

#include "clipture/FrameQueue.hpp"
#include "clipture/PacketRingBuffer.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace clipture {

class EncoderWorker {
public:
    EncoderWorker(FrameQueue& frames, PacketRingBuffer& packets);
    ~EncoderWorker();

    EncoderWorker(const EncoderWorker&) = delete;
    EncoderWorker& operator=(const EncoderWorker&) = delete;

    void start();
    void stop();
    void configure(int fps, int bitrateMbps, int targetWidth, int targetHeight, int maxEncodeWidth, int maxEncodeHeight, int nvencPreset);
    void requireFreshFrame();
    void resetAutoOutputResolution();

    bool running() const;
    bool nvencRuntimeLoaded() const;
    int framesAccepted() const;
    int framesEncoded() const;
    int pendingFrames() const;
    int sourceWidth() const;
    int sourceHeight() const;
    int outputWidth() const;
    int outputHeight() const;
    bool scalingActive() const;
    std::string status() const;

private:
    void run();

    FrameQueue& frames_;
    PacketRingBuffer& packets_;
    std::thread thread_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> nvencRuntimeLoaded_ = false;
    std::atomic<int> framesAccepted_ = 0;
    std::atomic<int> framesEncoded_ = 0;
    std::atomic<int> targetFps_ = 30;
    std::atomic<int> targetBitrateMbps_ = 40;
    std::atomic<int> targetWidth_ = 0;
    std::atomic<int> targetHeight_ = 0;
    std::atomic<int> maxEncodeWidth_ = 0;
    std::atomic<int> maxEncodeHeight_ = 0;
    std::atomic<int> autoOutputWidth_ = 0;
    std::atomic<int> autoOutputHeight_ = 0;
    std::atomic<int> sourceWidth_ = 0;
    std::atomic<int> sourceHeight_ = 0;
    std::atomic<int> outputWidth_ = 0;
    std::atomic<int> outputHeight_ = 0;
    std::atomic<bool> scalingActive_ = false;
    std::atomic<int> nvencPreset_ = 3;
    std::atomic<int> configVersion_ = 0;
    std::atomic<int> freshFrameVersion_ = 0;
    std::string status_ = "Encoder worker has not started.";
};

}  // namespace clipture

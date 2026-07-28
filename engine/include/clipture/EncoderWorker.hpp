#pragma once

#include "clipture/FrameQueue.hpp"
#include "clipture/PacketRingBuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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
    void configure(
        int fps,
        int bitrateMbps,
        int targetWidth,
        int targetHeight,
        int maxEncodeWidth,
        int maxEncodeHeight,
        int nvencPreset,
        bool discardBufferedPackets = false);
    void requireFreshFrame(bool discardBufferedPackets = false);
    void resetAutoOutputResolution();

    bool running() const;
    bool nvencRuntimeLoaded() const;
    int framesAccepted() const;
    int framesEncoded() const;
    int pendingFrames() const;
    int schedulerDroppedFrames() const;
    int encoderBackpressureDrops() const;
    int nvencInFlightFrames() const;
    int64_t maximumSubmitLatency100ns() const;
    int sourceWidth() const;
    int sourceHeight() const;
    int outputWidth() const;
    int outputHeight() const;
    bool scalingActive() const;
    std::string status() const;

private:
    struct EncodeJob {
        CapturedFrame frame;
        int fps = 30;
        int bitrateMbps = 40;
        int targetWidth = 0;
        int targetHeight = 0;
        int maxEncodeWidth = 0;
        int maxEncodeHeight = 0;
        int nvencPreset = 3;
        int configVersion = 0;
        int freshFrameVersion = 0;
    };

    void run();
    void encodeLoop();
    void queueLatestJob(EncodeJob job);
    void setStatus(std::string status);

    FrameQueue& frames_;
    PacketRingBuffer& packets_;
    std::thread thread_;
    std::thread encodeThread_;
    mutable std::mutex submitMutex_;
    std::condition_variable submitCv_;
    std::optional<EncodeJob> pendingJob_;
    mutable std::mutex statusMutex_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> nvencRuntimeLoaded_ = false;
    std::atomic<int> framesAccepted_ = 0;
    std::atomic<int> framesEncoded_ = 0;
    std::atomic<int> schedulerDroppedFrames_ = 0;
    std::atomic<int> encoderBackpressureDrops_ = 0;
    std::atomic<int> nvencInFlightFrames_ = 0;
    std::atomic<int64_t> maximumSubmitLatency100ns_ = 0;
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
    std::atomic<int> discardPacketsAtConfigVersion_ = 0;
    std::atomic<int> freshFrameVersion_ = 0;
    std::atomic<int> discardPacketsAtFreshFrameVersion_ = 0;
    std::string status_ = "Encoder worker has not started.";
};

}  // namespace clipture

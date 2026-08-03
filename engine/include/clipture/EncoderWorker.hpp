#pragma once

#include "clipture/CfrFrameScheduler.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/LatencyWindow.hpp"
#include "clipture/PacketRingBuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace clipture {

class ReplaySegmentStore;

struct EncoderRecentPerformance {
    double inputFps = 0.0;
    double outputFps = 0.0;
    LatencyWindowSnapshot inputPreparation;
    LatencyWindowSnapshot inputMap;
    LatencyWindowSnapshot encodeCall;
    LatencyWindowSnapshot outputEventWait;
    LatencyWindowSnapshot outputLock;
    LatencyWindowSnapshot outputCopy;
    LatencyWindowSnapshot outputUnmap;
};

class EncoderWorker {
public:
    EncoderWorker(
        FrameQueue& frames,
        PacketRingBuffer& packets,
        ReplaySegmentStore* replayStore = nullptr);
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
    int queuedEncodeFrames() const;
    int queuedFreshEncodeFrames() const;
    int queuedRepeatEncodeFrames() const;
    int schedulerDroppedFrames() const;
    int schedulerRepeatedFrames() const;
    int encoderQueueDrops() const;
    int encoderRepeatCoalesced() const;
    int nvencSurfaceDrops() const;
    int nvencInputDrops() const;
    int encoderBackpressureDrops() const;
    int nvencInFlightFrames() const;
    uint64_t nvencZeroCopyFrames() const;
    uint64_t nvencCopyFallbackFrames() const;
    uint64_t nvencConvertedFrames() const;
    int effectiveNvencPreset() const;
    int64_t maximumSubmitLatency100ns() const;
    int64_t averageScaleLatency100ns() const;
    int64_t maximumScaleLatency100ns() const;
    int64_t averageInputMapLatency100ns() const;
    int64_t maximumInputMapLatency100ns() const;
    int64_t averageNvencCallLatency100ns() const;
    int64_t maximumNvencCallLatency100ns() const;
    int64_t averageOutputDrainLatency100ns() const;
    int64_t maximumOutputDrainLatency100ns() const;
    EncoderRecentPerformance recentPerformance() const;
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
        CfrFrameRun run;
    };

    void run();
    void encodeLoop();
    bool queueTick(EncodeJob job);
    void setStatus(std::string status);

    FrameQueue& frames_;
    PacketRingBuffer& packets_;
    ReplaySegmentStore* replayStore_ = nullptr;
    std::thread thread_;
    std::thread encodeThread_;
    mutable std::mutex submitMutex_;
    std::condition_variable submitCv_;
    std::deque<EncodeJob> pendingJobs_;
    std::size_t pendingOutputTicks_ = 0;
    std::size_t pendingFreshTicks_ = 0;
    std::size_t pendingRepeatTicks_ = 0;
    mutable std::mutex statusMutex_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> nvencRuntimeLoaded_ = false;
    std::atomic<int> framesAccepted_ = 0;
    std::atomic<int> framesEncoded_ = 0;
    std::atomic<int> schedulerDroppedFrames_ = 0;
    std::atomic<int> schedulerRepeatedFrames_ = 0;
    std::atomic<int> encoderQueueDrops_ = 0;
    std::atomic<int> encoderRepeatCoalesced_ = 0;
    std::atomic<int> nvencSurfaceDrops_ = 0;
    std::atomic<int> nvencInputDrops_ = 0;
    std::atomic<int> encoderBackpressureDrops_ = 0;
    std::atomic<int> nvencInFlightFrames_ = 0;
    std::atomic<int> nvencPreparedFrames_ = 0;
    std::atomic<uint64_t> nvencZeroCopyFrames_ = 0;
    std::atomic<uint64_t> nvencCopyFallbackFrames_ = 0;
    std::atomic<uint64_t> nvencConvertedFrames_ = 0;
    std::atomic<int64_t> maximumSubmitLatency100ns_ = 0;
    std::atomic<uint64_t> profiledSubmissions_ = 0;
    std::atomic<int64_t> totalScaleLatency100ns_ = 0;
    std::atomic<int64_t> maximumScaleLatency100ns_ = 0;
    std::atomic<int64_t> totalInputMapLatency100ns_ = 0;
    std::atomic<int64_t> maximumInputMapLatency100ns_ = 0;
    std::atomic<int64_t> totalNvencCallLatency100ns_ = 0;
    std::atomic<int64_t> maximumNvencCallLatency100ns_ = 0;
    std::atomic<int64_t> totalOutputDrainLatency100ns_ = 0;
    std::atomic<int64_t> maximumOutputDrainLatency100ns_ = 0;
    std::atomic<int64_t> performanceStarted100ns_ = 0;
    LatencyWindow<> recentInputPreparationLatency_;
    LatencyWindow<> recentInputMapLatency_;
    LatencyWindow<> recentNvencCallLatency_;
    LatencyWindow<> recentOutputEventWaitLatency_;
    LatencyWindow<> recentOutputLockLatency_;
    LatencyWindow<> recentOutputCopyLatency_;
    LatencyWindow<> recentOutputUnmapLatency_;
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
    static constexpr std::size_t maximumPendingRuns_ = 8;
    std::string status_ = "Encoder worker has not started.";
};

}  // namespace clipture

#pragma once

#include <memory>
#include <cstdint>
#include <string>

namespace clipture {

class FrameQueue;

struct CaptureRuntimeStats {
    std::string requestedBackend = "auto";
    std::string activeBackend = "none";
    std::string fallbackReason;
    uint32_t refreshNumerator = 0;
    uint32_t refreshDenominator = 1;
    double refreshHz = 0.0;
    uint64_t acquiredUpdates = 0;
    uint64_t desktopPresents = 0;
    uint64_t pointerUpdates = 0;
    uint64_t publishedFrames = 0;
    uint64_t accumulatedFrames = 0;
    uint64_t accumulationEvents = 0;
    uint64_t samplerRejections = 0;
    uint64_t nonMonotonicTimestamps = 0;
    uint64_t acquireTimeouts = 0;
    uint64_t accessLosses = 0;
    uint64_t recreationAttempts = 0;
    uint64_t recreationSuccesses = 0;
    uint64_t fallbackCount = 0;
    double desktopPresentFps = 0.0;
    double publishedFreshFps = 0.0;
};

class CaptureSession {
public:
    CaptureSession();
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    bool startMonitor(FrameQueue* frameQueue, const std::string& monitorId);
    void setTargetFps(int fps);
    void stop();

    bool running() const;
    int capturedFrames() const;
    int64_t lastFrameInterval100ns() const;
    int64_t maximumFrameInterval100ns() const;
    int64_t lastPublishedAge100ns() const;
    uint64_t ownedSlotDrops() const;
    uint64_t sourceFramesSuperseded() const;
    uint64_t callbackErrors() const;
    uint64_t captureEpoch() const;
    std::string resolution() const;
    std::string displayName() const;
    bool hdrTonemappingActive() const;
    std::string status() const;
    void* activeMonitor() const;
    CaptureRuntimeStats runtimeStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture

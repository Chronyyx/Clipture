#pragma once

#include <memory>
#include <string>

namespace clipture {

class FrameQueue;

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
    std::string resolution() const;
    std::string displayName() const;
    bool hdrTonemappingActive() const;
    std::string status() const;
    void* activeMonitor() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture

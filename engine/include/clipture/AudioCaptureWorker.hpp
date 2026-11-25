#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace clipture {

class AudioReplayCoordinator;
class ProcessSnapshotFeed;

class AudioCaptureWorker {
public:
    // `processFeed`: where a capture thread waiting for its process to start
    // reads the process list; without one each waiting thread lists the
    // processes itself.
    explicit AudioCaptureWorker(
        PacketRingBuffer& packets,
        AudioReplayCoordinator* replayCoordinator = nullptr,
        ProcessSnapshotFeed* processFeed = nullptr);
    ~AudioCaptureWorker();

    AudioCaptureWorker(const AudioCaptureWorker&) = delete;
    AudioCaptureWorker& operator=(const AudioCaptureWorker&) = delete;

    void start(bool startConfiguredApps = true);
    void startConfiguredAppSources();
    void stop();
    void configureAppSources(const std::vector<std::string>& processNames);
    void setMicSettings(float volume, bool isolation, float isolationWeight, bool noiseGateEnabled, bool autoNoiseGate, float noiseGateThreshold, int noiseGateDebounceMs, const std::string& micDeviceId, const std::string& micDeviceMatchKey, const std::string& micDeviceName);

    bool running() const;
    // Captures system loopback and the microphone only while a track records
    // them; callable before start() and while running.
    void setCoreSources(bool systemAudio, bool microphone);
    int packetsCaptured() const;
    std::string status() const;
    std::string microphoneStatus() const;

private:
    struct AppCaptureThread {
        std::string processName;
        std::shared_ptr<std::atomic<bool>> stopRequested;
        std::thread thread;
    };

    void runCapture(bool loopback, const std::string& sourceId, std::shared_ptr<std::atomic<bool>> stopRequested);
    void runMicrophoneCapture();
    void runDeviceNotificationLoop();
    void signalMicRefresh();
    void requestMicRefresh();
    // Wakes the microphone thread waiting for a device. Takes its lock, so a
    // wake between its check and its wait is not lost: it waits without a
    // timeout.
    void wakeMicThread();
    void runProcessLoopbackCapture(const std::string& processName, std::shared_ptr<std::atomic<bool>> stopRequested);
    void runProcessLoopbackCaptureSession(
        const std::string& captureName,
        const std::string& sourceId,
        std::uint32_t processId,
        std::shared_ptr<std::atomic<bool>> stopRequested);
    void startAppCaptureThreadLocked(const std::string& processName);
    // Sleeps an app capture thread; false once it should stop.
    bool waitUnlessStopping(const std::atomic<bool>& stopRequested, std::chrono::milliseconds duration);
    // Wakes app capture threads that sleep or wait for a process list.
    void wakeAppCaptureThreads();
    void startMicThreadLocked();
    void stopMicThread();
    void startSystemThreadLocked();
    void stopSystemThread();
    void publishPacket(EncodedPacket packet);

    PacketRingBuffer& packets_;
    AudioReplayCoordinator* replayCoordinator_ = nullptr;
    ProcessSnapshotFeed* processFeed_ = nullptr;
    std::mutex appWaitMutex_;
    std::condition_variable appWaitCv_;
    std::vector<std::thread> coreThreads_;
    std::thread micThread_;
    std::thread systemThread_;
    std::shared_ptr<std::atomic<bool>> systemStopRequested_;
    bool wantSystem_ = true;
    bool wantMic_ = true;
    std::thread notificationThread_;
    std::shared_ptr<std::atomic<bool>> micStopRequested_;
    std::map<std::string, AppCaptureThread> appThreads_;
    std::vector<std::string> appProcessNames_;
    std::string micDeviceId_;
    std::string micDeviceMatchKey_;
    std::string micDeviceName_;
    std::string effectiveMicDeviceId_;
    std::string microphoneStatus_ = "System default";
    mutable std::mutex configMutex_;
    std::mutex notificationMutex_;
    std::condition_variable notificationCv_;
    std::mutex micRefreshMutex_;
    std::condition_variable micRefreshCv_;
    std::atomic<float> micVolume_ = 1.0f;
    std::atomic<bool> micIsolation_ = false;
    std::atomic<float> micIsolationWeight_ = 1.0f;
    std::atomic<bool> noiseGateEnabled_ = true;
    std::atomic<bool> autoNoiseGate_ = true;
    std::atomic<float> noiseGateThreshold_ = 0.05f;
    std::atomic<int> noiseGateDebounceMs_ = 180;
    std::atomic<bool> running_ = false;
    bool appCaptureEnabled_ = true;
    std::atomic<bool> notificationPending_ = false;
    std::atomic<int> micRefreshVersion_ = 0;
    std::atomic<int> packetsCaptured_ = 0;
    std::string status_ = "Audio capture has not started.";
};

}  // namespace clipture

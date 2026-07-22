#pragma once

#include "clipture/AudioCaptureWorker.hpp"
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/Diagnostics.hpp"
#include "clipture/EncoderWorker.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/PacketRingBuffer.hpp"

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <deque>
#include <mutex>

namespace clipture {

struct SaveClipRequest {
    int durationSeconds = 30;
    std::string saveFolder;
};

struct EngineSettings {
    int fps = 30;
    int bitrateMbps = 40;
    int nvencPreset = 3;
    int clipLengthSeconds = 30;
    std::string monitorId = "primary";
    int targetWidth = 0;
    int targetHeight = 0;
    bool includeMixedAudio = true;
    bool includeSystemAudio = true;
    bool includeMicrophoneAudio = true;
    bool captureGameAudio = false;
    bool captureForegroundSystemAudio = false;
    float micVolume = 1.0f;
    bool micIsolation = false;
    float micIsolationWeight = 0.5f;
    bool noiseGateEnabled = true;
    bool autoNoiseGate = true;
    float noiseGateThreshold = 0.05f;
    int noiseGateDebounceMs = 180;
    std::string micDeviceId;
    std::string micDeviceMatchKey;
    std::string micDeviceName;
    std::vector<std::string> appAudioProcesses;
    std::vector<std::string> systemAudioProcesses;
};

struct SaveClipResult {
    bool ok = false;
    std::string message;
    std::string clipJson;
};

class Engine {
public:
    Engine();
    ~Engine();

    const Diagnostics& diagnostics();
    const Diagnostics& configure(const EngineSettings& settings);
    SaveClipResult saveClip(const SaveClipRequest& request);

private:
    void arm();
    void refreshPacketCounts();
    void maybeResetAutoVideoResolution();
    void refreshAudioRouting();

    void gameDetectionLoop();

    EngineSettings settings_;
    Diagnostics diagnostics_;
    FrameQueue frameQueue_;
    PacketRingBuffer videoPackets_;
    PacketRingBuffer audioPackets_;
    PacketRingBuffer aacAudioPackets_;
    std::unique_ptr<AudioReplayCoordinator> audioReplayCoordinator_;
    std::unique_ptr<CaptureSession> captureSession_;
    std::unique_ptr<EncoderWorker> encoderWorker_;
    std::unique_ptr<AudioCaptureWorker> audioCaptureWorker_;
    std::vector<std::string> foregroundSystemProcesses_;
    std::mutex appAudioSourcesMutex_;
    std::vector<std::string> activeAppAudioSources_;
    std::map<std::string, std::string> activeAppAudioTrackAliases_;
    int64_t pendingAutoVideoResolutionReset100ns_ = 0;
    
    std::atomic<bool> gameDetectionRunning_{false};
    std::thread gameDetectionThread_;
    
    struct ForegroundHistoryEntry {
        std::chrono::steady_clock::time_point timestamp;
        std::string appName;
        bool isGame;
    };
    std::mutex historyMutex_;
    std::deque<ForegroundHistoryEntry> foregroundHistory_;
};

}  // namespace clipture

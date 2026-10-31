#pragma once

#include "clipture/AudioCaptureWorker.hpp"
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/CaptureTickGate.hpp"
#include "clipture/Diagnostics.hpp"
#include "clipture/EncoderWorker.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/GpuPriorityControl.hpp"
#include "clipture/PacketRingBuffer.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/ConsumedWindow.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace clipture {

struct SaveClipRequest {
    int durationSeconds = 30;
    std::string saveFolder;
    bool analyzeIo = false;
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
    bool saveInPlace = true;
    std::string saveFolder;
    // Save hotkey in games that run as administrator, through the relay the
    // user allowed (ADR 0024).
    bool adminGameHotkey = false;
    // Schedule the engine's GPU work ahead of the game (GpuPriorityControl).
    bool gpuPriorityBoost = true;
};

struct SaveClipResult {
    bool ok = false;
    std::string message;
    std::string clipJson;
    std::string ioAnalysisJson;
};

namespace platform { class AdminHotkeyRelay; }

class Engine {
public:
    Engine();
    ~Engine();

    // Game detection reports admin games to the relay; set before configure().
    void setAdminHotkeyRelay(platform::AdminHotkeyRelay* relay) { adminHotkeyRelay_ = relay; }

    const Diagnostics& diagnostics();
    const Diagnostics& configure(const EngineSettings& settings);
    std::string runningProcessesJson(bool includeExecutablePaths) const;
    std::string processExecutablePathJson(uint32_t processId) const;
    SaveClipResult saveClip(const SaveClipRequest& request);

private:
    void arm();
    void refreshPacketCounts();
    void maybeResetAutoVideoResolution();
    void refreshAudioRouting();

    void gameDetectionLoop();

    EngineSettings settings_;
    Diagnostics diagnostics_;
    GpuPriorityControl gpuPriority_;
    platform::AdminHotkeyRelay* adminHotkeyRelay_ = nullptr;
    CaptureTickGate captureTickGate_;
    FrameQueue frameQueue_;
    PacketRingBuffer videoPackets_;
    PacketRingBuffer audioPackets_;
    PacketRingBuffer aacAudioPackets_;
    std::unique_ptr<ReplaySegmentStore> videoReplayStore_;
    std::unique_ptr<ReplaySegmentStore> aacReplayStore_;
    std::unique_ptr<ReplaySegmentStore> pcmRecoveryStore_;
    std::shared_ptr<replay::InPlacePacketArchive> inPlaceArchive_ = std::make_shared<replay::InPlacePacketArchive>();
    replay::ConsumedWindow consumedWindow_;
    // Save writer rate learned by the last paced save into this folder.
    std::string learnedSaveWriteFolder_;
    uint64_t learnedSaveWriteBytesPerSecond_ = 0;
    std::unique_ptr<AudioReplayCoordinator> audioReplayCoordinator_;
    std::unique_ptr<CaptureSession> captureSession_;
    std::unique_ptr<EncoderWorker> encoderWorker_;
    std::unique_ptr<AudioCaptureWorker> audioCaptureWorker_;
    std::vector<std::string> foregroundSystemProcesses_;
    // Apps with a live audio session, each recorded on its own track; touched
    // only by the detection loop.
    struct AutoAppSession {
        std::string exeName;
        std::chrono::steady_clock::time_point lastActive;
    };
    std::map<unsigned long, AutoAppSession> autoAppSessions_; // by process ID
    std::mutex appAudioSourcesMutex_;
    std::vector<std::string> activeAppAudioSources_;
    std::map<std::string, std::string> activeAppAudioTrackAliases_;
    int64_t pendingAutoVideoResolutionReset100ns_ = 0;
    int64_t nextCaptureRestart100ns_ = 0;
    
    std::atomic<bool> gameDetectionRunning_{false};
    std::thread gameDetectionThread_;
    std::mutex gameDetectionWaitMutex_;
    std::condition_variable gameDetectionWaitCv_;
    bool armed_ = false;
    
    struct ForegroundHistoryEntry {
        std::chrono::steady_clock::time_point timestamp;
        std::string appName;
        bool isGame;
    };
    std::mutex historyMutex_;
    std::deque<ForegroundHistoryEntry> foregroundHistory_;
    void* activeGameCaptureWindow_ = nullptr;
    void* hybridGameWindow_ = nullptr;
};

}  // namespace clipture

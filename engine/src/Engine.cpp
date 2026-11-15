#include "clipture/Engine.hpp"
#include "clipture/ProcessVerdictCache.hpp"
#include "clipture/replay/BackfillPace.hpp"
#include "clipture/platform/ThreadName.hpp"
#include "clipture/platform/windows/AdminHotkeyRelay.hpp"
#include "clipture/platform/windows/ProcessElevation.hpp"
#include "clipture/AudioProcessSpec.hpp"
#include "clipture/CapturePipelinePolicy.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/NativeRuntime.hpp"
#include "clipture/ProcessSnapshot.hpp"
#include "clipture/VideoCadenceAnalysis.hpp"

#include <Windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <regex>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include <thread>
#include <atomic>

#include <map>
#include <mutex>
#include <set>
#include <span>

namespace clipture {
#include "EngineSupport.inl"  // helpers: games, paths, JSON, audio routing
Engine::Engine()
    : diagnostics_(collectDiagnostics()),
      frameQueue_(20),
      videoPackets_(5LL * 60LL * 10'000'000LL, 16u * 1024u * 1024u),
      audioPackets_(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u),
      aacAudioPackets_(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u),
      videoReplayStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("video", true))),
      aacReplayStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("audio", false))),
      pcmRecoveryStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("pcm-recovery", false))),
      audioReplayCoordinator_(std::make_unique<AudioReplayCoordinator>(
          audioPackets_,
          aacAudioPackets_,
          aacReplayStore_.get(),
          pcmRecoveryStore_.get())),
      captureSession_(std::make_unique<CaptureSession>()),
      encoderWorker_(std::make_unique<EncoderWorker>(
          frameQueue_,
          videoPackets_,
          videoReplayStore_.get())),
      audioCaptureWorker_(std::make_unique<AudioCaptureWorker>(
          audioPackets_,
          audioReplayCoordinator_.get(),
          &processSnapshotFeed_)) {
    // NVENC allocates at setup; never let that happen at realtime.
    encoderWorker_->setSetupObserver([this] { gpuPriority_.encoderSetupStarting(); });
}

Engine::~Engine() {
    gameDetectionRunning_ = false;
    gameDetectionWaitCv_.notify_all();
    if (gameDetectionThread_.joinable()) {
        gameDetectionThread_.join();
    }
    audioSessionWatcher_.reset();
    if (audioCaptureWorker_) audioCaptureWorker_->stop();
    if (audioReplayCoordinator_) audioReplayCoordinator_->stop();
    if (captureSession_) captureSession_->stop();
    if (encoderWorker_) encoderWorker_->stop();
    if (pcmRecoveryStore_) pcmRecoveryStore_->stop();
    if (aacReplayStore_) aacReplayStore_->stop();
    if (videoReplayStore_) videoReplayStore_->stop();
}

void Engine::refreshAudioRouting() {
    if (!audioReplayCoordinator_) return;
    std::map<std::string, std::string> routes;
    if (settings_.includeSystemAudio) routes["system-loopback-pcm"] = "system-loopback-pcm";
    if (settings_.includeMicrophoneAudio) routes["microphone-pcm"] = "microphone-pcm";

    std::vector<std::string> activeSources;
    std::map<std::string, std::string> aliases;
    {
        std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
        activeSources = activeAppAudioSources_;
        aliases = activeAppAudioTrackAliases_;
    }
    if (activeSources.empty()) {
        activeSources = settings_.appAudioProcesses;
        for (const auto& systemProcess : settings_.systemAudioProcesses) {
            if (!containsProcessName(activeSources, systemProcess)) activeSources.push_back(systemProcess);
            aliases[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
        }
    }
    for (const auto& sourceSpec : activeSources) {
        const auto sourceId = sourceIdForProcessSpec(sourceSpec);
        const auto alias = aliases.find(sourceId);
        if (alias != aliases.end()) {
            routes[sourceId] = alias->second;
        } else if (sourceId.rfind("game:", 0) == 0 && settings_.captureGameAudio) {
            routes[sourceId] = "game:" + getGameName(sourceId.substr(5));
        } else if (sourceId.rfind("app:", 0) == 0) {
            routes[sourceId] = sourceId;
        }
    }
    audioReplayCoordinator_->updateRouting(std::move(routes));
}

// The foreground game's name when it runs as administrator, which keeps this
// engine's hotkey from seeing its keys. A full-screen window this engine may
// not even query (anti-cheat) counts too, named by its title.
static std::string foregroundAdminGame(const ForegroundProcess& foreground, const std::string& name, bool isGame) {
    const HWND window = foreground.window;
    if (!window || foreground.processId == 0 || !platform::runsAboveCurrentProcess(foreground.processId)) return {};
    if (isGame) return name;
    RECT bounds {};
    MONITORINFO monitor { sizeof(monitor) };
    if (!GetWindowRect(window, &bounds) || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
        bounds.left > monitor.rcMonitor.left || bounds.top > monitor.rcMonitor.top ||
        bounds.right < monitor.rcMonitor.right || bounds.bottom < monitor.rcMonitor.bottom) {
        return {};
    }
    wchar_t title[128] {};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    const auto text = narrowWide(title);
    return text.empty() ? std::string("A game") : text;
}

#include "EngineDetection.inl"  // game and audio-source detection loop
std::vector<DWORD> Engine::soundingAudioProcessIds() const {
    if (!audioSessionWatcher_ || !audioSessionWatcher_->ready()) return activeAudioSessionProcessIds();
    const auto processIds = audioSessionWatcher_->soundingProcessIds();
    return { processIds.begin(), processIds.end() };
}

void Engine::onAudioSessionsChanged() {
    {
        std::lock_guard lock(gameDetectionWaitMutex_);
        audioSessionsChanged_ = true;
    }
    gameDetectionWaitCv_.notify_all();
}

std::string Engine::progressJson() {
    std::ostringstream json;
    json << "{\"engineRunning\":" << (diagnostics_.engineRunning ? "true" : "false")
         << ",\"encoderOutputPackets\":" << (encoderWorker_ ? encoderWorker_->framesEncoded() : 0)
         << ",\"encoderAcceptedFrames\":" << (encoderWorker_ ? encoderWorker_->framesAccepted() : 0)
         << ",\"nvencInFlightFrames\":" << (encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0)
         << ",\"queuedFrames\":" << frameQueue_.size()
         << ",\"gpuSchedulingClass\":\"" << jsonEscape(gpuPriority_.schedulingClass()) << "\"}";
    return json.str();
}

const Diagnostics& Engine::diagnostics(bool sampleGpuUsage) {
    refreshPacketCounts();
    gpuPriority_.publish(diagnostics_);
    gpuPriority_.refreshUsage(diagnostics_, sampleGpuUsage);
    if (adminHotkeyRelay_) {
        diagnostics_.adminGame = adminHotkeyRelay_->elevatedGame();
        diagnostics_.adminGameInFront = adminHotkeyRelay_->gameInFront();
        diagnostics_.adminHotkeyRelay = adminHotkeyRelay_->status();
        diagnostics_.adminHotkeySetup = adminHotkeyRelay_->setup();
    }
    return diagnostics_;
}

std::string Engine::runningProcessesJson(bool includeExecutablePaths) const {
    const auto snapshot = RunningProcessSnapshot::captureNameOnly();
    std::ostringstream json;
    json << "[";
    bool wroteProcess = false;
    for (const auto& process : snapshot.entries()) {
        if (process.exeName.empty() || process.processId == 0) continue;
        if (wroteProcess) json << ",";
        wroteProcess = true;
        json << "{"
             << "\"name\":\"" << jsonEscape(process.exeName) << "\","
             << "\"pid\":" << process.processId;
        if (includeExecutablePaths) {
            json << ",\"executablePath\":\""
                 << jsonEscape(RunningProcessSnapshot::queryProcessPath(process.processId)) << "\"";
        }
        json << "}";
    }
    json << "]";
    return json.str();
}

std::string Engine::processExecutablePathJson(uint32_t processId) const {
    return "\"" + jsonEscape(RunningProcessSnapshot::queryProcessPath(processId)) + "\"";
}

#include "EngineConfigure.inl"  // configure
#include "EngineSave.inl"  // saveClip
#include "EngineCapture.inl"  // arm and packet upkeep
}  // namespace clipture

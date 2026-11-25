#include "clipture/AudioCaptureWorker.hpp"
#include "clipture/platform/ThreadName.hpp"
#include "clipture/AudioProcessSpec.hpp"
#include "clipture/AudioTimeline.hpp"
#include "clipture/LoopbackSilencePolicy.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/PcmSampleConverter.hpp"
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/ProcessSnapshotFeed.hpp"

#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <wrl/implements.h>
#include <wrl/client.h>

#include "rnnoise.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "AudioCaptureShared.hpp"
namespace clipture {
AudioCaptureWorker::AudioCaptureWorker(
    PacketRingBuffer& packets,
    AudioReplayCoordinator* replayCoordinator,
    ProcessSnapshotFeed* processFeed)
    : packets_(packets), replayCoordinator_(replayCoordinator), processFeed_(processFeed) {}

bool AudioCaptureWorker::waitUnlessStopping(
    const std::atomic<bool>& stopRequested,
    std::chrono::milliseconds duration) {
    std::unique_lock lock(appWaitMutex_);
    return !appWaitCv_.wait_for(lock, duration, [&] { return !running_.load() || stopRequested.load(); });
}

void AudioCaptureWorker::wakeAppCaptureThreads() {
    // Through the mutex, so a thread about to sleep cannot miss the wake.
    { std::lock_guard lock(appWaitMutex_); }
    appWaitCv_.notify_all();
    if (processFeed_) processFeed_->wake();
}

AudioCaptureWorker::~AudioCaptureWorker() {
    stop();
}

void AudioCaptureWorker::start(bool startConfiguredApps) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard lock(configMutex_);
        // A source no track records is not captured (its packets were dropped).
        if (wantSystem_) startSystemThreadLocked();
        if (wantMic_) startMicThreadLocked();
    }
    notificationThread_ = std::thread(&AudioCaptureWorker::runDeviceNotificationLoop, this);
    std::vector<std::string> appProcessNames;
    {
        std::lock_guard lock(configMutex_);
        appCaptureEnabled_ = startConfiguredApps;
        appProcessNames = appProcessNames_;
        if (appCaptureEnabled_) {
            for (const auto& processName : appProcessNames) {
                startAppCaptureThreadLocked(processName);
            }
        }
    }
    if (appProcessNames.empty()) {
        std::cerr << "[audio] No app audio sources configured." << std::endl;
    }
}

void AudioCaptureWorker::startConfiguredAppSources() {
    std::lock_guard lock(configMutex_);
    if (!running_ || appCaptureEnabled_) return;
    appCaptureEnabled_ = true;
    for (const auto& processName : appProcessNames_) {
        startAppCaptureThreadLocked(processName);
    }
}

void AudioCaptureWorker::stop() {
    if (!running_.exchange(false)) return;
    notificationPending_.store(true);
    notificationCv_.notify_all();
    wakeMicThread();
    stopMicThread();
    stopSystemThread();
    std::vector<std::thread> appThreadsToJoin;
    {
        std::lock_guard lock(configMutex_);
        appCaptureEnabled_ = false;
        for (auto& [_, appThread] : appThreads_) {
            if (appThread.stopRequested) appThread.stopRequested->store(true);
            if (appThread.thread.joinable()) {
                appThreadsToJoin.push_back(std::move(appThread.thread));
            }
        }
        appThreads_.clear();
    }
    wakeAppCaptureThreads();
    for (auto& thread : coreThreads_) {
        if (thread.joinable()) thread.join();
    }
    coreThreads_.clear();
    if (notificationThread_.joinable()) notificationThread_.join();
    for (auto& thread : appThreadsToJoin) {
        if (thread.joinable()) thread.join();
    }
}

bool AudioCaptureWorker::running() const {
    return running_;
}

void AudioCaptureWorker::configureAppSources(const std::vector<std::string>& processNames) {
    // Filter out empty or whitespace-only entries.
    std::vector<std::string> filtered;
    for (const auto& name : processNames) {
        bool allWhitespace = true;
        for (char ch : name) {
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                allWhitespace = false;
                break;
            }
        }
        if (!name.empty() && !allWhitespace) {
            filtered.push_back(name);
        }
    }
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());

    std::vector<std::thread> removedThreads;
    std::vector<std::string> added;
    {
        std::lock_guard lock(configMutex_);
        if (filtered == appProcessNames_) return;

        std::vector<std::string> removed;
        std::set_difference(
            appProcessNames_.begin(),
            appProcessNames_.end(),
            filtered.begin(),
            filtered.end(),
            std::back_inserter(removed));
        std::set_difference(
            filtered.begin(),
            filtered.end(),
            appProcessNames_.begin(),
            appProcessNames_.end(),
            std::back_inserter(added));

        appProcessNames_ = filtered;
        std::cerr << "[audio] App audio sources changed:";
        for (const auto& name : appProcessNames_) std::cerr << " " << name;
        std::cerr << (appProcessNames_.empty() ? " (none)" : "") << std::endl;

        if (running_ && appCaptureEnabled_) {
            for (const auto& processName : removed) {
                auto it = appThreads_.find(processName);
                if (it == appThreads_.end()) continue;
                if (it->second.stopRequested) it->second.stopRequested->store(true);
                if (it->second.thread.joinable()) {
                    removedThreads.push_back(std::move(it->second.thread));
                }
                appThreads_.erase(it);
            }
        }
    }

    if (!removedThreads.empty()) wakeAppCaptureThreads();
    for (auto& thread : removedThreads) {
        if (thread.joinable()) thread.join();
    }

    // Process-loopback capture includes the target's entire child tree. Finish
    // old targets before replacements start so a reconfiguration cannot briefly
    // publish the same game audio from both overlapping trees.
    {
        std::lock_guard lock(configMutex_);
        if (running_ && appCaptureEnabled_) {
            for (const auto& processName : added) {
                if (std::binary_search(appProcessNames_.begin(), appProcessNames_.end(), processName)) {
                    startAppCaptureThreadLocked(processName);
                }
            }
        }
    }
}

void AudioCaptureWorker::startAppCaptureThreadLocked(const std::string& processName) {
    if (appThreads_.find(processName) != appThreads_.end()) return;
    std::cerr << "[audio] Spawning process loopback thread for: " << processName << std::endl;
    auto stopRequested = std::make_shared<std::atomic<bool>>(false);
    AppCaptureThread appThread {
        processName,
        stopRequested,
        std::thread(&AudioCaptureWorker::runProcessLoopbackCapture, this, processName, stopRequested)
    };
    appThreads_.emplace(processName, std::move(appThread));
}

void AudioCaptureWorker::setMicSettings(float volume, bool isolation, float isolationWeight, bool noiseGateEnabled, bool autoNoiseGate, float noiseGateThreshold, int noiseGateDebounceMs, const std::string& micDeviceId, const std::string& micDeviceMatchKey, const std::string& micDeviceName) {
    micVolume_.store(std::clamp(volume, 0.0f, 4.0f));
    micIsolation_.store(isolation);
    micIsolationWeight_.store(isolationWeight);
    noiseGateEnabled_.store(noiseGateEnabled);
    autoNoiseGate_.store(autoNoiseGate);
    noiseGateThreshold_.store(noiseGateThreshold);
    noiseGateDebounceMs_.store(std::clamp(noiseGateDebounceMs, 0, 1000));
    bool refreshForDeviceChange = false;
    {
        std::lock_guard lock(configMutex_);
        if (micDeviceId_ != micDeviceId || micDeviceMatchKey_ != micDeviceMatchKey || micDeviceName_ != micDeviceName) {
            micDeviceId_ = micDeviceId;
            micDeviceMatchKey_ = micDeviceMatchKey;
            micDeviceName_ = micDeviceName;
            refreshForDeviceChange = running_.load();
        }
    }
    if (refreshForDeviceChange) {
        requestMicRefresh();
    }
}

int AudioCaptureWorker::packetsCaptured() const {
    return packetsCaptured_;
}

std::string AudioCaptureWorker::status() const {
    return status_;
}

std::string AudioCaptureWorker::microphoneStatus() const {
    std::lock_guard lock(configMutex_);
    return microphoneStatus_;
}

void AudioCaptureWorker::publishPacket(EncodedPacket packet) {
    packet.codec = PacketCodec::PcmS16;
    if (replayCoordinator_) {
        replayCoordinator_->publish(std::move(packet));
    } else {
        packets_.push(std::move(packet));
    }
}

void AudioCaptureWorker::setCoreSources(bool systemAudio, bool microphone) {
    bool stopSystem = false, stopMic = false;
    {
        std::lock_guard lock(configMutex_);
        wantSystem_ = systemAudio;
        wantMic_ = microphone;
        if (!microphone) microphoneStatus_ = "Microphone track off";
        if (running_) {
            if (systemAudio) startSystemThreadLocked(); else stopSystem = systemThread_.joinable();
            if (microphone) startMicThreadLocked(); else stopMic = micThread_.joinable();
        }
    }
    if (stopSystem) stopSystemThread();
    if (stopMic) stopMicThread();
}

void AudioCaptureWorker::startSystemThreadLocked() {
    if (systemThread_.joinable()) return;
    systemStopRequested_ = std::make_shared<std::atomic<bool>>(false);
    systemThread_ = std::thread(&AudioCaptureWorker::runCapture, this, true, "system-loopback-pcm", systemStopRequested_);
}

void AudioCaptureWorker::stopSystemThread() {
    {
        std::lock_guard lock(configMutex_);
        if (systemStopRequested_) systemStopRequested_->store(true);
    }
    if (systemThread_.joinable()) systemThread_.join();
    std::lock_guard lock(configMutex_);
    systemStopRequested_.reset();
}

void AudioCaptureWorker::startMicThreadLocked() {
    if (micThread_.joinable()) return;
    micStopRequested_ = std::make_shared<std::atomic<bool>>(false);
    micThread_ = std::thread(&AudioCaptureWorker::runMicrophoneCapture, this);
}

void AudioCaptureWorker::stopMicThread() {
    std::shared_ptr<std::atomic<bool>> stopRequested;
    {
        std::lock_guard lock(configMutex_);
        stopRequested = micStopRequested_;
        if (stopRequested) stopRequested->store(true);
    }
    wakeMicThread();
    if (micThread_.joinable()) micThread_.join();
    {
        std::lock_guard lock(configMutex_);
        micStopRequested_.reset();
    }
}

void AudioCaptureWorker::requestMicRefresh() {
    micRefreshVersion_.fetch_add(1);
    wakeMicThread();
}

void AudioCaptureWorker::wakeMicThread() {
    { std::lock_guard lock(micRefreshMutex_); }
    micRefreshCv_.notify_all();
}

void AudioCaptureWorker::signalMicRefresh() {
    notificationPending_.store(true);
    notificationCv_.notify_one();
}

void AudioCaptureWorker::runDeviceNotificationLoop() {
    nameCurrentThread(L"audio-devices");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<AudioDeviceNotificationClient> notifications;
    hr = Microsoft::WRL::MakeAndInitialize<AudioDeviceNotificationClient>(&notifications, &notificationPending_, &notificationMutex_, &notificationCv_);
    if (SUCCEEDED(hr) && notifications) {
        enumerator->RegisterEndpointNotificationCallback(notifications.Get());
    }

    while (running_) {
        std::unique_lock lock(notificationMutex_);
        notificationCv_.wait(lock, [&] { return !running_.load() || notificationPending_.load(); });
        if (!running_) break;
        notificationPending_.store(false);
        lock.unlock();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!running_) break;

        MicConfigSnapshot config;
        std::string previousEffectiveId;
        {
            std::lock_guard configLock(configMutex_);
            config = { micDeviceId_, micDeviceMatchKey_, micDeviceName_ };
            previousEffectiveId = effectiveMicDeviceId_;
        }

        const auto resolved = resolveMicrophoneDevice(enumerator.Get(), config);
        if (resolved.effectiveId != previousEffectiveId) {
            requestMicRefresh();
        }
    }

    if (notifications) {
        enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
    }
    if (coInitialized) CoUninitialize();
}

}  // namespace clipture

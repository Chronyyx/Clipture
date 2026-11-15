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
void AudioCaptureWorker::runProcessLoopbackCapture(const std::string& processName, std::shared_ptr<std::atomic<bool>> stopRequested) {
    nameCurrentThread(L"audio-app");
    const auto captureSpec = parseAudioProcessSpec(processName);
    const std::string captureName = captureSpec.processName;
    const std::string sourceId = sourceIdForProcessSpec(processName);
    if (captureName.empty()) return;

    std::cerr << "[audio] Process loopback supervisor started for: " << captureName
              << " as " << sourceId << std::endl;
    const auto findTarget = [&](const RunningProcessSnapshot& snapshot) -> DWORD {
        if (!captureSpec.pidSpecific) return preferredProcessTreeRootForName(snapshot.entries(), captureName);
        const auto found = std::find_if(
            snapshot.entries().begin(),
            snapshot.entries().end(),
            [&](const RunningProcessInfo& entry) {
                return entry.processId == captureSpec.processId &&
                    _stricmp(entry.exeName.c_str(), captureName.c_str()) == 0;
            });
        return found != snapshot.entries().end() ? found->processId : 0;
    };
    const auto stopping = [&] { return !running_.load() || stopRequested->load(); };
    // Listing every process costs milliseconds. The first look after a start
    // or a finished capture lists them itself. A thread whose process is not
    // running then waits for the detection loop's next lists (one every 2 s),
    // each taken moments before it is read, instead of listing them again.
    constexpr auto kOwnListInterval = std::chrono::milliseconds(2000);
    constexpr auto kNextListWait = std::chrono::milliseconds(2500);
    bool waitingLogged = false;
    bool waitForNextList = false;
    std::uint64_t seenList = 0;
    DWORD previousProcessId = 0;
    while (!stopping()) {
        DWORD processId = 0;
        ProcessSnapshotFeed::Snapshot shared;
        if (waitForNextList && processFeed_) {
            shared = processFeed_->waitForNewer(seenList, kNextListWait, stopping);
            if (stopping()) break;
        } else if (waitForNextList && !waitUnlessStopping(*stopRequested, kOwnListInterval)) {
            break;
        }
        if (shared) {
            processId = findTarget(*shared);
        } else {
            // Lists published before this one may be older than it.
            if (processFeed_) seenList = processFeed_->generation();
            processId = findTarget(RunningProcessSnapshot::captureNameOnly());
        }

        if (processId == 0) {
            if (!waitingLogged) {
                std::cerr << "[audio] Waiting for configured audio process: " << captureName << std::endl;
                waitingLogged = true;
            }
            waitForNextList = true;
            continue;
        }
        waitForNextList = false;

        if (waitingLogged || previousProcessId != processId) {
            std::cerr << "[audio] Bound " << captureName << " process tree at PID " << processId << std::endl;
        }
        waitingLogged = false;
        previousProcessId = processId;
        runProcessLoopbackCaptureSession(captureName, sourceId, processId, stopRequested);

        if (!running_ || stopRequested->load()) break;
        std::cerr << "[audio] Process loopback session ended for " << captureName
                  << "; waiting to rebind." << std::endl;
        if (!waitUnlessStopping(*stopRequested, std::chrono::milliseconds(1000))) break;
    }
    std::cerr << "[audio] Process loopback supervisor stopped for: " << captureName << std::endl;
}

void AudioCaptureWorker::runProcessLoopbackCaptureSession(
    const std::string& captureName,
    const std::string& sourceId,
    std::uint32_t processId,
    std::shared_ptr<std::atomic<bool>> stopRequested) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams {};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = processId;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateBlob {};
    PropVariantInit(&activateBlob);
    activateBlob.vt = VT_BLOB;
    activateBlob.blob.cbSize = sizeof(activationParams);
    activateBlob.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    Microsoft::WRL::ComPtr<ActivateCompletionHandler> completion;
    hr = Microsoft::WRL::MakeAndInitialize<ActivateCompletionHandler>(&completion);
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: MakeAndInitialize for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateBlob,
        completion.Get(),
        &operation);
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: ActivateAudioInterfaceAsync for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }
    if (!completion->wait()) {
        std::cerr << "[audio] FAILED: Activation timed out for " << captureName << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = completion->result();
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: Activation result for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }
    hr = completion->activated()->QueryInterface(IID_PPV_ARGS(&audioClient));
    if (FAILED(hr) || !audioClient) {
        std::cerr << "[audio] FAILED: QueryInterface IAudioClient for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = getDefaultRenderMixFormat();
    if (!mixFormat) {
        std::cerr << "[audio] FAILED: getDefaultRenderMixFormat for " << captureName << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }

    UniqueEventHandle captureEvent;
    if (!captureEvent) {
        CoTaskMemFree(mixFormat);
        if (coInitialized) CoUninitialize();
        return;
    }

    constexpr REFERENCE_TIME bufferDuration = 1'000'000;
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufferDuration,
        0,
        mixFormat,
        nullptr);
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: AudioClient Initialize for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        CoTaskMemFree(mixFormat);
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: GetService IAudioCaptureClient for " << captureName << " hr=0x" << std::hex << hr << std::dec << std::endl;
        CoTaskMemFree(mixFormat);
        if (coInitialized) CoUninitialize();
        return;
    }
    hr = audioClient->SetEventHandle(captureEvent.get());
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: SetEventHandle for " << captureName
                  << " hr=0x" << std::hex << hr << std::dec << std::endl;
        CoTaskMemFree(mixFormat);
        if (coInitialized) CoUninitialize();
        return;
    }

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    hr = audioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[audio] FAILED: AudioClient Start for " << captureName
                  << " hr=0x" << std::hex << hr << std::dec << std::endl;
        if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
        CoTaskMemFree(mixFormat);
        if (coInitialized) CoUninitialize();
        return;
    }
    HANDLE targetProcessHandle = OpenProcess(SYNCHRONIZE, FALSE, processId);
    auto nextProcessHealthCheck = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int64_t nextPts100ns = now100ns();
    bool audioClockAnchored = false;

    const int outputChannelsForSilence = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));

    std::cerr << "[audio] Process loopback capture RUNNING for " << captureName
              << " (PID " << processId << ", " << mixFormat->nSamplesPerSec << " Hz, "
              << mixFormat->nChannels << " ch)" << std::endl;

    while (running_ && !stopRequested->load()) {
        const auto now = std::chrono::steady_clock::now();
        if (targetProcessHandle && now >= nextProcessHealthCheck) {
            nextProcessHealthCheck = now + std::chrono::seconds(1);
            if (WaitForSingleObject(targetProcessHandle, 0) == WAIT_OBJECT_0) {
                std::cerr << "[audio] Captured process exited: " << captureName
                          << " (PID " << processId << ")" << std::endl;
                break;
            }
        }

        if (!waitForWasapiCapture(captureEvent.get(), 10)) break;

        UINT32 packetFrames = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
        bool capturedPacket = false;
        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition100ns = 0;
            if (FAILED(captureClient->GetBuffer(
                    &data, &frames, &flags, &devicePosition, &qpcPosition100ns))) break;
            const int64_t capturePacketDuration100ns = static_cast<int64_t>(
                (10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
            alignAudioPtsToWasapiClock(
                qpcPosition100ns,
                flags,
                capturePacketDuration100ns,
                audioClockAnchored,
                nextPts100ns);

            int outputChannels = 0;
            const int estimatedChannels = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));
            auto pcm = packets_.acquirePayload(static_cast<std::size_t>(frames) * estimatedChannels * sizeof(int16_t));
            convertToS16(data, frames, flags, mixFormat, outputChannels, *pcm);

            EncodedPacket packet;
            packet.kind = PacketKind::Audio;
            packet.pts100ns = nextPts100ns;
            packet.dts100ns = packet.pts100ns;
            packet.duration100ns = static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
            packet.sourceId = sourceId;
            packet.encoderId = "PCM_S16";
            packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
            packet.channelCount = outputChannels;
            packet.bitsPerSample = 16;
            packet.payload = std::move(pcm);
            publishPacket(std::move(packet));
            nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
            ++packetsCaptured_;
            capturedPacket = true;

            captureClient->ReleaseBuffer(frames);
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
        }
        if (!capturedPacket) {
            const int64_t wallNow = now100ns();
            const UINT32 silenceFrames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
            const int64_t silenceDuration = static_cast<int64_t>(silenceFrames) * 10'000'000 / mixFormat->nSamplesPerSec;
            int fills = 0;
            while (running_ && !stopRequested->load() && loopbackSilenceDue(nextPts100ns, wallNow, silenceDuration) && fills < 5) {
                const UINT32 frames = silenceFrames;
                EncodedPacket packet;
                packet.kind = PacketKind::Audio;
                packet.pts100ns = nextPts100ns;
                packet.dts100ns = packet.pts100ns;
                packet.duration100ns = static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                packet.sourceId = sourceId;
                packet.encoderId = "PCM_S16";
                packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
                packet.channelCount = outputChannelsForSilence;
                packet.bitsPerSample = 16;
                packet.payload = packets_.acquirePayload(static_cast<std::size_t>(frames) * outputChannelsForSilence * sizeof(int16_t));
                fillSilentS16(frames, outputChannelsForSilence, mutablePayload(packet));
                publishPacket(std::move(packet));
                nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                ++packetsCaptured_;
                ++fills;
            }
        }
    }

    audioClient->Stop();
    if (targetProcessHandle) CloseHandle(targetProcessHandle);
    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    CoTaskMemFree(mixFormat);
    if (coInitialized) CoUninitialize();
}

}  // namespace clipture

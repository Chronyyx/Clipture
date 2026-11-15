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
void AudioCaptureWorker::runCapture(bool loopback, const std::string& sourceId,
                                    std::shared_ptr<std::atomic<bool>> stopRequested) {
    nameCurrentThread(L"audio-system");
    const auto keepRunning = [&] { return running_.load() && !(stopRequested && stopRequested->load()); };
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        status_ = "WASAPI device enumerator failed.";
        if (coInitialized) CoUninitialize();
        return;
    }

    std::string selectedMicDeviceId;
    if (!loopback) {
        std::lock_guard lock(configMutex_);
        selectedMicDeviceId = micDeviceId_;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (!loopback && !selectedMicDeviceId.empty()) {
        const auto wideId = widen(selectedMicDeviceId);
        hr = enumerator->GetDevice(wideId.c_str(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device);
    }
    if (FAILED(hr)) {
        status_ = loopback
            ? "No default render audio endpoint found."
            : (!selectedMicDeviceId.empty() ? "Selected microphone endpoint was not found." : "No default microphone endpoint found.");
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
    if (FAILED(hr)) {
        status_ = loopback ? "Could not activate WASAPI loopback client." : "Could not activate WASAPI microphone client.";
        if (coInitialized) CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        status_ = "Could not read WASAPI mix format.";
        if (coInitialized) CoUninitialize();
        return;
    }

    UniqueEventHandle captureEvent;
    if (!captureEvent) {
        CoTaskMemFree(mixFormat);
        status_ = "Could not create WASAPI capture event.";
        if (coInitialized) CoUninitialize();
        return;
    }

    constexpr REFERENCE_TIME bufferDuration = 1'000'000; // 100 ms
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        status_ = loopback ? "WASAPI loopback initialization failed." : "WASAPI microphone initialization failed.";
        if (coInitialized) CoUninitialize();
        return;
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        status_ = "Could not get WASAPI capture client.";
        if (coInitialized) CoUninitialize();
        return;
    }
    hr = audioClient->SetEventHandle(captureEvent.get());
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        status_ = "Could not set WASAPI capture event.";
        if (coInitialized) CoUninitialize();
        return;
    }

    DenoiseState* rnnoiseState = nullptr;
    const int rnnoiseFrameSize = rnnoise_get_frame_size(); // 480
    const bool rnnoiseSampleRateSupported = mixFormat->nSamplesPerSec == 48000;
    RnnoiseFrameQueue rnnoiseQueue(rnnoiseFrameSize);
    PacketPayload micInputScratch;
    std::vector<float> rnnoiseFrameIn(static_cast<std::size_t>(rnnoiseFrameSize));
    std::vector<float> rnnoiseFrameOut(static_cast<std::size_t>(rnnoiseFrameSize));
    auto lastRnnoiseQueueLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    audioClient->Start();
    status_ = "WASAPI system and microphone capture are running.";
    int64_t nextPts100ns = now100ns();
    bool audioClockAnchored = false;
    const int outputChannelsForSilence = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));

    float currentGateGain = 1.0f;
    int gateHoldFrames = 0;

    while (keepRunning()) {
        if (!waitForWasapiCapture(captureEvent.get(), loopback ? 10 : 50)) break;
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
            PacketPayloadPtr pcm;

            if (sourceId == "microphone-pcm") {
                convertToS16(
                    data,
                    frames,
                    flags,
                    mixFormat,
                    outputChannels,
                    micInputScratch);
                const auto* micInputSamples = reinterpret_cast<const int16_t*>(micInputScratch.data());
                const float vol = micVolume_.load();
                const bool isolation = micIsolation_.load();
                const float weight = micIsolationWeight_.load();
                const bool gateEnabled = noiseGateEnabled_.load();
                const bool autoGate = autoNoiseGate_.load();
                const float gateThreshold = noiseGateThreshold_.load();
                const int gateDebounceMs = noiseGateDebounceMs_.load();
                
                for (UINT32 i = 0; i < frames; ++i) {
                    float mono = 0.0f;
                    for (int ch = 0; ch < outputChannels; ++ch) {
                        mono += micInputSamples[i * outputChannels + ch];
                    }
                    mono /= outputChannels;
                    rnnoiseQueue.push(std::clamp(mono, -32768.0f, 32767.0f));
                }
                
                while (rnnoiseQueue.popFrame(rnnoiseFrameIn)) {
                    const bool needsRnnoise = rnnoiseSampleRateSupported &&
                        (isolation || (gateEnabled && autoGate));
                    if (needsRnnoise && !rnnoiseState) {
                        rnnoiseState = rnnoise_create(nullptr);
                    } else if (!needsRnnoise && rnnoiseState) {
                        rnnoise_destroy(rnnoiseState);
                        rnnoiseState = nullptr;
                    }

                    float vadProb = 0.0f;
                    const bool rnnoiseReady = needsRnnoise && rnnoiseState;
                    if (rnnoiseReady) {
                        vadProb = rnnoise_process_frame(rnnoiseState, rnnoiseFrameOut.data(), rnnoiseFrameIn.data());
                    }
                    
                    bool isSpeaking = true;
                    if (!gateEnabled) {
                        gateHoldFrames = 0;
                    } else if (autoGate) {
                        isSpeaking = rnnoiseReady ? vadProb > 0.5f : true;
                    } else {
                        float sum = 0.0f;
                        for (int j = 0; j < rnnoiseFrameSize; ++j) {
                            float s = rnnoiseFrameIn[j] / 32768.0f;
                            sum += s * s;
                        }
                        float rms = std::sqrt(sum / rnnoiseFrameSize);
                        isSpeaking = rms > gateThreshold;
                    }
                    
                    if (gateEnabled) {
                        const int debounceFrames = std::max(0, static_cast<int>(std::ceil((gateDebounceMs / 1000.0f) * mixFormat->nSamplesPerSec / rnnoiseFrameSize)));
                        if (isSpeaking) {
                            gateHoldFrames = debounceFrames;
                        } else if (gateHoldFrames > 0) {
                            --gateHoldFrames;
                        }
                    }
                    const bool gateOpen = !gateEnabled || isSpeaking || gateHoldFrames > 0;
                    float targetGateGain = gateOpen ? 1.0f : 0.0f;
                    
                    auto outPcm = packets_.acquirePayload(static_cast<std::size_t>(rnnoiseFrameSize) * outputChannels * sizeof(int16_t));
                    auto* out16 = reinterpret_cast<int16_t*>(outPcm->data());
                    
                    for (int j = 0; j < rnnoiseFrameSize; ++j) {
                        const float gateStep = targetGateGain > currentGateGain ? 0.18f : 0.05f;
                        currentGateGain += (targetGateGain - currentGateGain) * gateStep;
                        
                        float original = rnnoiseFrameIn[j];
                        float processed = (isolation && rnnoiseReady) ? rnnoiseFrameOut[j] : original;
                        float w = (isolation && rnnoiseReady) ? weight : 0.0f;
                        
                        float blended = (original * (1.0f - w)) + (processed * w);
                        blended *= vol * currentGateGain;
                        const int16_t outVal = processingSampleToS16(blended);
                        for (int ch = 0; ch < outputChannels; ++ch) {
                            out16[j * outputChannels + ch] = outVal;
                        }
                    }
                    
                    EncodedPacket packet;
                    packet.kind = PacketKind::Audio;
                    packet.pts100ns = nextPts100ns;
                    packet.dts100ns = packet.pts100ns;
                    packet.duration100ns = static_cast<int64_t>((10'000'000.0 * rnnoiseFrameSize) / mixFormat->nSamplesPerSec);
                    packet.sourceId = sourceId;
                    packet.encoderId = "PCM_S16";
                    packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
                    packet.channelCount = outputChannels;
                    packet.bitsPerSample = 16;
                    packet.payload = std::move(outPcm);
                    publishPacket(std::move(packet));
                    
                    nextPts100ns += packet.duration100ns;
                    ++packetsCaptured_;
                    capturedPacket = true;
                }
                maybeLogRnnoiseQueueDepth(sourceId, rnnoiseQueue, rnnoiseFrameSize, lastRnnoiseQueueLog);
                
                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
                continue;
            }

            const int estimatedChannels = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));
            pcm = packets_.acquirePayload(
                static_cast<std::size_t>(frames) * estimatedChannels * sizeof(int16_t));
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
            nextPts100ns += packet.duration100ns;
            ++packetsCaptured_;
            capturedPacket = true;

            captureClient->ReleaseBuffer(frames);
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) {
                packetFrames = 0;
            }
        }
        if (!capturedPacket) {
            const int64_t wallNow = now100ns();
            const UINT32 silenceFrames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
            const int64_t silenceDuration = static_cast<int64_t>(silenceFrames) * 10'000'000 / mixFormat->nSamplesPerSec;
            int fills = 0;
            while (keepRunning() && loopbackSilenceDue(nextPts100ns, wallNow, silenceDuration) && fills < 5) {
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
    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    CoTaskMemFree(mixFormat);
    if (rnnoiseState) rnnoise_destroy(rnnoiseState);
    if (coInitialized) CoUninitialize();
}

}  // namespace clipture

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
void AudioCaptureWorker::runMicrophoneCapture() {
    nameCurrentThread(L"audio-mic");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::lock_guard lock(configMutex_);
        microphoneStatus_ = "No microphone available";
        return;
    }

    int64_t nextPts100ns = now100ns();

    while (running_ && (!micStopRequested_ || !micStopRequested_->load())) {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr) || !enumerator) {
            std::lock_guard lock(configMutex_);
            microphoneStatus_ = "No microphone available";
            break;
        }

        MicConfigSnapshot config;
        {
            std::lock_guard lock(configMutex_);
            config = { micDeviceId_, micDeviceMatchKey_, micDeviceName_ };
        }

        const int captureVersion = micRefreshVersion_.load();
        const auto resolved = resolveMicrophoneDevice(enumerator.Get(), config);
        {
            std::lock_guard lock(configMutex_);
            effectiveMicDeviceId_ = resolved.effectiveId;
            if (!resolved.device) {
                microphoneStatus_ = resolved.explicitSelection ? "Selected mic unavailable, no default microphone" : "No microphone available";
            } else if (resolved.usingDefaultFallback) {
                microphoneStatus_ = "Selected mic unavailable, using default: " + resolved.name;
            } else if (resolved.explicitSelection) {
                microphoneStatus_ = "Selected mic: " + resolved.name;
            } else {
                microphoneStatus_ = "System default: " + resolved.name;
            }
        }

        if (!resolved.device) {
            nextPts100ns = now100ns();

            // No microphone: sleep until one appears (the device notification
            // thread asks for a refresh), the settings change, or a stop.
            std::unique_lock refreshLock(micRefreshMutex_);
            micRefreshCv_.wait(refreshLock, [&] {
                return !running_.load() || (micStopRequested_ && micStopRequested_->load()) || micRefreshVersion_.load() != captureVersion;
            });
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioClient> audioClient;
        hr = resolved.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr) || !audioClient) {
            requestMicRefresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        UniqueEventHandle captureEvent;
        if (!captureEvent) {
            CoTaskMemFree(mixFormat);
            requestMicRefresh();
            continue;
        }

        constexpr REFERENCE_TIME bufferDuration = 1'000'000;
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            bufferDuration,
            0,
            mixFormat,
            nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(mixFormat);
            requestMicRefresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient;
        hr = audioClient->GetService(IID_PPV_ARGS(&captureClient));
        if (FAILED(hr) || !captureClient) {
            CoTaskMemFree(mixFormat);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        hr = audioClient->SetEventHandle(captureEvent.get());
        if (FAILED(hr)) {
            CoTaskMemFree(mixFormat);
            requestMicRefresh();
            continue;
        }

        DenoiseState* rnnoiseState = nullptr;
        const int rnnoiseFrameSize = rnnoise_get_frame_size();
        const bool rnnoiseSampleRateSupported = mixFormat->nSamplesPerSec == 48000;
        RnnoiseFrameQueue rnnoiseQueue(rnnoiseFrameSize);
        PacketPayload micInputScratch;
        std::vector<float> rnnoiseFrameIn(static_cast<std::size_t>(rnnoiseFrameSize));
        std::vector<float> rnnoiseFrameOut(static_cast<std::size_t>(rnnoiseFrameSize));
        auto lastRnnoiseQueueLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        float currentGateGain = 1.0f;
        int gateHoldFrames = 0;

        DWORD taskIndex = 0;
        HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        audioClient->Start();
        status_ = "WASAPI system and microphone capture are running.";
        bool audioClockAnchored = false;
        nextPts100ns = std::max(nextPts100ns, now100ns());

        while (running_ && (!micStopRequested_ || !micStopRequested_->load()) && micRefreshVersion_.load() == captureVersion) {
            if (!waitForWasapiCapture(captureEvent.get(), 50)) break;
            UINT32 packetFrames = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
            while (packetFrames > 0 && micRefreshVersion_.load() == captureVersion) {
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
                // RNNoise is trained around S16-scale input. Quantize through the
                // replay format first so high-resolution endpoint noise is not
                // amplified into denoiser artifacts when speech opens the gate.
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
                    const float targetGateGain = gateOpen ? 1.0f : 0.0f;

                    auto outPcm = packets_.acquirePayload(static_cast<std::size_t>(rnnoiseFrameSize) * outputChannels * sizeof(int16_t));
                    auto* out16 = reinterpret_cast<int16_t*>(outPcm->data());

                    for (int j = 0; j < rnnoiseFrameSize; ++j) {
                        const float gateStep = targetGateGain > currentGateGain ? 0.18f : 0.05f;
                        currentGateGain += (targetGateGain - currentGateGain) * gateStep;

                        const float original = rnnoiseFrameIn[j];
                        const float processed = (isolation && rnnoiseReady) ? rnnoiseFrameOut[j] : original;
                        const float w = (isolation && rnnoiseReady) ? weight : 0.0f;

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
                    packet.sourceId = "microphone-pcm";
                    packet.encoderId = "PCM_S16";
                    packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
                    packet.channelCount = outputChannels;
                    packet.bitsPerSample = 16;
                    packet.payload = std::move(outPcm);
                    publishPacket(std::move(packet));

                    nextPts100ns += packet.duration100ns;
                    ++packetsCaptured_;
                }
                maybeLogRnnoiseQueueDepth("microphone-pcm", rnnoiseQueue, rnnoiseFrameSize, lastRnnoiseQueueLog);

                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
            }
        }

        audioClient->Stop();
        if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
        CoTaskMemFree(mixFormat);
        if (rnnoiseState) rnnoise_destroy(rnnoiseState);
    }

    if (coInitialized) CoUninitialize();
}

}  // namespace clipture

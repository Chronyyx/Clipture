#include "clipture/AudioCaptureWorker.hpp"
#include "clipture/ProcessSnapshot.hpp"

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

namespace clipture {
namespace {

class ActivateCompletionHandler :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        Microsoft::WRL::FtmBase,
        IActivateAudioInterfaceCompletionHandler> {
public:
    HRESULT RuntimeClassInitialize() {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    IFACEMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hr = E_FAIL;
        Microsoft::WRL::ComPtr<IUnknown> activated;
        if (operation) {
            operation->GetActivateResult(&hr, &activated);
        }
        result_ = hr;
        activated_ = activated;
        SetEvent(event_);
        return S_OK;
    }

    bool wait(DWORD timeoutMs = 5000) const {
        return event_ && WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
    }

    HRESULT result() const {
        return result_;
    }

    IUnknown* activated() const {
        return activated_.Get();
    }

private:
    HANDLE event_ = nullptr;
    HRESULT result_ = E_PENDING;
    Microsoft::WRL::ComPtr<IUnknown> activated_;
};

class AudioDeviceNotificationClient :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMMNotificationClient> {
public:
    HRESULT RuntimeClassInitialize(std::atomic<bool>* pending, std::mutex* mutex, std::condition_variable* cv) {
        pending_ = pending;
        mutex_ = mutex;
        cv_ = cv;
        return S_OK;
    }

    IFACEMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override {
        signal();
        return S_OK;
    }

    IFACEMETHODIMP OnDeviceAdded(LPCWSTR) override {
        signal();
        return S_OK;
    }

    IFACEMETHODIMP OnDeviceRemoved(LPCWSTR) override {
        signal();
        return S_OK;
    }

    IFACEMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
        if (flow == eCapture || flow == eAll) signal();
        return S_OK;
    }

    IFACEMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        signal();
        return S_OK;
    }

private:
    void signal() const {
        if (!pending_ || !mutex_ || !cv_) return;
        pending_->store(true);
        std::lock_guard lock(*mutex_);
        cv_->notify_one();
    }

    std::atomic<bool>* pending_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
};

struct MicConfigSnapshot {
    std::string deviceId;
    std::string matchKey;
    std::string name;
};

struct ResolvedMicDevice {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    std::string effectiveId;
    std::string name;
    bool usingDefaultFallback = false;
    bool selectedUnavailable = false;
    bool explicitSelection = false;
};

class RnnoiseFrameQueue {
public:
    explicit RnnoiseFrameQueue(int frameSize)
        : frameSize_(static_cast<std::size_t>(std::max(1, frameSize))) {
        buffer_.resize(frameSize_ * 4);
    }

    void push(float sample) {
        ensureCapacity(size_ + 1);
        buffer_[(head_ + size_) % buffer_.size()] = sample;
        ++size_;
        maxDepth_ = std::max(maxDepth_, size_);
    }

    bool popFrame(std::vector<float>& frame) {
        if (size_ < frameSize_) return false;
        if (frame.size() != frameSize_) frame.resize(frameSize_);

        for (std::size_t i = 0; i < frameSize_; ++i) {
            frame[i] = buffer_[(head_ + i) % buffer_.size()];
        }

        head_ = (head_ + frameSize_) % buffer_.size();
        size_ -= frameSize_;
        if (size_ == 0) head_ = 0;
        return true;
    }

    std::size_t pending() const {
        return size_;
    }

    std::size_t maxDepth() const {
        return maxDepth_;
    }

private:
    void ensureCapacity(std::size_t required) {
        if (required <= buffer_.size()) return;
        std::vector<float> next(std::max(required, buffer_.size() * 2));
        for (std::size_t i = 0; i < size_; ++i) {
            next[i] = buffer_[(head_ + i) % buffer_.size()];
        }
        buffer_ = std::move(next);
        head_ = 0;
    }

    std::vector<float> buffer_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t maxDepth_ = 0;
    std::size_t frameSize_ = 0;
};

void maybeLogRnnoiseQueueDepth(
    const std::string& sourceId,
    const RnnoiseFrameQueue& queue,
    int frameSize,
    std::chrono::steady_clock::time_point& lastLog) {
    const auto warningDepth = static_cast<std::size_t>(std::max(1, frameSize) * 4);
    if (queue.maxDepth() < warningDepth) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastLog < std::chrono::seconds(10)) return;
    lastLog = now;

    std::cerr << "[perf] rnnoise_queue source=\"" << sourceId << "\""
              << " maxDepth=" << queue.maxDepth()
              << " pending=" << queue.pending()
              << " frameSize=" << frameSize
              << std::endl;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring result(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    return result;
}

std::string narrow(const wchar_t* value) {
    if (!value) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

std::string propertyStoreString(IPropertyStore* properties, const PROPERTYKEY& key) {
    if (!properties) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = properties->GetValue(key, &value);
    std::string result;
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = narrow(value.pwszVal);
    }
    PropVariantClear(&value);
    return result;
}

std::string deviceId(IMMDevice* device) {
    if (!device) return {};
    LPWSTR id = nullptr;
    const HRESULT hr = device->GetId(&id);
    if (FAILED(hr) || !id) return {};
    const std::string result = narrow(id);
    CoTaskMemFree(id);
    return result;
}

std::string deviceFriendlyName(IMMDevice* device) {
    if (!device) return "Unknown";
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties) return "Unknown";
    auto name = propertyStoreString(properties.Get(), PKEY_Device_FriendlyName);
    return name.empty() ? "Unknown" : name;
}

std::string deviceMatchKey(IMMDevice* device) {
    if (!device) return {};
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && properties) {
        const auto instanceId = propertyStoreString(properties.Get(), PKEY_Device_InstanceId);
        if (!instanceId.empty()) return instanceId;
    }
    return deviceId(device);
}

bool deviceIsActive(IMMDevice* device) {
    if (!device) return false;
    DWORD state = 0;
    return SUCCEEDED(device->GetState(&state)) && (state & DEVICE_STATE_ACTIVE) != 0;
}

Microsoft::WRL::ComPtr<IMMDevice> defaultCaptureEndpoint(IMMDeviceEnumerator* enumerator) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (!enumerator) return nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) return nullptr;
    return device;
}

ResolvedMicDevice resolveMicrophoneDevice(IMMDeviceEnumerator* enumerator, const MicConfigSnapshot& config) {
    ResolvedMicDevice result;
    result.explicitSelection = !config.deviceId.empty() || !config.matchKey.empty();

    if (!enumerator) return result;

    if (!config.deviceId.empty()) {
        Microsoft::WRL::ComPtr<IMMDevice> exact;
        if (SUCCEEDED(enumerator->GetDevice(widen(config.deviceId).c_str(), &exact)) && exact && deviceIsActive(exact.Get())) {
            result.device = exact;
            result.effectiveId = deviceId(exact.Get());
            result.name = deviceFriendlyName(exact.Get());
            return result;
        }
        result.selectedUnavailable = true;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);

        if (!config.matchKey.empty()) {
            for (UINT index = 0; index < count; ++index) {
                Microsoft::WRL::ComPtr<IMMDevice> candidate;
                if (FAILED(collection->Item(index, &candidate)) || !candidate) continue;
                if (deviceMatchKey(candidate.Get()) == config.matchKey) {
                    result.device = candidate;
                    result.effectiveId = deviceId(candidate.Get());
                    result.name = deviceFriendlyName(candidate.Get());
                    return result;
                }
            }
            result.selectedUnavailable = true;
        }

        if (!config.name.empty() && result.explicitSelection && config.deviceId.empty() && config.matchKey.empty()) {
            Microsoft::WRL::ComPtr<IMMDevice> singleMatch;
            int matches = 0;
            for (UINT index = 0; index < count; ++index) {
                Microsoft::WRL::ComPtr<IMMDevice> candidate;
                if (FAILED(collection->Item(index, &candidate)) || !candidate) continue;
                if (deviceFriendlyName(candidate.Get()) == config.name) {
                    singleMatch = candidate;
                    ++matches;
                }
            }
            if (matches == 1 && singleMatch) {
                result.device = singleMatch;
                result.effectiveId = deviceId(singleMatch.Get());
                result.name = deviceFriendlyName(singleMatch.Get());
                return result;
            }
        }
    }

    auto fallback = defaultCaptureEndpoint(enumerator);
    if (fallback && deviceIsActive(fallback.Get())) {
        result.device = fallback;
        result.effectiveId = deviceId(fallback.Get());
        result.name = deviceFriendlyName(fallback.Get());
        result.usingDefaultFallback = result.explicitSelection;
        return result;
    }

    return result;
}

std::string captureProcessName(const std::string& sourceSpec) {
    if (sourceSpec.rfind("game:", 0) == 0) return sourceSpec.substr(5);
    if (sourceSpec.rfind("app:", 0) == 0) return sourceSpec.substr(4);
    return sourceSpec;
}

std::string sourceIdForProcessSpec(const std::string& sourceSpec) {
    if (sourceSpec.rfind("game:", 0) == 0) return sourceSpec;
    if (sourceSpec.rfind("app:", 0) == 0) return sourceSpec;
    return "app:" + sourceSpec;
}

DWORD findProcessIdByName(const std::string& processName) {
    return RunningProcessSnapshot::captureNameOnly().processIdForName(processName);
}

int64_t now100ns() {
    FILETIME fileTime {};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart);
}

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

int sourceBitsPerSample(const WAVEFORMATEX* format) {
    if (!format) return 16;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->Samples.wValidBitsPerSample > 0) return extensible->Samples.wValidBitsPerSample;
    }
    return format->wBitsPerSample;
}

int16_t clampToS16(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(value * 32767.0f));
}

void convertToS16(const BYTE* data, UINT32 frames, DWORD flags, const WAVEFORMATEX* format, int& outChannels, PacketPayload& output) {
    const int inputChannels = std::max<int>(1, format ? format->nChannels : 2);
    const int outputChannels = std::min<int>(2, inputChannels);
    const int bits = sourceBitsPerSample(format);
    const bool floatFormat = isFloatFormat(format);
    const std::size_t outputSamples = static_cast<std::size_t>(frames) * outputChannels;
    output.resize(outputSamples * sizeof(int16_t));
    outChannels = outputChannels;

    auto* dest = reinterpret_cast<int16_t*>(output.data());
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
        std::fill(dest, dest + outputSamples, int16_t { 0 });
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame) {
        for (int ch = 0; ch < outputChannels; ++ch) {
            const int inputIndex = static_cast<int>(frame) * inputChannels + ch;
            int16_t sample = 0;
            if (floatFormat && bits == 32) {
                const auto* src = reinterpret_cast<const float*>(data);
                sample = clampToS16(src[inputIndex]);
            } else if (bits == 16) {
                const auto* src = reinterpret_cast<const int16_t*>(data);
                sample = src[inputIndex];
            } else if (bits == 24) {
                const auto* src = data + (static_cast<std::size_t>(inputIndex) * 3);
                int32_t value = (static_cast<int32_t>(src[0]) << 8) |
                    (static_cast<int32_t>(src[1]) << 16) |
                    (static_cast<int32_t>(src[2]) << 24);
                sample = static_cast<int16_t>(value >> 16);
            } else if (bits == 32) {
                const auto* src = reinterpret_cast<const int32_t*>(data);
                sample = static_cast<int16_t>(src[inputIndex] >> 16);
            }
            dest[(static_cast<std::size_t>(frame) * outputChannels) + ch] = sample;
        }
    }
}

void fillSilentS16(UINT32 frames, int channels, PacketPayload& output) {
    const std::size_t outputSamples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(std::max(1, channels));
    output.resize(outputSamples * sizeof(int16_t));
    std::fill(output.begin(), output.end(), std::byte { 0 });
}

WAVEFORMATEX* getDefaultRenderMixFormat() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return nullptr;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return nullptr;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf())))) return nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(audioClient->GetMixFormat(&mixFormat))) return nullptr;
    return mixFormat;
}

}  // namespace

AudioCaptureWorker::AudioCaptureWorker(PacketRingBuffer& packets)
    : packets_(packets) {}

AudioCaptureWorker::~AudioCaptureWorker() {
    stop();
}

void AudioCaptureWorker::start() {
    if (running_.exchange(true)) return;
    coreThreads_.emplace_back(&AudioCaptureWorker::runCapture, this, true, "system-loopback-pcm");
    {
        std::lock_guard lock(configMutex_);
        startMicThreadLocked();
    }
    notificationThread_ = std::thread(&AudioCaptureWorker::runDeviceNotificationLoop, this);
    std::vector<std::string> appProcessNames;
    {
        std::lock_guard lock(configMutex_);
        appProcessNames = appProcessNames_;
        for (const auto& processName : appProcessNames) {
            startAppCaptureThreadLocked(processName);
        }
    }
    if (appProcessNames.empty()) {
        std::cerr << "[audio] No app audio sources configured." << std::endl;
    }
}

void AudioCaptureWorker::stop() {
    if (!running_.exchange(false)) return;
    notificationPending_.store(true);
    notificationCv_.notify_all();
    micRefreshCv_.notify_all();
    stopMicThread();
    std::vector<std::thread> appThreadsToJoin;
    {
        std::lock_guard lock(configMutex_);
        for (auto& [_, appThread] : appThreads_) {
            if (appThread.stopRequested) appThread.stopRequested->store(true);
            if (appThread.thread.joinable()) {
                appThreadsToJoin.push_back(std::move(appThread.thread));
            }
        }
        appThreads_.clear();
    }
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
        std::vector<std::string> added;
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

        if (running_) {
            for (const auto& processName : removed) {
                auto it = appThreads_.find(processName);
                if (it == appThreads_.end()) continue;
                if (it->second.stopRequested) it->second.stopRequested->store(true);
                if (it->second.thread.joinable()) {
                    removedThreads.push_back(std::move(it->second.thread));
                }
                appThreads_.erase(it);
            }
            for (const auto& processName : added) {
                startAppCaptureThreadLocked(processName);
            }
        }
    }

    for (auto& thread : removedThreads) {
        if (thread.joinable()) thread.join();
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
    micRefreshCv_.notify_all();
    if (micThread_.joinable()) micThread_.join();
    {
        std::lock_guard lock(configMutex_);
        micStopRequested_.reset();
    }
}

void AudioCaptureWorker::requestMicRefresh() {
    micRefreshVersion_.fetch_add(1);
    micRefreshCv_.notify_all();
}

void AudioCaptureWorker::signalMicRefresh() {
    notificationPending_.store(true);
    notificationCv_.notify_one();
}

void AudioCaptureWorker::runDeviceNotificationLoop() {
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

void AudioCaptureWorker::runMicrophoneCapture() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::lock_guard lock(configMutex_);
        microphoneStatus_ = "No microphone available";
        return;
    }

    int64_t nextPts100ns = now100ns();
    constexpr int fallbackSampleRate = 48000;
    constexpr int fallbackChannels = 1;

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
            const int64_t wallNow = now100ns();
            int fills = 0;
            while (running_ && (!micStopRequested_ || !micStopRequested_->load()) && nextPts100ns <= wallNow && fills < 10) {
                const UINT32 frames = fallbackSampleRate / 100;
                EncodedPacket packet;
                packet.kind = PacketKind::Audio;
                packet.pts100ns = nextPts100ns;
                packet.dts100ns = packet.pts100ns;
                packet.duration100ns = static_cast<int64_t>((10'000'000.0 * frames) / fallbackSampleRate);
                packet.sourceId = "microphone-pcm";
                packet.encoderId = "PCM_S16";
                packet.sampleRate = fallbackSampleRate;
                packet.channelCount = fallbackChannels;
                packet.bitsPerSample = 16;
                packet.payload = packets_.acquirePayload(static_cast<std::size_t>(frames) * fallbackChannels * sizeof(int16_t));
                fillSilentS16(frames, fallbackChannels, mutablePayload(packet));
                packets_.push(std::move(packet));
                nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / fallbackSampleRate);
                ++packetsCaptured_;
                ++fills;
            }

            std::unique_lock refreshLock(micRefreshMutex_);
            micRefreshCv_.wait_for(refreshLock, std::chrono::milliseconds(500), [&] {
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

        constexpr REFERENCE_TIME bufferDuration = 1'000'000;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
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

        DenoiseState* rnnoiseState = nullptr;
        const int rnnoiseFrameSize = rnnoise_get_frame_size();
        RnnoiseFrameQueue rnnoiseQueue(rnnoiseFrameSize);
        std::vector<float> rnnoiseFrameIn(static_cast<std::size_t>(rnnoiseFrameSize));
        std::vector<float> rnnoiseFrameOut(static_cast<std::size_t>(rnnoiseFrameSize));
        auto lastRnnoiseQueueLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        float currentGateGain = 1.0f;
        int gateHoldFrames = 0;

        DWORD taskIndex = 0;
        HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        audioClient->Start();
        status_ = "WASAPI system and microphone capture are running.";
        const int outputChannelsForSilence = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));
        const UINT32 catchUpFrames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
        const int64_t catchUpDuration100ns = static_cast<int64_t>((10'000'000.0 * catchUpFrames) / mixFormat->nSamplesPerSec);
        const int64_t captureStart100ns = now100ns();
        while (running_ && (!micStopRequested_ || !micStopRequested_->load()) && nextPts100ns + catchUpDuration100ns <= captureStart100ns) {
            EncodedPacket packet;
            packet.kind = PacketKind::Audio;
            packet.pts100ns = nextPts100ns;
            packet.dts100ns = packet.pts100ns;
            packet.duration100ns = catchUpDuration100ns;
            packet.sourceId = "microphone-pcm";
            packet.encoderId = "PCM_S16";
            packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
            packet.channelCount = outputChannelsForSilence;
            packet.bitsPerSample = 16;
            packet.payload = packets_.acquirePayload(static_cast<std::size_t>(catchUpFrames) * outputChannelsForSilence * sizeof(int16_t));
            fillSilentS16(catchUpFrames, outputChannelsForSilence, mutablePayload(packet));
            packets_.push(std::move(packet));
            nextPts100ns += catchUpDuration100ns;
            ++packetsCaptured_;
        }

        while (running_ && (!micStopRequested_ || !micStopRequested_->load()) && micRefreshVersion_.load() == captureVersion) {
            UINT32 packetFrames = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
            bool capturedPacket = false;
            while (packetFrames > 0 && micRefreshVersion_.load() == captureVersion) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

                int outputChannels = 0;
                const int estimatedChannels = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));
                auto pcm = packets_.acquirePayload(static_cast<std::size_t>(frames) * estimatedChannels * sizeof(int16_t));
                convertToS16(data, frames, flags, mixFormat, outputChannels, *pcm);

                const float vol = micVolume_.load();
                const bool isolation = micIsolation_.load();
                const float weight = micIsolationWeight_.load();
                const bool gateEnabled = noiseGateEnabled_.load();
                const bool autoGate = autoNoiseGate_.load();
                const float gateThreshold = noiseGateThreshold_.load();
                const int gateDebounceMs = noiseGateDebounceMs_.load();
                auto* pcm16 = reinterpret_cast<int16_t*>(pcm->data());

                for (UINT32 i = 0; i < frames; ++i) {
                    float mono = 0.0f;
                    for (int ch = 0; ch < outputChannels; ++ch) {
                        mono += pcm16[i * outputChannels + ch];
                    }
                    mono /= outputChannels;
                    rnnoiseQueue.push(mono);
                }

                while (rnnoiseQueue.popFrame(rnnoiseFrameIn)) {
                    const bool needsRnnoise = isolation || (gateEnabled && autoGate);
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
                        const int16_t outVal = static_cast<int16_t>(std::clamp(blended, -32768.0f, 32767.0f));
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
                    packets_.push(std::move(packet));

                    nextPts100ns += packet.duration100ns;
                    ++packetsCaptured_;
                }
                maybeLogRnnoiseQueueDepth("microphone-pcm", rnnoiseQueue, rnnoiseFrameSize, lastRnnoiseQueueLog);

                capturedPacket = true;
                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
            }

            if (!capturedPacket) {
                const int64_t wallNow = now100ns();
                int fills = 0;
                while (running_ && (!micStopRequested_ || !micStopRequested_->load()) && nextPts100ns <= wallNow && fills < 5) {
                    const UINT32 frames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
                    EncodedPacket packet;
                    packet.kind = PacketKind::Audio;
                    packet.pts100ns = nextPts100ns;
                    packet.dts100ns = packet.pts100ns;
                    packet.duration100ns = static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                    packet.sourceId = "microphone-pcm";
                    packet.encoderId = "PCM_S16";
                    packet.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
                    packet.channelCount = outputChannelsForSilence;
                    packet.bitsPerSample = 16;
                    packet.payload = packets_.acquirePayload(static_cast<std::size_t>(frames) * outputChannelsForSilence * sizeof(int16_t));
                    fillSilentS16(frames, outputChannelsForSilence, mutablePayload(packet));
                    packets_.push(std::move(packet));
                    nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                    ++packetsCaptured_;
                    ++fills;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        audioClient->Stop();
        if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
        CoTaskMemFree(mixFormat);
        if (rnnoiseState) rnnoise_destroy(rnnoiseState);
    }

    if (coInitialized) CoUninitialize();
}

void AudioCaptureWorker::runCapture(bool loopback, const std::string& sourceId) {
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

    constexpr REFERENCE_TIME bufferDuration = 1'000'000; // 100 ms
    const DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
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

    DenoiseState* rnnoiseState = nullptr;
    const int rnnoiseFrameSize = rnnoise_get_frame_size(); // 480
    RnnoiseFrameQueue rnnoiseQueue(rnnoiseFrameSize);
    std::vector<float> rnnoiseFrameIn(static_cast<std::size_t>(rnnoiseFrameSize));
    std::vector<float> rnnoiseFrameOut(static_cast<std::size_t>(rnnoiseFrameSize));
    auto lastRnnoiseQueueLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    audioClient->Start();
    status_ = "WASAPI system and microphone capture are running.";
    int64_t nextPts100ns = now100ns();
    const int outputChannelsForSilence = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));

    float currentGateGain = 1.0f;
    int gateHoldFrames = 0;

    while (running_) {
        UINT32 packetFrames = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
        bool capturedPacket = false;
        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            int outputChannels = 0;
            const int estimatedChannels = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));
            auto pcm = packets_.acquirePayload(static_cast<std::size_t>(frames) * estimatedChannels * sizeof(int16_t));
            convertToS16(data, frames, flags, mixFormat, outputChannels, *pcm);

            if (sourceId == "microphone-pcm") {
                const float vol = micVolume_.load();
                const bool isolation = micIsolation_.load();
                const float weight = micIsolationWeight_.load();
                const bool gateEnabled = noiseGateEnabled_.load();
                const bool autoGate = autoNoiseGate_.load();
                const float gateThreshold = noiseGateThreshold_.load();
                const int gateDebounceMs = noiseGateDebounceMs_.load();
                
                auto* pcm16 = reinterpret_cast<int16_t*>(pcm->data());
                
                for (UINT32 i = 0; i < frames; ++i) {
                    float mono = 0.0f;
                    for (int ch = 0; ch < outputChannels; ++ch) {
                        mono += pcm16[i * outputChannels + ch];
                    }
                    mono /= outputChannels;
                    rnnoiseQueue.push(mono);
                }
                
                while (rnnoiseQueue.popFrame(rnnoiseFrameIn)) {
                    const bool needsRnnoise = isolation || (gateEnabled && autoGate);
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
                        int16_t outVal = static_cast<int16_t>(std::clamp(blended, -32768.0f, 32767.0f));
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
                    packets_.push(std::move(packet));
                    
                    nextPts100ns += packet.duration100ns;
                    ++packetsCaptured_;
                }
                maybeLogRnnoiseQueueDepth(sourceId, rnnoiseQueue, rnnoiseFrameSize, lastRnnoiseQueueLog);
                
                capturedPacket = true;
                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
                continue;
            }

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
            packets_.push(std::move(packet));
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
            int fills = 0;
            while (running_ && nextPts100ns <= wallNow && fills < 5) {
                const UINT32 frames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
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
                packets_.push(std::move(packet));
                nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                ++packetsCaptured_;
                ++fills;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    audioClient->Stop();
    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    CoTaskMemFree(mixFormat);
    if (rnnoiseState) rnnoise_destroy(rnnoiseState);
    if (coInitialized) CoUninitialize();
}

void AudioCaptureWorker::runProcessLoopbackCapture(const std::string& processName, std::shared_ptr<std::atomic<bool>> stopRequested) {
    const std::string captureName = captureProcessName(processName);
    const std::string sourceId = sourceIdForProcessSpec(processName);
    std::cerr << "[audio] Process loopback thread started for: " << captureName << " as " << sourceId << std::endl;

    // Retry finding the process a few times — the process may still be starting up
    // when the audio worker launches, or the snapshot may miss it briefly.
    DWORD processId = 0;
    constexpr int maxProcessLookupAttempts = 3;
    for (int attempt = 0; attempt < maxProcessLookupAttempts && running_ && !stopRequested->load(); ++attempt) {
        processId = findProcessIdByName(captureName);
        if (processId != 0) break;
        std::cerr << "[audio] Process not found (attempt " << (attempt + 1) << "/" << maxProcessLookupAttempts << "): " << captureName << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (processId == 0) {
        std::cerr << "[audio] FAILED: Process not found after retries: " << captureName << std::endl;
        status_ = "Configured app audio source is not running: " + captureName;
        return;
    }
    std::cerr << "[audio] Found process " << captureName << " with PID " << processId << std::endl;

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

    constexpr REFERENCE_TIME bufferDuration = 1'000'000;
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, mixFormat, nullptr);
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

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    audioClient->Start();
    int64_t nextPts100ns = now100ns();
    const int outputChannelsForSilence = std::min<int>(2, std::max<int>(1, mixFormat->nChannels));

    std::cerr << "[audio] Process loopback capture RUNNING for " << captureName
              << " (PID " << processId << ", " << mixFormat->nSamplesPerSec << " Hz, "
              << mixFormat->nChannels << " ch)" << std::endl;

    while (running_ && !stopRequested->load()) {
        UINT32 packetFrames = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) break;
        bool capturedPacket = false;
        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

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
            packets_.push(std::move(packet));
            nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
            ++packetsCaptured_;
            capturedPacket = true;

            captureClient->ReleaseBuffer(frames);
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) packetFrames = 0;
        }
        // Fill silence when no packets arrive, keeping the PTS clock aligned
        // with the system/mic capture threads. Without this, the app audio track
        // can have gaps in timestamps and may be trimmed from the ring buffer.
        if (!capturedPacket) {
            const int64_t wallNow = now100ns();
            int fills = 0;
            while (running_ && !stopRequested->load() && nextPts100ns <= wallNow && fills < 5) {
                const UINT32 frames = std::max<UINT32>(1, mixFormat->nSamplesPerSec / 100);
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
                packets_.push(std::move(packet));
                nextPts100ns += static_cast<int64_t>((10'000'000.0 * frames) / mixFormat->nSamplesPerSec);
                ++packetsCaptured_;
                ++fills;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    audioClient->Stop();
    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    CoTaskMemFree(mixFormat);
    if (coInitialized) CoUninitialize();
}

}  // namespace clipture

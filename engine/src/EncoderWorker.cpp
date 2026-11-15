#include "clipture/EncoderWorker.hpp"
#include "clipture/EncoderPipelinePolicy.hpp"
#include "clipture/FreshConversionPolicy.hpp"
#include "Nv12ConversionTarget.hpp"
#include "clipture/CapturePipelinePolicy.hpp"
#include "clipture/CatchUpSelection.hpp"
#include "clipture/DeferredFramePreparation.hpp"
#include "clipture/EncoderCadence.hpp"
#include "clipture/DeadlineWait.hpp"
#include "VramTrace.hpp"
#include "clipture/platform/ThreadName.hpp"
#include "clipture/ResourceSizingPolicy.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/H264PacketAnalyzer.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/NvencRateControl.hpp"
#include "GpuFrameHandoff.hpp"
#include "GpuStageProbe.hpp"
#include "SharedTextureReader.hpp"
#include "EncoderSourceSnapshot.hpp"

#include <clipture/FixedRateFrameSampler.hpp>

#include <Windows.h>
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <span>
#include <iostream>
#include <iterator>
#include <vector>

namespace clipture {
namespace {

constexpr uint32_t kBundledNvencHeaderApiMajor = NVENCAPI_MAJOR_VERSION;
constexpr uint32_t kBundledNvencHeaderApiMinor = NVENCAPI_MINOR_VERSION;
constexpr uint32_t kBundledNvencHeaderApiRaw = NVENCAPI_VERSION;
constexpr uint32_t kNvencStructSignature = 0x7u;
constexpr uint32_t kNvencExtendedStructFlag = 1u << 31;

// OBS's Windows NVENC path uses asynchronous event-driven encoding.
// Async event mode decouples NVENC hardware latency from the scheduler and
// prevents blocking the CPU submission loop.
constexpr bool kPreferObsStyleBufferedSyncNvenc = false;

// Shared versus isolated input devices are an engineering A/B policy. Sharing
// avoids cross-device transfers; isolation avoids passing the DXGI device into
// NVENC but must pay for an explicitly synchronized transfer.

constexpr bool kEnableEncoderGpuSchedulingHeadroom = true;
constexpr bool kEnableRoundTransitionOutputHeadroom = true;
constexpr UINT kEncoderMaximumFrameLatency = 16;

// Fault injection for stress trials: CLIPTURE_TEST_ENCODER_STALL="<ms>,<periodMs>"
// delays one cadence wake by <ms> every <periodMs>, as a busy game does.
void injectTestEncoderStall() {
    static const auto config = [] {
        std::pair<int, int> result {0, 0};
        if (const char* value = std::getenv("CLIPTURE_TEST_ENCODER_STALL")) {
            std::sscanf(value, "%d,%d", &result.first, &result.second);
        }
        return result;
    }();
    static int64_t lastStall100ns = 0;
    if (config.first <= 0 || config.second <= 0) return;
    const auto now = monotonicNow100ns();
    if (now - lastStall100ns < int64_t {config.second} * 10'000) return;
    lastStall100ns = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(config.first));
}

constexpr std::size_t kDefaultNvencOutputSlots = 32;
constexpr std::size_t kRoundTransitionNvencOutputSlots = 32;
constexpr std::size_t kDefaultSyncOutputDelay = 3;
// Slots past the warm set allocate their bitstream buffer and input surface on
// first use and free them once idle, so burst headroom only costs memory while
// a burst is actually using it.
std::size_t warmNvencOutputSlots() {
    return static_cast<std::size_t>(resourceSizingPolicy().nvencWarmBitstreams);
}
constexpr int64_t kIdleNvencOutputSlotRelease100ns = 3LL * 10'000'000LL;
// Input views and registrations pin their textures; drop ones the capture pool
// no longer hands out so freed capture slots actually release memory.
constexpr int64_t kIdleEncoderInputRelease100ns = 2LL * 10'000'000LL;
constexpr std::size_t kRoundTransitionSyncOutputDelay = 8;

using NvEncodeApiCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using NvEncodeApiGetMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

std::string hex32(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string statusName(NVENCSTATUS status) {
    switch (status) {
        case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
        case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NV_ENC_ERR_NO_ENCODE_DEVICE";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_INVALID_DEVICE: return "NV_ENC_ERR_INVALID_DEVICE";
        case NV_ENC_ERR_DEVICE_NOT_EXIST: return "NV_ENC_ERR_DEVICE_NOT_EXIST";
        case NV_ENC_ERR_INVALID_PTR: return "NV_ENC_ERR_INVALID_PTR";
        case NV_ENC_ERR_INVALID_EVENT: return "NV_ENC_ERR_INVALID_EVENT";
        case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
        case NV_ENC_ERR_INVALID_CALL: return "NV_ENC_ERR_INVALID_CALL";
        case NV_ENC_ERR_OUT_OF_MEMORY: return "NV_ENC_ERR_OUT_OF_MEMORY";
        case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
        case NV_ENC_ERR_UNSUPPORTED_PARAM: return "NV_ENC_ERR_UNSUPPORTED_PARAM";
        case NV_ENC_ERR_LOCK_BUSY: return "NV_ENC_ERR_LOCK_BUSY";
        case NV_ENC_ERR_NOT_ENOUGH_BUFFER: return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
        case NV_ENC_ERR_INVALID_VERSION: return "NV_ENC_ERR_INVALID_VERSION";
        case NV_ENC_ERR_MAP_FAILED: return "NV_ENC_ERR_MAP_FAILED";
        case NV_ENC_ERR_NEED_MORE_INPUT: return "NV_ENC_ERR_NEED_MORE_INPUT";
        case NV_ENC_ERR_ENCODER_BUSY: return "NV_ENC_ERR_ENCODER_BUSY";
        case NV_ENC_ERR_EVENT_NOT_REGISTERD: return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
        case NV_ENC_ERR_GENERIC: return "NV_ENC_ERR_GENERIC";
        case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY: return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
        case NV_ENC_ERR_UNIMPLEMENTED: return "NV_ENC_ERR_UNIMPLEMENTED";
        case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
        case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
        case NV_ENC_ERR_RESOURCE_NOT_MAPPED: return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
        case NV_ENC_ERR_NEED_MORE_OUTPUT: return "NV_ENC_ERR_NEED_MORE_OUTPUT";
        default: return "NVENC_UNKNOWN_STATUS";
    }
}

std::string statusDetails(NVENCSTATUS status) {
    std::ostringstream out;
    const auto code = static_cast<uint32_t>(status);
    out << statusName(status)
        << "(code=" << static_cast<int>(status)
        << ", raw=" << hex32(code) << ")";
    return out.str();
}

uint32_t nvencDriverApiMajor(uint32_t version) {
    return version >> 4;
}

uint32_t nvencDriverApiMinor(uint32_t version) {
    return version & 0x0Fu;
}

std::string nvencDriverApiVersionName(uint32_t version) {
    return std::to_string(nvencDriverApiMajor(version)) + "." + std::to_string(nvencDriverApiMinor(version));
}

uint32_t nvencDriverApiVersionRaw(uint32_t major, uint32_t minor) {
    return (major << 4) | minor;
}

uint32_t nvencHeaderApiVersionRaw(uint32_t major, uint32_t minor) {
    return major | (minor << 24);
}

uint32_t nvencHeaderApiMajor(uint32_t version) {
    return version & 0x00FFFFFFu;
}

uint32_t nvencHeaderApiMinor(uint32_t version) {
    return version >> 24;
}

std::string nvencHeaderApiVersionName(uint32_t version) {
    return std::to_string(nvencHeaderApiMajor(version)) + "." +
        std::to_string(nvencHeaderApiMinor(version));
}

std::string nvencDriverApiVersionLabel(uint32_t version) {
    if (version == 0) return "unknown";
    return nvencDriverApiVersionName(version) + "(" + hex32(version) + ")";
}

std::string nvencHeaderApiVersionLabel(uint32_t version) {
    return nvencHeaderApiVersionName(version) + "(" + hex32(version) + ")";
}

bool chooseNvencApiVersion(uint32_t driverMaxVersion, uint32_t& apiVersion, std::string& reason) {
    if (driverMaxVersion == 0) {
        reason = "NVIDIA driver did not report a maximum NVENC API version.";
        return false;
    }

    const uint32_t driverMajor = nvencDriverApiMajor(driverMaxVersion);
    const uint32_t driverMinor = nvencDriverApiMinor(driverMaxVersion);
    if (driverMajor < kBundledNvencHeaderApiMajor) {
        reason = "NVIDIA driver NVENC API " + nvencDriverApiVersionLabel(driverMaxVersion) +
            " is older than this build requires (" + std::to_string(kBundledNvencHeaderApiMajor) + ".0+).";
        return false;
    }

    const uint32_t selectedMinor = driverMajor > kBundledNvencHeaderApiMajor
        ? kBundledNvencHeaderApiMinor
        : std::min(driverMinor, kBundledNvencHeaderApiMinor);
    apiVersion = nvencHeaderApiVersionRaw(kBundledNvencHeaderApiMajor, selectedMinor);
    reason = "driverMaxApi=" + nvencDriverApiVersionLabel(driverMaxVersion) +
        " selectedApi=" + nvencHeaderApiVersionLabel(apiVersion);
    return true;
}

uint32_t nvencStructVersionForApi(uint32_t version, uint32_t apiVersion, bool extended = false) {
    return apiVersion | (version << 16) | (kNvencStructSignature << 28) |
        (extended ? kNvencExtendedStructFlag : 0u);
}

GUID nvencPresetGuid(int preset) {
    switch (std::clamp(preset, 1, 5)) {
        case 1: return NV_ENC_PRESET_P1_GUID;
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        case 5: return NV_ENC_PRESET_P5_GUID;
        case 3:
        default: return NV_ENC_PRESET_P3_GUID;
    }
}

GUID legacyNvencPresetGuid(int preset) {
    static const GUID NV_ENC_PRESET_LEGACY_LOW_LATENCY_DEFAULT_GUID = {0x49df21c5, 0x6afa, 0x47d4, {0xb1, 0x59, 0x5f, 0x4a, 0xbb, 0x11, 0xbb, 0xcb}};
    static const GUID NV_ENC_PRESET_LEGACY_LOW_LATENCY_HQ_GUID = {0xa70176cd, 0xa9bc, 0x41e0, {0xb1, 0x76, 0xb7, 0xd2, 0x72, 0x24, 0xb5, 0x82}};
    static const GUID NV_ENC_PRESET_LEGACY_LOW_LATENCY_HP_GUID = {0xc5f733b9, 0xea97, 0x4cf9, {0xbe, 0xca, 0xa4, 0x6b, 0xa6, 0x7f, 0xbe, 0x8a}};
    
    switch (std::clamp(preset, 1, 5)) {
        case 1:
        case 2:
            return NV_ENC_PRESET_LEGACY_LOW_LATENCY_HP_GUID;
        case 3:
        case 4:
            return NV_ENC_PRESET_LEGACY_LOW_LATENCY_DEFAULT_GUID;
        case 5:
        default:
            return NV_ENC_PRESET_LEGACY_LOW_LATENCY_HQ_GUID;
    }
}

std::string nvencPresetName(int preset) {
    switch (std::clamp(preset, 1, 5)) {
        case 1: return "P1 fastest";
        case 2: return "P2 low resource";
        case 4: return "P4 quality";
        case 5: return "P5 higher quality";
        case 3:
        default: return "P3 balanced";
    }
}

std::string nvencTuningName(NV_ENC_TUNING_INFO tuning) {
    switch (tuning) {
        case NV_ENC_TUNING_INFO_LOW_LATENCY: return "low-latency";
        case NV_ENC_TUNING_INFO_HIGH_QUALITY: return "high-quality";
        case NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY: return "ultra-low-latency";
        case NV_ENC_TUNING_INFO_LOSSLESS: return "lossless";
        case NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY: return "ultra-high-quality";
        case NV_ENC_TUNING_INFO_UNDEFINED:
        default: return "undefined";
    }
}

struct EncodeDimensions {
    int width = 0;
    int height = 0;
    bool capped = false;
};

int evenEncodeDimension(int value) {
    return std::max(2, value & ~1);
}

EncodeDimensions fitEncodeDimensions(int width, int height, int capWidth, int capHeight) {
    EncodeDimensions result {
        evenEncodeDimension(std::max(1, width)),
        evenEncodeDimension(std::max(1, height)),
        false,
    };

    if (capWidth > 0 && result.width > capWidth) {
        result.height = evenEncodeDimension((result.height * capWidth) / result.width);
        result.width = evenEncodeDimension(capWidth);
        result.capped = true;
    }
    if (capHeight > 0 && result.height > capHeight) {
        result.width = evenEncodeDimension((result.width * capHeight) / result.height);
        result.height = evenEncodeDimension(capHeight);
        result.capped = true;
    }
    return result;
}

class MmcssThreadRegistration {
public:
    explicit MmcssThreadRegistration(const wchar_t* taskName, AVRT_PRIORITY priority) {
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex_);
        if (handle_) AvSetMmThreadPriority(handle_, priority);
    }

    ~MmcssThreadRegistration() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }

private:
    DWORD taskIndex_ = 0;
    HANDLE handle_ = nullptr;
};

struct NvencFrameTimings {
    enum class InputPath {
        None,
        DirectCaptureBgra,
        CachedSurface,
        BgraCopyFallback,
        VideoProcessor,
    };

    int64_t scale100ns = 0;
    int64_t inputMap100ns = 0;
    int64_t encodeCall100ns = 0;
    InputPath inputPath = InputPath::None;
};

struct NvencOutputTimings {
    int64_t lock100ns = 0;
    int64_t copy100ns = 0;
    int64_t unmap100ns = 0;
    uint32_t lockBusyRetries = 0;
};

void updateMaximum(std::atomic<int64_t>& maximum, int64_t value) {
    int64_t previous = maximum.load(std::memory_order_relaxed);
    while (value > previous &&
           !maximum.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

class NvencSession {
public:
    NvencSession(
        PacketRingBuffer& packetPool,
        std::atomic<int>* externalInFlightCount,
        std::atomic<int>* externalDropCount,
        std::atomic<int>* externalSurfaceDropCount,
        std::atomic<int>* externalInputDropCount,
        SlotPoolUsage* outputSlotUsage,
        LatencyWindow<>* outputEventWaitLatency,
        LatencyWindow<>* outputLockLatency,
        LatencyWindow<>* outputCopyLatency,
        LatencyWindow<>* outputUnmapLatency)
        : packetPool_(packetPool),
          externalInFlightCount_(externalInFlightCount),
          externalDropCount_(externalDropCount),
          externalSurfaceDropCount_(externalSurfaceDropCount),
          externalInputDropCount_(externalInputDropCount),
          outputSlotUsage_(outputSlotUsage),
          outputEventWaitLatency_(outputEventWaitLatency),
          outputLockLatency_(outputLockLatency),
          outputCopyLatency_(outputCopyLatency),
          outputUnmapLatency_(outputUnmapLatency) {}

    ~NvencSession() {
        destroy();
    }

#include "NvencSessionInit.inl"  // NvencSession: initialize
    bool isInitialized() const {
        return initialized_;
    }

    // First failing removed reason of the encoder's or capture's device.
    HRESULT deviceRemovedReason() const {
        if (device_) {
            const HRESULT hr = device_->GetDeviceRemovedReason();
            if (FAILED(hr)) return hr;
        }
        if (captureDevice_) {
            const HRESULT hr = captureDevice_->GetDeviceRemovedReason();
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }

    int outputWidth() const {
        return width_;
    }

    int outputHeight() const {
        return height_;
    }

#include "NvencSessionSubmit.inl"  // NvencSession: encode, submit and drain
private:
    struct RegisteredInput {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registeredResource = nullptr;
        NV_ENC_BUFFER_FORMAT bufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
        int width = 0;
        int height = 0;
        int64_t lastUsed100ns = 0;
    };

    struct CachedVideoInputView {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
        uint64_t scalerGeneration = 0;
        int64_t lastUsed100ns = 0;
    };

    struct OutputSlot {
        NV_ENC_OUTPUT_PTR bitstreamBuffer = nullptr;
        HANDLE completionEvent = nullptr;
        bool eventRegistered = false;
        bool inFlight = false;
        NV_ENC_INPUT_PTR mappedInput = nullptr;
        NV_ENC_REGISTERED_PTR mappedRegisteredResource = nullptr;
        int64_t framePts100ns = 0;
        int64_t frameDuration100ns = 0;
        int frameEncodedWidth = 0;
        int frameEncodedHeight = 0;
        int frameSourceWidth = 0;
        int frameSourceHeight = 0;
        uint64_t captureEpoch = 0;
        uint64_t sourceFrameSequence = 0;
        bool sourceHadDesktopPresent = false;
        bool sourceHadPointerUpdate = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> retainedInputTexture;
        std::shared_ptr<void> retainedInputLease;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> scaledTexture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> scaledOutputView;
        uint64_t scaledViewGeneration = 0;
        uint64_t surfaceCaptureEpoch = 0;
        uint64_t surfaceSourceFrameSequence = 0;
        int64_t lastReserved100ns = 0;
    };

    struct PreparedSubmission {
        OutputSlot* slot = nullptr;
        NV_ENC_REGISTERED_PTR registeredResource = nullptr;
        NV_ENC_BUFFER_FORMAT bufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    };

#include "NvencSessionSharedInput.inl"  // NvencSession: D3D devices and shared-frame input
#include "NvencSessionOutputThread.inl"  // NvencSession: async output thread
#include "NvencSessionFramePreparation.inl"  // NvencSession: per-frame input preparation and scaling
#include "NvencSessionSlots.inl"  // NvencSession: output slots, input registration, draining and teardown
    HMODULE module_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST funcs_ {};
    Microsoft::WRL::ComPtr<ID3D11Device> captureDevice_;
    GpuFrameHandoff gpuHandoff_;
    SharedTextureReader directReader_;
    EncoderSourceSnapshot sourceSnapshot_;
    bool earlyRetireLogged_ = false;
    bool directReadUnavailable_ = false, directReadLogged_ = false;
    GpuStageProbe conversionProbe_ {"canonical-conversion"};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> captureContext_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11Device> bridgeCaptureDevice_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> captureBridgeTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> encoderBridgeTexture_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> captureBridgeMutex_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> encoderBridgeMutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> canonicalTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> canonicalOutputView_;
    PacketRingBuffer& packetPool_;
    std::atomic<int>* externalInFlightCount_ = nullptr;
    std::atomic<int>* externalDropCount_ = nullptr;
    std::atomic<int>* externalSurfaceDropCount_ = nullptr;
    std::atomic<int>* externalInputDropCount_ = nullptr;
    SlotPoolUsage* outputSlotUsage_ = nullptr;
    std::atomic<int> allocatedOutputSlots_ = 0;
    LatencyWindow<>* outputEventWaitLatency_ = nullptr;
    LatencyWindow<>* outputLockLatency_ = nullptr;
    LatencyWindow<>* outputCopyLatency_ = nullptr;
    LatencyWindow<>* outputUnmapLatency_ = nullptr;
    void* encoder_ = nullptr;
    NV_ENC_CONFIG encodeConfig_ {};
    NV_ENC_INITIALIZE_PARAMS initParams_ {};
    GUID presetGuid_ {};
    std::vector<RegisteredInput> registeredInputs_;
    std::vector<CachedVideoInputView> inputViews_;
    std::vector<OutputSlot> outputSlots_;
    std::deque<PreparedSubmission> preparedSubmissions_;
    std::deque<std::size_t> inFlightOrder_;
    std::vector<EncodedPacket> completedPackets_;
    std::thread outputThread_;
    mutable std::mutex outputMutex_;
    std::condition_variable outputCv_;
    std::atomic<bool> outputThreadRunning_ = false;
    std::atomic<bool> outputFatal_ = false;
    std::atomic<int> inFlightCount_ = 0;
    std::atomic<uint64_t> outputDrainSamples_ = 0;
    std::atomic<int64_t> totalOutputDrainLatency100ns_ = 0;
    std::atomic<int64_t> maximumOutputDrainLatency100ns_ = 0;
    std::string outputFailure_;
    bool asyncEnabled_ = false;
    bool initialized_ = false;
    bool initFailureLogged_ = false;
    uint32_t apiVersion_ = kBundledNvencHeaderApiRaw;
    int requestedWidth_ = 0;
    int requestedHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrateMbps_ = 40;
    int maxEncodeWidth_ = 0;
    int64_t lastVramTrace100ns_ = 0;
    int maxEncodeHeight_ = 0;
    int h264CapWidth_ = 0;
    int h264CapHeight_ = 0;
    int scalerInputWidth_ = 0;
    int scalerInputHeight_ = 0;
    int scalerOutputWidth_ = 0;
    int scalerOutputHeight_ = 0;
    uint64_t scalerGeneration_ = 0;
    DXGI_FORMAT scalerOutputFormat_ = DXGI_FORMAT_UNKNOWN;
    bool preferNv12Input_ = false;
    bool bgraFallbackLogged_ = false;
    bool supportsNv12Input_ = false;
    bool dedicatedInputPathLogged_ = false;
    bool nv12RegistrationFallbackLogged_ = false;
    bool supportsArgbInput_ = false;
    bool supportsDirectCaptureInput_ = false;
    bool directCaptureFallbackLogged_ = false;
    bool directBgraFallbackLogged_ = false;
    bool separateEncoderDevice_ = false;
    UINT bridgeWidth_ = 0;
    UINT bridgeHeight_ = 0;
    DXGI_FORMAT bridgeFormat_ = DXGI_FORMAT_UNKNOWN;
    uint64_t canonicalCaptureEpoch_ = 0;
    uint64_t canonicalSourceFrameSequence_ = 0;
    uint64_t canonicalScalerGeneration_ = 0;
    FreshConversionPolicy freshConversion_;
    bool directFreshLogged_ = false;
    uint32_t frameIndex_ = 0;
    int64_t nextKeyframePts100ns_ = 0;
    static constexpr std::size_t outputSlotCount_ = kEnableRoundTransitionOutputHeadroom
        ? kRoundTransitionNvencOutputSlots
        : kDefaultNvencOutputSlots;
    // Keep all encoder-owned surfaces registered plus the capture texture pool cache.
    static constexpr std::size_t maxRegisteredInputs_ = 64;
    static constexpr std::size_t maximumInputViewCacheSize_ = 16;
    static constexpr std::size_t minimumPreparedSubmissionDepth_ = 2;
    static constexpr std::size_t maximumPreparedSubmissionDepth_ = outputSlotCount_ / 2;
    std::size_t preparedSubmissionDepth_ = minimumPreparedSubmissionDepth_;
    std::size_t syncOutputDelay_ = kDefaultSyncOutputDelay;
    static constexpr bool useDedicatedInputSurfaces_ = true;
    static constexpr DWORD asyncDrainWaitTimeoutMs_ = 1000;
    static constexpr int64_t asyncDrainWaitTimeout100ns_ =
        static_cast<int64_t>(asyncDrainWaitTimeoutMs_) * 10'000LL;
    static constexpr auto outputSlotAvailabilityWait_ = std::chrono::milliseconds(20);
};

}  // namespace

#include "EncoderWorkerState.inl"  // EncoderWorker: lifecycle, configuration and counters
#include "EncoderWorkerLoops.inl"  // EncoderWorker: scheduler and encode loops
}  // namespace clipture

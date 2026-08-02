#include "clipture/EncoderWorker.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/EncoderQueuePolicy.hpp"
#include "clipture/H264PacketAnalyzer.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/VideoTimeline.hpp"

#include <Windows.h>
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <span>
#include <iostream>
#include <vector>

namespace clipture {
namespace {

constexpr uint32_t kBundledNvencHeaderApiMajor = NVENCAPI_MAJOR_VERSION;
constexpr uint32_t kBundledNvencHeaderApiMinor = NVENCAPI_MINOR_VERSION;
constexpr uint32_t kBundledNvencHeaderApiRaw = NVENCAPI_VERSION;
constexpr uint32_t kNvencStructSignature = 0x7u;
constexpr uint32_t kNvencExtendedStructFlag = 1u << 31;

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
        LatencyWindow<>* outputEventWaitLatency,
        LatencyWindow<>* outputLockLatency,
        LatencyWindow<>* outputCopyLatency,
        LatencyWindow<>* outputUnmapLatency)
        : packetPool_(packetPool),
          externalInFlightCount_(externalInFlightCount),
          externalDropCount_(externalDropCount),
          externalSurfaceDropCount_(externalSurfaceDropCount),
          externalInputDropCount_(externalInputDropCount),
          outputEventWaitLatency_(outputEventWaitLatency),
          outputLockLatency_(outputLockLatency),
          outputCopyLatency_(outputCopyLatency),
          outputUnmapLatency_(outputUnmapLatency) {}

    ~NvencSession() {
        destroy();
    }

    bool initialize(
        ID3D11Texture2D* texture,
        int width,
        int height,
        int outputWidth,
        int outputHeight,
        int maxEncodeWidth,
        int maxEncodeHeight,
        int fps,
        int bitrateMbps,
        int nvencPreset,
        std::string& status) {
        if (initialized_) return true;
        (void)width;
        (void)height;
        if (!texture) {
            status = "NVENC initialize failed: first frame texture is null.";
            return false;
        }

        if (!module_) {
            module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
            if (!module_) module_ = LoadLibraryW(L"nvEncodeAPI.dll");
        }
        if (!module_) {
            status = "NVENC initialize failed: nvEncodeAPI DLL not found.";
            return false;
        }
        auto createInstance = reinterpret_cast<NvEncodeApiCreateInstance>(GetProcAddress(module_, "NvEncodeAPICreateInstance"));
        if (!createInstance) {
            status = "NVENC initialize failed: NvEncodeAPICreateInstance missing.";
            return false;
        }
        const auto getMaxSupportedVersion = reinterpret_cast<NvEncodeApiGetMaxSupportedVersion>(
            GetProcAddress(module_, "NvEncodeAPIGetMaxSupportedVersion"));
        uint32_t driverMaxApiVersion = 0;
        if (getMaxSupportedVersion) {
            const NVENCSTATUS versionStatus = getMaxSupportedVersion(&driverMaxApiVersion);
            if (versionStatus != NV_ENC_SUCCESS) {
                driverMaxApiVersion = 0;
                if (!initFailureLogged_) {
                    std::cerr << "[encoder] NvEncodeAPIGetMaxSupportedVersion failed: "
                              << statusDetails(versionStatus) << std::endl;
                }
            }
        }
        uint32_t selectedApiVersion = 0;
        std::string apiSelection;
        if (!chooseNvencApiVersion(driverMaxApiVersion, selectedApiVersion, apiSelection)) {
            status = "NVENC initialize failed: " + apiSelection;
            if (!initFailureLogged_) {
                std::cerr << "[encoder] " << status << std::endl;
                initFailureLogged_ = true;
            }
            return false;
        }

        apiVersion_ = selectedApiVersion;
        funcs_ = {};
        funcs_.version = nvencStructVersionForApi(2, apiVersion_);
        NVENCSTATUS nvStatus = createInstance(&funcs_);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncodeAPICreateInstance failed: " + statusDetails(nvStatus) + " " + apiSelection;
            if (!initFailureLogged_) {
                std::cerr << "[encoder] " << status << std::endl;
                initFailureLogged_ = true;
            }
            return false;
        }

        device_.Reset();
        texture->GetDevice(&device_);
        if (!device_) {
            status = "NVENC initialize failed: could not get D3D11 device from captured texture.";
            return false;
        }

        const int boundedPreset = std::clamp(nvencPreset, 1, 5);
        struct InitAttempt {
            GUID presetGuid;
            NV_ENC_TUNING_INFO tuningInfo;
            bool async;
            const char* presetFamily;
        };
        const InitAttempt attempts[] = {
            { nvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_LOW_LATENCY, true, "modern" },
            { nvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_LOW_LATENCY, false, "modern" },
            { legacyNvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_UNDEFINED, true, "legacy" },
            { legacyNvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_UNDEFINED, false, "legacy" },
        };
        auto logInitAttemptFailure = [this](const InitAttempt& attempt, const std::string& reason) {
            if (initFailureLogged_) return;
            std::cerr << "[encoder] NVENC init attempt failed"
                      << " presetFamily=" << attempt.presetFamily
                      << " tuning=" << nvencTuningName(attempt.tuningInfo)
                      << " async=" << (attempt.async ? "true" : "false")
                      << " error=\"" << reason << "\""
                      << std::endl;
        };

        std::string lastFailure;
        for (const auto& attempt : attempts) {
            destroyEncoderResources();
            apiVersion_ = selectedApiVersion;

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams {};
            openParams.version = nvencStructVersionForApi(1, apiVersion_);
            openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
            openParams.device = device_.Get();
            openParams.apiVersion = apiVersion_;
            nvStatus = funcs_.nvEncOpenEncodeSessionEx(&openParams, &encoder_);
            if (nvStatus != NV_ENC_SUCCESS) {
                lastFailure = "NvEncOpenEncodeSessionEx failed: " + statusDetails(nvStatus) + " " + apiSelection;
                logInitAttemptFailure(attempt, lastFailure);
                continue;
            }

            supportsNv12Input_ = supportsInputFormat(NV_ENC_BUFFER_FORMAT_NV12);
            preferNv12Input_ = supportsNv12Input_;
            supportsArgbInput_ = supportsInputFormat(NV_ENC_BUFFER_FORMAT_ARGB);
            supportsDirectCaptureInput_ = supportsArgbInput_;
            const int asyncSupport = queryEncodeCap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
            if (attempt.async && asyncSupport == 0) {
                lastFailure = "NVENC device reports async encode is unsupported.";
                logInitAttemptFailure(attempt, lastFailure);
                continue;
            }
            const int capWidth = queryEncodeCap(NV_ENC_CAPS_WIDTH_MAX);
            const int capHeight = queryEncodeCap(NV_ENC_CAPS_HEIGHT_MAX);
            const int requestedWidth = std::max(1, outputWidth);
            const int requestedHeight = std::max(1, outputHeight);
            const auto encodeSize = fitEncodeDimensions(requestedWidth, requestedHeight, capWidth, capHeight);

            presetGuid_ = attempt.presetGuid;

            NV_ENC_PRESET_CONFIG presetConfig {};
            presetConfig.version = nvencStructVersionForApi(5, apiVersion_, true);
            presetConfig.presetCfg.version = nvencStructVersionForApi(9, apiVersion_, true);
            bool usedLegacyPresetConfigApi = false;
            nvStatus = funcs_.nvEncGetEncodePresetConfigEx
                ? funcs_.nvEncGetEncodePresetConfigEx(
                    encoder_,
                    NV_ENC_CODEC_H264_GUID,
                    presetGuid_,
                    attempt.tuningInfo,
                    &presetConfig)
                : NV_ENC_ERR_UNIMPLEMENTED;
            if (nvStatus != NV_ENC_SUCCESS &&
                std::strcmp(attempt.presetFamily, "legacy") == 0 &&
                funcs_.nvEncGetEncodePresetConfig) {
                presetConfig = {};
                presetConfig.version = nvencStructVersionForApi(5, apiVersion_, true);
                presetConfig.presetCfg.version = nvencStructVersionForApi(9, apiVersion_, true);
                nvStatus = funcs_.nvEncGetEncodePresetConfig(
                    encoder_,
                    NV_ENC_CODEC_H264_GUID,
                    presetGuid_,
                    &presetConfig);
                usedLegacyPresetConfigApi = nvStatus == NV_ENC_SUCCESS;
            }
            if (nvStatus != NV_ENC_SUCCESS) {
                lastFailure = "NvEncGetEncodePresetConfigEx failed: " + statusDetails(nvStatus);
                logInitAttemptFailure(attempt, lastFailure);
                continue;
            }

            encodeConfig_ = presetConfig.presetCfg;
            encodeConfig_.gopLength = static_cast<uint32_t>(std::max(1, fps));
            encodeConfig_.frameIntervalP = 1;
            encodeConfig_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
            encodeConfig_.rcParams = {};
            encodeConfig_.rcParams.version = nvencStructVersionForApi(1, apiVersion_);
            encodeConfig_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig_.rcParams.averageBitRate =
                static_cast<uint32_t>(std::max(1, bitrateMbps) * 1'000'000);
            encodeConfig_.rcParams.maxBitRate = encodeConfig_.rcParams.averageBitRate;
            encodeConfig_.rcParams.enableLookahead = 0;
            encodeConfig_.rcParams.lookaheadDepth = 0;
            encodeConfig_.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
            encodeConfig_.rcParams.enableAQ = 0;
            encodeConfig_.rcParams.enableTemporalAQ = 0;
            encodeConfig_.rcParams.aqStrength = 0;
            encodeConfig_.rcParams.zeroReorderDelay = 1;
            auto& h264Config = encodeConfig_.encodeCodecConfig.h264Config;
            h264Config.idrPeriod = encodeConfig_.gopLength;
            h264Config.repeatSPSPPS = 1;
            h264Config.enableFillerDataInsertion = 0;
            h264Config.outputBufferingPeriodSEI = 0;
            h264Config.outputPictureTimingSEI = 0;
            h264Config.outputAUD = 0;
            h264Config.outputFramePackingSEI = 0;
            h264Config.outputRecoveryPointSEI = 0;
            h264Config.enableScalabilityInfoSEI = 0;
            h264Config.disableSVCPrefixNalu = 1;

            requestedWidth_ = requestedWidth;
            requestedHeight_ = requestedHeight;
            h264CapWidth_ = capWidth > 0 ? capWidth : 0;
            h264CapHeight_ = capHeight > 0 ? capHeight : 0;
            maxEncodeWidth_ = std::max(encodeSize.width, maxEncodeWidth);
            maxEncodeHeight_ = std::max(encodeSize.height, maxEncodeHeight);
            if (h264CapWidth_ > 0) maxEncodeWidth_ = std::min(maxEncodeWidth_, h264CapWidth_);
            if (h264CapHeight_ > 0) maxEncodeHeight_ = std::min(maxEncodeHeight_, h264CapHeight_);

            initParams_ = {};
            initParams_.version = nvencStructVersionForApi(7, apiVersion_, true);
            initParams_.encodeGUID = NV_ENC_CODEC_H264_GUID;
            initParams_.presetGUID = presetGuid_;
            initParams_.encodeWidth = static_cast<uint32_t>(encodeSize.width);
            initParams_.encodeHeight = static_cast<uint32_t>(encodeSize.height);
            initParams_.darWidth = initParams_.encodeWidth;
            initParams_.darHeight = initParams_.encodeHeight;
            initParams_.frameRateNum = static_cast<uint32_t>(std::max(1, fps));
            initParams_.frameRateDen = 1;
            asyncEnabled_ = attempt.async;
            initParams_.enableEncodeAsync = asyncEnabled_ ? 1 : 0;
            initParams_.enablePTD = 1;
            initParams_.tuningInfo = attempt.tuningInfo;
            initParams_.maxEncodeWidth = static_cast<uint32_t>(maxEncodeWidth_);
            initParams_.maxEncodeHeight = static_cast<uint32_t>(maxEncodeHeight_);
            initParams_.encodeConfig = &encodeConfig_;
            nvStatus = funcs_.nvEncInitializeEncoder(encoder_, &initParams_);
            if (nvStatus != NV_ENC_SUCCESS) {
                lastFailure = "NvEncInitializeEncoder failed: " + statusDetails(nvStatus);
                logInitAttemptFailure(attempt, lastFailure);
                continue;
            }

            if (!createOutputSlots(status)) {
                lastFailure = status;
                logInitAttemptFailure(attempt, lastFailure);
                continue;
            }

            width_ = encodeSize.width;
            height_ = encodeSize.height;
            fps_ = std::max(1, fps);
            bitrateMbps_ = std::max(1, bitrateMbps);
            preparedSubmissionDepth_ = static_cast<std::size_t>(std::clamp(
                boundedPreset + 1,
                2,
                static_cast<int>(maximumPreparedSubmissionDepth_)));
            initialized_ = true;
            if (asyncEnabled_) startOutputThread();
            initFailureLogged_ = false;
            status = asyncEnabled_
                ? "Direct NVENC H.264 async " + nvencPresetName(boundedPreset) + " session initialized."
                : "Direct NVENC H.264 sync compatibility " + nvencPresetName(boundedPreset) + " session initialized.";
            if (useDedicatedInputSurfaces_) {
                status += supportsNv12Input_
                    ? " Native-size frames use persistent per-slot NV12 input surfaces."
                    : " Native-size frames use persistent per-slot BGRA input surfaces because NV12 is unavailable.";
            } else if (supportsArgbInput_) {
                status += " Native-size frames use zero-copy BGRA input when the capture texture is available.";
            }
            status += " Input preparation depth is " +
                std::to_string(preparedSubmissionDepth_) + ".";
            status += " Single-pass low-resource rate control is active.";
            if (attempt.presetFamily == std::string("legacy")) {
                status += " Legacy preset fallback is active.";
            }
            if (encodeSize.capped) {
                status += " Output was fit to NVENC H.264 caps from " +
                    std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) +
                    " to " + std::to_string(width_) + "x" + std::to_string(height_) + ".";
            }
            (void)usedLegacyPresetConfigApi;
            return true;
        }

        destroyEncoderResources();
        status = lastFailure.empty()
            ? "NVENC initialize failed: no compatible H.264 initialization path worked."
            : "NVENC initialize failed: no compatible H.264 initialization path worked. Last error: " + lastFailure;
        if (!initFailureLogged_) {
            std::cerr << "[encoder] " << status << std::endl;
            initFailureLogged_ = true;
        }
        return false;
    }

    int outputWidth() const {
        return width_;
    }

    int outputHeight() const {
        return height_;
    }

    bool encode(
        CapturedFrame frame,
        std::vector<EncodedPacket>& packets,
        std::string& status,
        NvencFrameTimings& timings) {
        timings = {};
        if (!initialized_) {
            status = "NVENC encode called before initialization.";
            return false;
        }

        if (!drainReady(packets, status)) return false;

        auto* slot = freeOutputSlot(frame, packets, status);
        if (!slot) {
            if (externalDropCount_) externalDropCount_->fetch_add(1, std::memory_order_relaxed);
            if (externalSurfaceDropCount_) externalSurfaceDropCount_->fetch_add(1, std::memory_order_relaxed);
            status = "NVENC output pool remained busy after a bounded wait; dropping this frame.";
            return true;
        }

        const int64_t scaleStarted100ns = monotonicNow100ns();
        auto encodeTexture = prepareFrameForEncode(frame, *slot, timings.inputPath, status);
        timings.scale100ns = monotonicNow100ns() - scaleStarted100ns;
        if (!encodeTexture) {
            releaseOutputSlotReservation(*slot);
            return false;
        }
        const int64_t inputMapStarted100ns = monotonicNow100ns();
        auto* registeredInput = registeredInputFor(encodeTexture.Get(), width_, height_, status);
        if (!registeredInput &&
            timings.inputPath == NvencFrameTimings::InputPath::DirectCaptureBgra) {
            supportsDirectCaptureInput_ = false;
            if (!directCaptureFallbackLogged_) {
                std::cerr << "[encoder] Capture texture registration failed; using per-slot BGRA copy surfaces."
                          << std::endl;
                directCaptureFallbackLogged_ = true;
            }
            timings.inputPath = NvencFrameTimings::InputPath::None;
            encodeTexture = prepareFrameForEncode(frame, *slot, timings.inputPath, status);
            if (encodeTexture) {
                registeredInput = registeredInputFor(encodeTexture.Get(), width_, height_, status);
            }
        }
        if (!registeredInput &&
            timings.inputPath == NvencFrameTimings::InputPath::VideoProcessor &&
            preferNv12Input_) {
            preferNv12Input_ = false;
            supportsNv12Input_ = false;
            slot->scaledOutputView.Reset();
            slot->scaledTexture.Reset();
            slot->scaledViewGeneration = 0;
            if (!nv12RegistrationFallbackLogged_) {
                std::cerr << "[encoder] NV12 registration failed; using persistent per-slot BGRA input surfaces."
                          << std::endl;
                nv12RegistrationFallbackLogged_ = true;
            }
            timings.inputPath = NvencFrameTimings::InputPath::None;
            encodeTexture = prepareFrameForEncode(frame, *slot, timings.inputPath, status);
            if (encodeTexture) {
                registeredInput = registeredInputFor(encodeTexture.Get(), width_, height_, status);
            }
        }
        if (!registeredInput && timings.inputPath == NvencFrameTimings::InputPath::BgraCopyFallback) {
            supportsArgbInput_ = false;
            slot->scaledOutputView.Reset();
            slot->scaledTexture.Reset();
            slot->scaledViewGeneration = 0;
            if (!directBgraFallbackLogged_) {
                std::cerr << "[encoder] Direct BGRA registration failed; retaining the NV12 video-processor path."
                          << std::endl;
                directBgraFallbackLogged_ = true;
            }
            timings.inputPath = NvencFrameTimings::InputPath::None;
            encodeTexture = prepareFrameForEncode(frame, *slot, timings.inputPath, status);
            if (encodeTexture) {
                registeredInput = registeredInputFor(encodeTexture.Get(), width_, height_, status);
            }
        }
        if (!registeredInput) {
            timings.inputMap100ns = monotonicNow100ns() - inputMapStarted100ns;
            releaseOutputSlotReservation(*slot);
            return false;
        }
        timings.inputMap100ns = monotonicNow100ns() - inputMapStarted100ns;

        slot->framePts100ns = frame.pts100ns;
        slot->frameDuration100ns = 10'000'000LL / fps_;
        slot->frameEncodedWidth = width_;
        slot->frameEncodedHeight = height_;
        slot->frameSourceWidth = frame.width;
        slot->frameSourceHeight = frame.height;
        slot->captureEpoch = frame.captureEpoch;
        slot->sourceFrameSequence = frame.sequence;
        if (timings.inputPath == NvencFrameTimings::InputPath::DirectCaptureBgra) {
            slot->retainedInputTexture = frame.texture;
            slot->retainedInputLease = frame.textureLease;
        } else {
            // The conversion/copy was queued on the shared immediate context.
            // Later capture writes are ordered after it, so the capture slot no
            // longer needs to remain leased while NVENC consumes its own surface.
            frame.textureLease.reset();
            frame.texture.Reset();
        }
        preparedSubmissions_.push_back(PreparedSubmission {
            slot,
            registeredInput->registeredResource,
            registeredInput->bufferFormat,
        });

        if (preparedSubmissions_.size() < preparedSubmissionDepth_) {
            status = "Direct NVENC is preparing input frames.";
            return true;
        }
        return submitPrepared(packets, status, timings);
    }

    bool submitPrepared(
        std::vector<EncodedPacket>& packets,
        std::string& status,
        NvencFrameTimings& timings) {
        if (preparedSubmissions_.empty()) return true;
        const PreparedSubmission prepared = preparedSubmissions_.front();
        preparedSubmissions_.pop_front();
        auto* slot = prepared.slot;
        if (!slot || !prepared.registeredResource) {
            if (slot) releaseOutputSlotReservation(*slot);
            status = "NVENC prepared input was invalid.";
            return false;
        }
        if (inputResourceInFlight(prepared.registeredResource)) {
            releaseOutputSlotReservation(*slot);
            if (externalDropCount_) externalDropCount_->fetch_add(1, std::memory_order_relaxed);
            if (externalInputDropCount_) externalInputDropCount_->fetch_add(1, std::memory_order_relaxed);
            status = "NVENC input is still in flight; dropping this frame without blocking capture.";
            return true;
        }

        if (asyncEnabled_ && slot->completionEvent) {
            ResetEvent(slot->completionEvent);
        }

        NV_ENC_MAP_INPUT_RESOURCE mapped {};
        mapped.version = nvencStructVersionForApi(4, apiVersion_);
        mapped.registeredResource = prepared.registeredResource;
        const int64_t inputMapStarted100ns = monotonicNow100ns();
        NVENCSTATUS nvStatus = funcs_.nvEncMapInputResource(encoder_, &mapped);
        timings.inputMap100ns += monotonicNow100ns() - inputMapStarted100ns;
        if (nvStatus != NV_ENC_SUCCESS) {
            releaseOutputSlotReservation(*slot);
            status = "NvEncMapInputResource failed: " + statusDetails(nvStatus);
            return false;
        }

        NV_ENC_PIC_PARAMS pic {};
        pic.version = nvencStructVersionForApi(7, apiVersion_, true);
        pic.inputWidth = static_cast<uint32_t>(slot->frameEncodedWidth);
        pic.inputHeight = static_cast<uint32_t>(slot->frameEncodedHeight);
        pic.inputPitch = static_cast<uint32_t>(slot->frameEncodedWidth);
        pic.frameIdx = frameIndex_;
        pic.inputTimeStamp = static_cast<uint64_t>(slot->framePts100ns);
        pic.inputDuration = static_cast<uint64_t>(10'000'000LL / fps_);
        pic.inputBuffer = mapped.mappedResource;
        pic.outputBitstream = slot->bitstreamBuffer;
        pic.completionEvent = asyncEnabled_ ? slot->completionEvent : nullptr;
        pic.bufferFmt = mapped.mappedBufferFmt == NV_ENC_BUFFER_FORMAT_UNDEFINED
            ? prepared.bufferFormat
            : mapped.mappedBufferFmt;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (nextKeyframePts100ns_ == 0 || slot->framePts100ns >= nextKeyframePts100ns_) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            nextKeyframePts100ns_ = slot->framePts100ns + 10'000'000LL;
        }

        const int64_t encodeCallStarted100ns = monotonicNow100ns();
        nvStatus = funcs_.nvEncEncodePicture(encoder_, &pic);
        timings.encodeCall100ns = monotonicNow100ns() - encodeCallStarted100ns;
        if (nvStatus == NV_ENC_ERR_NEED_MORE_INPUT) {
            funcs_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            releaseOutputSlotReservation(*slot);
            ++frameIndex_;
            status = "Direct NVENC accepted input and is waiting for more frames.";
            return true;
        }
        if (nvStatus != NV_ENC_SUCCESS) {
            funcs_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            releaseOutputSlotReservation(*slot);
            status = "NvEncEncodePicture failed: " + statusDetails(nvStatus);
            return false;
        }

        slot->mappedInput = mapped.mappedResource;
        slot->mappedRegisteredResource = prepared.registeredResource;
        // Direct capture inputs retain their pool lease until the output thread
        // has unlocked the bitstream and unmapped this resource.
        if (asyncEnabled_) {
            {
                std::lock_guard lock(outputMutex_);
                inFlightOrder_.push_back(slotIndex(slot));
                const int current = inFlightCount_.fetch_add(1) + 1;
                if (externalInFlightCount_) externalInFlightCount_->store(current);
            }
            outputCv_.notify_one();
            ++frameIndex_;
            status = "Direct NVENC async pipeline is queueing H.264 frames.";
            collectCompletedPackets(packets);
            return true;
        }

        EncodedPacket packet;
        bool produced = false;
        NvencOutputTimings outputTimings;
        const int64_t drainStarted100ns = monotonicNow100ns();
        if (!lockOutputSlot(*slot, packet, status, produced, outputTimings)) {
            return false;
        }
        const int64_t drainLatency100ns = monotonicNow100ns() - drainStarted100ns;
        const int64_t outputTimingRecorded100ns = monotonicNow100ns();
        if (outputLockLatency_) {
            outputLockLatency_->record(outputTimingRecorded100ns, outputTimings.lock100ns);
        }
        if (outputCopyLatency_) {
            outputCopyLatency_->record(outputTimingRecorded100ns, outputTimings.copy100ns);
        }
        if (outputUnmapLatency_) {
            outputUnmapLatency_->record(outputTimingRecorded100ns, outputTimings.unmap100ns);
        }
        outputDrainSamples_.fetch_add(1, std::memory_order_relaxed);
        totalOutputDrainLatency100ns_.fetch_add(drainLatency100ns, std::memory_order_relaxed);
        updateMaximum(maximumOutputDrainLatency100ns_, drainLatency100ns);
        slot->inFlight = false;
        if (produced) {
            packets.push_back(std::move(packet));
        }
        ++frameIndex_;
        status = "Direct NVENC is outputting H.264 packets.";
        return true;
    }

    bool drainPending(std::vector<EncodedPacket>& packets, std::string& status) {
        return drainAll(packets, status);
    }

    int inFlightCount() const {
        return inFlightCount_.load();
    }

    int preparedCount() const {
        return static_cast<int>(preparedSubmissions_.size());
    }

    int64_t averageOutputDrainLatency100ns() const {
        const uint64_t count = outputDrainSamples_.load(std::memory_order_relaxed);
        return count > 0
            ? totalOutputDrainLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
            : 0;
    }

    int64_t maximumOutputDrainLatency100ns() const {
        return maximumOutputDrainLatency100ns_.load(std::memory_order_relaxed);
    }

private:
    struct RegisteredInput {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registeredResource = nullptr;
        NV_ENC_BUFFER_FORMAT bufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
        int width = 0;
        int height = 0;
    };

    struct CachedVideoInputView {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
        uint64_t scalerGeneration = 0;
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
        Microsoft::WRL::ComPtr<ID3D11Texture2D> retainedInputTexture;
        std::shared_ptr<void> retainedInputLease;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> scaledTexture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> scaledOutputView;
        uint64_t scaledViewGeneration = 0;
        uint64_t surfaceCaptureEpoch = 0;
        uint64_t surfaceSourceFrameSequence = 0;
    };

    struct PreparedSubmission {
        OutputSlot* slot = nullptr;
        NV_ENC_REGISTERED_PTR registeredResource = nullptr;
        NV_ENC_BUFFER_FORMAT bufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    };

    void startOutputThread() {
        if (outputThreadRunning_.exchange(true)) return;
        outputFatal_.store(false);
        outputThread_ = std::thread([this] {
            MmcssThreadRegistration mmcss(L"Capture", AVRT_PRIORITY_HIGH);
            while (true) {
                std::size_t index = 0;
                {
                    std::unique_lock lock(outputMutex_);
                    outputCv_.wait(lock, [this] {
                        return !outputThreadRunning_.load() || !inFlightOrder_.empty();
                    });
                    if (inFlightOrder_.empty()) {
                        if (!outputThreadRunning_) break;
                        continue;
                    }
                    index = inFlightOrder_.front();
                }

                if (index >= outputSlots_.size()) {
                    setOutputFailure("NVENC async drain failed: output slot index is invalid.");
                    break;
                }

                auto& slot = outputSlots_[index];
                const int64_t eventWaitStarted100ns = monotonicNow100ns();
                const DWORD waitResult = WaitForSingleObject(slot.completionEvent, asyncDrainWaitTimeoutMs_);
                const int64_t eventWait100ns = monotonicNow100ns() - eventWaitStarted100ns;
                if (outputEventWaitLatency_) {
                    outputEventWaitLatency_->record(monotonicNow100ns(), eventWait100ns);
                }
                if (waitResult == WAIT_TIMEOUT) {
                    setOutputFailure(
                        "NVENC async wait timed out after " +
                        std::to_string(asyncDrainWaitTimeoutMs_) + " ms.");
                    std::cerr << "[perf] nvenc_async_wait_timeout timeoutMs="
                              << asyncDrainWaitTimeoutMs_ << std::endl;
                    break;
                }
                if (waitResult != WAIT_OBJECT_0) {
                    setOutputFailure("NVENC async wait failed.");
                    break;
                }

                EncodedPacket packet;
                std::string outputStatus;
                bool produced = false;
                NvencOutputTimings outputTimings;
                const int64_t drainStarted100ns = monotonicNow100ns();
                if (!lockOutputSlot(slot, packet, outputStatus, produced, outputTimings)) {
                    setOutputFailure(outputStatus);
                    break;
                }
                const int64_t drainLatency100ns = monotonicNow100ns() - drainStarted100ns;
                const int64_t outputTimingRecorded100ns = monotonicNow100ns();
                if (outputLockLatency_) {
                    outputLockLatency_->record(outputTimingRecorded100ns, outputTimings.lock100ns);
                }
                if (outputCopyLatency_) {
                    outputCopyLatency_->record(outputTimingRecorded100ns, outputTimings.copy100ns);
                }
                if (outputUnmapLatency_) {
                    outputUnmapLatency_->record(outputTimingRecorded100ns, outputTimings.unmap100ns);
                }
                outputDrainSamples_.fetch_add(1, std::memory_order_relaxed);
                totalOutputDrainLatency100ns_.fetch_add(drainLatency100ns, std::memory_order_relaxed);
                updateMaximum(maximumOutputDrainLatency100ns_, drainLatency100ns);

                {
                    std::lock_guard lock(outputMutex_);
                    if (!inFlightOrder_.empty() && inFlightOrder_.front() == index) {
                        inFlightOrder_.pop_front();
                    }
                    slot.inFlight = false;
                    const int remaining = inFlightCount_.fetch_sub(1) - 1;
                    if (externalInFlightCount_) externalInFlightCount_->store(std::max(0, remaining));
                    if (produced) completedPackets_.push_back(std::move(packet));
                }
                outputCv_.notify_all();
            }
            outputCv_.notify_all();
        });
    }

    void setOutputFailure(std::string failure) {
        {
            std::lock_guard lock(outputMutex_);
            outputFailure_ = std::move(failure);
            outputFatal_.store(true);
        }
        outputCv_.notify_all();
    }

    void collectCompletedPackets(std::vector<EncodedPacket>& packets) {
        std::lock_guard lock(outputMutex_);
        for (auto& packet : completedPackets_) packets.push_back(std::move(packet));
        completedPackets_.clear();
    }

    void stopOutputThread() {
        if (!outputThreadRunning_.exchange(false)) return;
        outputCv_.notify_all();
        if (outputThread_.joinable()) outputThread_.join();
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> prepareFrameForEncode(
        const CapturedFrame& frame,
        OutputSlot& slot,
        NvencFrameTimings::InputPath& inputPath,
        std::string& status) {
        inputPath = NvencFrameTimings::InputPath::None;
        if (!frame.texture || !device_ || width_ <= 0 || height_ <= 0) {
            status = "NVENC input preparation failed: invalid frame or output size.";
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC frameDesc {};
        frame.texture->GetDesc(&frameDesc);
        const bool canUseNativeBgra =
            supportsArgbInput_ &&
            frame.width == width_ &&
            frame.height == height_ &&
            frameDesc.Width == static_cast<UINT>(width_) &&
            frameDesc.Height == static_cast<UINT>(height_) &&
            frameDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

        // Fresh capture-pool textures can be registered with NVENC directly.
        // Keep their lease until NVENC unmaps the resource so capture cannot
        // overwrite a surface that the hardware encoder is still reading.
        if constexpr (!useDedicatedInputSurfaces_) {
            if (canUseNativeBgra && supportsDirectCaptureInput_ && frame.textureLease &&
                !inputTexturePendingOrInFlight(frame.texture.Get())) {
                inputPath = NvencFrameTimings::InputPath::DirectCaptureBgra;
                return frame.texture;
            }
        }

        if (!context_) device_->GetImmediateContext(&context_);
        if (!context_) {
            status = "NVENC input preparation failed: could not get D3D11 immediate context.";
            return nullptr;
        }

        if constexpr (useDedicatedInputSurfaces_) {
            if (canUseNativeBgra && !preferNv12Input_) {
                if (!ensureBgraCopyOutput(slot, status)) return nullptr;
                const bool surfaceAlreadyContainsFrame =
                    frame.sequence != 0 &&
                    slot.surfaceCaptureEpoch == frame.captureEpoch &&
                    slot.surfaceSourceFrameSequence == frame.sequence;
                if (!surfaceAlreadyContainsFrame) {
                    context_->CopyResource(slot.scaledTexture.Get(), frame.texture.Get());
                    slot.surfaceCaptureEpoch = frame.captureEpoch;
                    slot.surfaceSourceFrameSequence = frame.sequence;
                }
                inputPath = surfaceAlreadyContainsFrame
                    ? NvencFrameTimings::InputPath::CachedSurface
                    : NvencFrameTimings::InputPath::BgraCopyFallback;
                return slot.scaledTexture;
            }
        }

        // Repeated scheduler ticks can overlap while referring to the same
        // capture texture. Give those ticks a private surface so CFR output is
        // preserved without mapping one D3D resource into NVENC twice.
        if constexpr (!useDedicatedInputSurfaces_) {
            if (canUseNativeBgra) {
                if (!ensureBgraCopyOutput(slot, status)) return nullptr;
                context_->CopyResource(slot.scaledTexture.Get(), frame.texture.Get());
                inputPath = NvencFrameTimings::InputPath::BgraCopyFallback;
                return slot.scaledTexture;
            }
        }

        if (!ensureVideoScaler(frame.width, frame.height, status)) {
            return nullptr;
        }
        if (!ensureScaledOutput(slot, status)) {
            return nullptr;
        }

        auto inputView = videoProcessorInputView(frame.texture.Get(), status);
        if (!inputView) return nullptr;

        const bool surfaceAlreadyContainsFrame =
            frame.sequence != 0 &&
            slot.surfaceCaptureEpoch == frame.captureEpoch &&
            slot.surfaceSourceFrameSequence == frame.sequence;
        if (!surfaceAlreadyContainsFrame) {
            D3D11_VIDEO_PROCESSOR_STREAM stream {};
            stream.Enable = TRUE;
            stream.pInputSurface = inputView.Get();
            HRESULT hr = videoContext_->VideoProcessorBlt(
                videoProcessor_.Get(), slot.scaledOutputView.Get(), 0, 1, &stream);
            if (FAILED(hr)) {
                std::ostringstream message;
                message << "NVENC scale failed: VideoProcessorBlt HRESULT 0x" << std::hex << hr;
                status = message.str();
                return nullptr;
            }
            slot.surfaceCaptureEpoch = frame.captureEpoch;
            slot.surfaceSourceFrameSequence = frame.sequence;
        }

        inputPath = surfaceAlreadyContainsFrame
            ? NvencFrameTimings::InputPath::CachedSurface
            : NvencFrameTimings::InputPath::VideoProcessor;
        return slot.scaledTexture;
    }

    bool ensureBgraCopyOutput(OutputSlot& slot, std::string& status) {
        if (slot.scaledTexture) {
            D3D11_TEXTURE2D_DESC existing {};
            slot.scaledTexture->GetDesc(&existing);
            if (existing.Width == static_cast<UINT>(width_) &&
                existing.Height == static_cast<UINT>(height_) &&
                existing.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                return true;
            }
            slot.scaledOutputView.Reset();
            slot.scaledTexture.Reset();
            slot.scaledViewGeneration = 0;
            slot.surfaceCaptureEpoch = 0;
            slot.surfaceSourceFrameSequence = 0;
        }

        D3D11_TEXTURE2D_DESC textureDesc {};
        textureDesc.Width = static_cast<UINT>(std::max(1, width_));
        textureDesc.Height = static_cast<UINT>(std::max(1, height_));
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &slot.scaledTexture);
        if (FAILED(hr) || !slot.scaledTexture) {
            std::ostringstream message;
            message << "NVENC BGRA copy surface creation failed: HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }
        slot.scaledViewGeneration = 0;
        slot.surfaceCaptureEpoch = 0;
        slot.surfaceSourceFrameSequence = 0;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> videoProcessorInputView(
        ID3D11Texture2D* texture,
        std::string& status) {
        for (const auto& cached : inputViews_) {
            if (cached.texture.Get() == texture && cached.scalerGeneration == scalerGeneration_) {
                return cached.view;
            }
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc {};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        const HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
            texture,
            videoProcessorEnumerator_.Get(),
            &inputDesc,
            &inputView);
        if (FAILED(hr) || !inputView) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateVideoProcessorInputView HRESULT 0x" << std::hex << hr;
            status = message.str();
            return nullptr;
        }

        if (inputViews_.size() >= maximumInputViewCacheSize_) inputViews_.erase(inputViews_.begin());
        inputViews_.push_back(CachedVideoInputView { texture, inputView, scalerGeneration_ });
        return inputView;
    }

    bool ensureVideoScaler(int inputWidth, int inputHeight, std::string& status) {
        if (videoProcessor_ &&
            scalerInputWidth_ == inputWidth &&
            scalerInputHeight_ == inputHeight &&
            scalerOutputWidth_ == width_ &&
            scalerOutputHeight_ == height_) {
            return true;
        }

        if (!context_) {
            device_->GetImmediateContext(&context_);
        }
        if (!context_) {
            status = "NVENC scale failed: could not get D3D11 immediate context.";
            return false;
        }
        if (!videoDevice_) {
            HRESULT hr = device_.As(&videoDevice_);
            if (FAILED(hr) || !videoDevice_) {
                status = "NVENC scale failed: D3D11 video device is unavailable.";
                return false;
            }
        }
        if (!videoContext_) {
            HRESULT hr = context_.As(&videoContext_);
            if (FAILED(hr) || !videoContext_) {
                status = "NVENC scale failed: D3D11 video context is unavailable.";
                return false;
            }
        }
        if (!videoContext1_) {
            context_.As(&videoContext1_);
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc {};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = static_cast<UINT>(std::max(1, inputWidth));
        contentDesc.InputHeight = static_cast<UINT>(std::max(1, inputHeight));
        contentDesc.OutputWidth = static_cast<UINT>(std::max(1, width_));
        contentDesc.OutputHeight = static_cast<UINT>(std::max(1, height_));
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&contentDesc, &enumerator);
        if (FAILED(hr) || !enumerator) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateVideoProcessorEnumerator HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
        hr = videoDevice_->CreateVideoProcessor(enumerator.Get(), 0, &processor);
        if (FAILED(hr) || !processor) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateVideoProcessor HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        RECT sourceRect { 0, 0, std::max(1, inputWidth), std::max(1, inputHeight) };
        RECT destRect { 0, 0, std::max(1, width_), std::max(1, height_) };
        videoContext_->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &sourceRect);
        videoContext_->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destRect);
        videoContext_->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &destRect);
        if (videoContext1_) {
            videoContext1_->VideoProcessorSetStreamColorSpace1(
                processor.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }

        videoProcessorEnumerator_ = enumerator;
        videoProcessor_ = processor;
        scalerInputWidth_ = inputWidth;
        scalerInputHeight_ = inputHeight;
        scalerOutputWidth_ = width_;
        scalerOutputHeight_ = height_;
        ++scalerGeneration_;
        if (scalerGeneration_ == 0) ++scalerGeneration_;
        scalerOutputFormat_ = DXGI_FORMAT_UNKNOWN;
        inputViews_.clear();
        return true;
    }

    bool ensureScaledOutput(OutputSlot& slot, std::string& status) {
        const DXGI_FORMAT requestedFormat =
            preferNv12Input_ ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (slot.scaledTexture) {
            D3D11_TEXTURE2D_DESC existing {};
            slot.scaledTexture->GetDesc(&existing);
            if (existing.Width == static_cast<UINT>(width_) &&
                existing.Height == static_cast<UINT>(height_) &&
                existing.Format == requestedFormat &&
                slot.scaledViewGeneration == scalerGeneration_) {
                return true;
            }
            slot.scaledOutputView.Reset();
            slot.scaledTexture.Reset();
            slot.scaledViewGeneration = 0;
            slot.surfaceCaptureEpoch = 0;
            slot.surfaceSourceFrameSequence = 0;
        }

        auto createOutput = [&](DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC textureDesc {};
            textureDesc.Width = static_cast<UINT>(std::max(1, width_));
            textureDesc.Height = static_cast<UINT>(std::max(1, height_));
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = format;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                textureDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            }

            HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &slot.scaledTexture);
            if (FAILED(hr) || !slot.scaledTexture) return hr;

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc {};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;
            hr = videoDevice_->CreateVideoProcessorOutputView(
                slot.scaledTexture.Get(),
                videoProcessorEnumerator_.Get(),
                &outputDesc,
                &slot.scaledOutputView);
            if (FAILED(hr) || !slot.scaledOutputView) {
                slot.scaledOutputView.Reset();
                slot.scaledTexture.Reset();
                return hr;
            }
            slot.scaledViewGeneration = scalerGeneration_;
            slot.surfaceCaptureEpoch = 0;
            slot.surfaceSourceFrameSequence = 0;
            if (scalerOutputFormat_ != format) {
                if (videoContext1_) {
                    videoContext1_->VideoProcessorSetOutputColorSpace1(
                        videoProcessor_.Get(),
                        format == DXGI_FORMAT_NV12
                            ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                }
                scalerOutputFormat_ = format;
            }
            if (!dedicatedInputPathLogged_) {
                std::cerr << "[encoder] Persistent per-slot "
                          << (format == DXGI_FORMAT_NV12 ? "NV12" : "BGRA")
                          << " input surfaces active; capture leases release after GPU conversion."
                          << std::endl;
                dedicatedInputPathLogged_ = true;
            }
            return S_OK;
        };

        HRESULT hr = createOutput(requestedFormat);
        if (FAILED(hr) && preferNv12Input_) {
            preferNv12Input_ = false;
            hr = createOutput(DXGI_FORMAT_B8G8R8A8_UNORM);
            if (SUCCEEDED(hr) && !bgraFallbackLogged_) {
                std::cerr << "[encoder] NV12 video-processor output is unavailable; using BGRA NVENC input fallback."
                          << std::endl;
                bgraFallbackLogged_ = true;
            }
        }
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "NVENC scale failed: no compatible output surface, HRESULT 0x" << std::hex << hr;
            status = message.str();
            slot.scaledOutputView.Reset();
            slot.scaledTexture.Reset();
            slot.scaledViewGeneration = 0;
            return false;
        }
        return true;
    }

    bool createOutputSlots(std::string& status) {
        outputSlots_.resize(outputSlotCount_);
        for (auto& slot : outputSlots_) {
            NV_ENC_CREATE_BITSTREAM_BUFFER bitstream {};
            bitstream.version = nvencStructVersionForApi(1, apiVersion_);
            NVENCSTATUS nvStatus = funcs_.nvEncCreateBitstreamBuffer(encoder_, &bitstream);
            if (nvStatus != NV_ENC_SUCCESS) {
                status = "NvEncCreateBitstreamBuffer failed: " + statusDetails(nvStatus);
                return false;
            }
            slot.bitstreamBuffer = bitstream.bitstreamBuffer;

            if (asyncEnabled_) {
                slot.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!slot.completionEvent) {
                    status = "NVENC async event creation failed.";
                    return false;
                }

                if (!funcs_.nvEncRegisterAsyncEvent) {
                    status = "NVENC async event registration is unavailable in this driver API.";
                    return false;
                }

                NV_ENC_EVENT_PARAMS eventParams {};
                eventParams.version = nvencStructVersionForApi(2, apiVersion_);
                eventParams.completionEvent = slot.completionEvent;
                nvStatus = funcs_.nvEncRegisterAsyncEvent(encoder_, &eventParams);
                if (nvStatus != NV_ENC_SUCCESS) {
                    status = "NvEncRegisterAsyncEvent failed: " + statusDetails(nvStatus);
                    return false;
                }
                slot.eventRegistered = true;
            }
        }
        return true;
    }

    int queryEncodeCap(NV_ENC_CAPS cap) {
        if (!encoder_) return -1;
        NV_ENC_CAPS_PARAM params {};
        params.version = nvencStructVersionForApi(1, apiVersion_);
        params.capsToQuery = cap;
        int value = 0;
        const NVENCSTATUS nvStatus = funcs_.nvEncGetEncodeCaps(encoder_, NV_ENC_CODEC_H264_GUID, &params, &value);
        return nvStatus == NV_ENC_SUCCESS ? value : -1;
    }

    bool supportsInputFormat(NV_ENC_BUFFER_FORMAT format) {
        if (!encoder_ || !funcs_.nvEncGetInputFormatCount || !funcs_.nvEncGetInputFormats) return false;
        uint32_t count = 0;
        NVENCSTATUS nvStatus = funcs_.nvEncGetInputFormatCount(
            encoder_, NV_ENC_CODEC_H264_GUID, &count);
        if (nvStatus != NV_ENC_SUCCESS || count == 0) return false;
        std::vector<NV_ENC_BUFFER_FORMAT> formats(count, NV_ENC_BUFFER_FORMAT_UNDEFINED);
        uint32_t returnedCount = 0;
        nvStatus = funcs_.nvEncGetInputFormats(
            encoder_, NV_ENC_CODEC_H264_GUID, formats.data(), count, &returnedCount);
        formats.resize(std::min<std::size_t>(formats.size(), returnedCount));
        return nvStatus == NV_ENC_SUCCESS &&
            std::find(formats.begin(), formats.end(), format) != formats.end();
    }

    bool inputResourceInFlight(NV_ENC_REGISTERED_PTR registeredResource) const {
        std::lock_guard lock(outputMutex_);
        for (const auto& slot : outputSlots_) {
            if (slot.inFlight && slot.mappedRegisteredResource == registeredResource && registeredResource) {
                return true;
            }
        }
        return false;
    }

    bool inputResourcePendingOrInFlight(NV_ENC_REGISTERED_PTR registeredResource) const {
        if (!registeredResource) return false;
        if (std::any_of(
                preparedSubmissions_.begin(),
                preparedSubmissions_.end(),
                [registeredResource](const PreparedSubmission& prepared) {
                    return prepared.registeredResource == registeredResource;
                })) {
            return true;
        }
        return inputResourceInFlight(registeredResource);
    }

    bool inputTexturePendingOrInFlight(ID3D11Texture2D* texture) const {
        if (!texture) return false;
        std::lock_guard lock(outputMutex_);
        return std::any_of(outputSlots_.begin(), outputSlots_.end(), [texture](const OutputSlot& slot) {
            return slot.inFlight && slot.retainedInputTexture.Get() == texture;
        });
    }

    bool drainReady(std::vector<EncodedPacket>& packets, std::string& status) {
        if (!asyncEnabled_) return true;
        collectCompletedPackets(packets);
        if (outputFatal_.load()) {
            std::lock_guard lock(outputMutex_);
            status = outputFailure_.empty() ? "NVENC async output failed." : outputFailure_;
            return false;
        }
        return true;
    }

    bool drainAll(std::vector<EncodedPacket>& packets, std::string& status) {
        NvencFrameTimings ignoredTimings;
        while (!preparedSubmissions_.empty()) {
            ignoredTimings = {};
            if (!submitPrepared(packets, status, ignoredTimings)) return false;
        }
        if (!asyncEnabled_) return true;
        std::unique_lock lock(outputMutex_);
        constexpr auto maximumDrainWait = std::chrono::milliseconds(
            outputSlotCount_ * asyncDrainWaitTimeoutMs_ + 500);
        const bool finished = outputCv_.wait_for(lock, maximumDrainWait, [this] {
            return inFlightOrder_.empty() || outputFatal_.load();
        });
        for (auto& packet : completedPackets_) packets.push_back(std::move(packet));
        completedPackets_.clear();
        if (outputFatal_.load()) {
            status = outputFailure_.empty() ? "NVENC async output failed." : outputFailure_;
            return false;
        }
        if (!finished) {
            status = "NVENC async drain did not finish within its bounded wait.";
            return false;
        }
        return true;
    }

    bool lockOutputSlot(
        OutputSlot& slot,
        EncodedPacket& packet,
        std::string& status,
        bool& produced,
        NvencOutputTimings& timings) {
        produced = false;
        timings = {};
        const int64_t lockStarted100ns = monotonicNow100ns();
        NV_ENC_LOCK_BITSTREAM lock {};
        NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
        do {
            lock = {};
            lock.version = nvencStructVersionForApi(2, apiVersion_, true);
            lock.outputBitstream = slot.bitstreamBuffer;
            lock.doNotWait = asyncEnabled_ ? 1 : 0;
            nvStatus = funcs_.nvEncLockBitstream(encoder_, &lock);
            if (nvStatus != NV_ENC_ERR_LOCK_BUSY || !asyncEnabled_) break;

            ++timings.lockBusyRetries;
            if (monotonicNow100ns() - lockStarted100ns >= asyncDrainWaitTimeout100ns_) break;
            SwitchToThread();
        } while (true);
        timings.lock100ns = monotonicNow100ns() - lockStarted100ns;
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncLockBitstream failed: " + statusDetails(nvStatus);
            return false;
        }

        packet.kind = PacketKind::Video;
        packet.codec = PacketCodec::H264AnnexB;
        packet.pts100ns = static_cast<int64_t>(lock.outputTimeStamp ? lock.outputTimeStamp : slot.framePts100ns);
        packet.dts100ns = packet.pts100ns;
        packet.duration100ns = static_cast<int64_t>(lock.outputDuration ? lock.outputDuration : slot.frameDuration100ns);
        packet.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
        packet.sourceId = "primary-monitor";
        packet.encoderId = asyncEnabled_ ? "NVENC_ASYNC" : "NVENC";
        packet.encodedWidth = slot.frameEncodedWidth;
        packet.encodedHeight = slot.frameEncodedHeight;
        packet.sourceWidth = slot.frameSourceWidth;
        packet.sourceHeight = slot.frameSourceHeight;
        packet.encoderEpoch = static_cast<uint32_t>(slot.captureEpoch);
        packet.sourceFrameSequence = slot.sourceFrameSequence;
        const int64_t copyStarted100ns = monotonicNow100ns();
        packet.payload = packetPool_.acquirePayload(lock.bitstreamSizeInBytes);
        if (lock.bitstreamSizeInBytes > 0 && lock.bitstreamBufferPtr) {
            std::memcpy(mutablePayload(packet).data(), lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        }
        analyzeH264Packet(packet);
        timings.copy100ns = monotonicNow100ns() - copyStarted100ns;

        funcs_.nvEncUnlockBitstream(encoder_, slot.bitstreamBuffer);
        const int64_t unmapStarted100ns = monotonicNow100ns();
        if (slot.mappedInput) {
            funcs_.nvEncUnmapInputResource(encoder_, slot.mappedInput);
        }
        timings.unmap100ns = monotonicNow100ns() - unmapStarted100ns;
        {
            std::lock_guard outputLock(outputMutex_);
            slot.mappedInput = nullptr;
            resetOutputSlotFrameState(slot);
        }
        produced = !payloadEmpty(packet);
        status = asyncEnabled_
            ? "Direct NVENC async pipeline is outputting H.264 packets."
            : "Direct NVENC is outputting H.264 packets.";
        return true;
    }

    OutputSlot* freeOutputSlot(
        const CapturedFrame& frame,
        std::vector<EncodedPacket>& packets,
        std::string& status) {
        (void)packets;
        if (outputSlots_.empty()) {
            status = "NVENC output slot pool is empty.";
            return nullptr;
        }

        std::unique_lock lock(outputMutex_);
        if (outputFatal_.load()) {
            status = outputFailure_.empty() ? "NVENC async output failed." : outputFailure_;
            return nullptr;
        }
        auto reserveFreeSlot = [this, &frame]() -> OutputSlot* {
            if (frame.sequence != 0) {
                for (std::size_t attempt = 0; attempt < outputSlots_.size(); ++attempt) {
                    const std::size_t index = (nextOutputSlot_ + attempt) % outputSlots_.size();
                    auto& slot = outputSlots_[index];
                    if (!slot.inFlight &&
                        slot.surfaceCaptureEpoch == frame.captureEpoch &&
                        slot.surfaceSourceFrameSequence == frame.sequence) {
                        slot.inFlight = true;
                        nextOutputSlot_ = (index + 1) % outputSlots_.size();
                        return &slot;
                    }
                }
            }
            for (std::size_t attempt = 0; attempt < outputSlots_.size(); ++attempt) {
                const std::size_t index = (nextOutputSlot_ + attempt) % outputSlots_.size();
                if (!outputSlots_[index].inFlight) {
                    outputSlots_[index].inFlight = true;
                    nextOutputSlot_ = (index + 1) % outputSlots_.size();
                    return &outputSlots_[index];
                }
            }
            return nullptr;
        };
        if (auto* slot = reserveFreeSlot()) return slot;
        if (asyncEnabled_) {
            outputCv_.wait_for(lock, outputSlotAvailabilityWait_, [this] {
                return outputFatal_.load() || std::any_of(
                    outputSlots_.begin(), outputSlots_.end(), [](const OutputSlot& slot) { return !slot.inFlight; });
            });
            if (outputFatal_.load()) {
                status = outputFailure_.empty() ? "NVENC async output failed." : outputFailure_;
                return nullptr;
            }
            if (auto* slot = reserveFreeSlot()) return slot;
        }
        status = "NVENC output slot pool is full.";
        return nullptr;
    }

    static void clearOutputSlotInputState(OutputSlot& slot) {
        slot.mappedRegisteredResource = nullptr;
        slot.retainedInputLease.reset();
        slot.retainedInputTexture.Reset();
    }

    static void resetOutputSlotFrameState(OutputSlot& slot) {
        clearOutputSlotInputState(slot);
        slot.framePts100ns = 0;
        slot.frameDuration100ns = 0;
        slot.frameEncodedWidth = 0;
        slot.frameEncodedHeight = 0;
        slot.frameSourceWidth = 0;
        slot.frameSourceHeight = 0;
        slot.captureEpoch = 0;
        slot.sourceFrameSequence = 0;
    }

    void releaseOutputSlotReservation(OutputSlot& slot) {
        std::lock_guard lock(outputMutex_);
        resetOutputSlotFrameState(slot);
        slot.inFlight = false;
        outputCv_.notify_all();
    }

    std::size_t slotIndex(const OutputSlot* slot) const {
        return static_cast<std::size_t>(slot - outputSlots_.data());
    }

    RegisteredInput* registeredInputFor(ID3D11Texture2D* texture, int width, int height, std::string& status) {
        if (!texture) {
            status = "NVENC encode failed: frame texture is null.";
            return nullptr;
        }

        for (auto& input : registeredInputs_) {
            if (input.texture.Get() == texture && input.width == width && input.height == height) {
                return &input;
            }
        }

        if (registeredInputs_.size() >= maxRegisteredInputs_) {
            auto victim = std::find_if(registeredInputs_.begin(), registeredInputs_.end(), [this](const RegisteredInput& input) {
                return !inputResourcePendingOrInFlight(input.registeredResource);
            });
            if (victim == registeredInputs_.end()) {
                status = "NVENC input resource cache is full.";
                return nullptr;
            }
            if (victim->registeredResource) {
                funcs_.nvEncUnregisterResource(encoder_, victim->registeredResource);
            }
            registeredInputs_.erase(victim);
        }

        NV_ENC_REGISTER_RESOURCE registered {};
        registered.version = nvencStructVersionForApi(5, apiVersion_);
        registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        registered.width = static_cast<uint32_t>(width);
        registered.height = static_cast<uint32_t>(height);
        registered.pitch = 0;
        registered.subResourceIndex = 0;
        registered.resourceToRegister = texture;
        D3D11_TEXTURE2D_DESC textureDesc {};
        texture->GetDesc(&textureDesc);
        registered.bufferFormat = textureDesc.Format == DXGI_FORMAT_NV12
            ? NV_ENC_BUFFER_FORMAT_NV12
            : NV_ENC_BUFFER_FORMAT_ARGB;
        registered.bufferUsage = NV_ENC_INPUT_IMAGE;

        const NVENCSTATUS nvStatus = funcs_.nvEncRegisterResource(encoder_, &registered);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncRegisterResource failed: " + statusDetails(nvStatus);
            return nullptr;
        }

        RegisteredInput input {};
        input.texture = texture;
        input.registeredResource = registered.registeredResource;
        input.bufferFormat = registered.bufferFormat;
        input.width = width;
        input.height = height;
        registeredInputs_.push_back(std::move(input));
        return &registeredInputs_.back();
    }

    void destroyEncoderResources() {
        initialized_ = false;
        if (encoder_) {
            std::vector<EncodedPacket> ignoredPackets;
            std::string ignoredStatus;
            drainAll(ignoredPackets, ignoredStatus);
        }
        stopOutputThread();
        if (encoder_) {
            for (auto& slot : outputSlots_) {
                if (slot.mappedInput) {
                    funcs_.nvEncUnmapInputResource(encoder_, slot.mappedInput);
                    slot.mappedInput = nullptr;
                }
                resetOutputSlotFrameState(slot);
                slot.scaledViewGeneration = 0;
                if (slot.eventRegistered) {
                    NV_ENC_EVENT_PARAMS eventParams {};
                    eventParams.version = nvencStructVersionForApi(2, apiVersion_);
                    eventParams.completionEvent = slot.completionEvent;
                    funcs_.nvEncUnregisterAsyncEvent(encoder_, &eventParams);
                    slot.eventRegistered = false;
                }
                if (slot.bitstreamBuffer) {
                    funcs_.nvEncDestroyBitstreamBuffer(encoder_, slot.bitstreamBuffer);
                    slot.bitstreamBuffer = nullptr;
                }
                if (slot.completionEvent) {
                    CloseHandle(slot.completionEvent);
                    slot.completionEvent = nullptr;
                }
            }
            outputSlots_.clear();
            preparedSubmissions_.clear();
            inFlightOrder_.clear();
            completedPackets_.clear();
            outputFailure_.clear();
            outputFatal_.store(false);
            inFlightCount_.store(0);
            if (externalInFlightCount_) externalInFlightCount_->store(0);
        }
        if (encoder_) {
            for (auto& input : registeredInputs_) {
                if (input.registeredResource) {
                    funcs_.nvEncUnregisterResource(encoder_, input.registeredResource);
                    input.registeredResource = nullptr;
                }
            }
            registeredInputs_.clear();
        }
        if (encoder_) {
            funcs_.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
        }
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        videoContext1_.Reset();
        videoContext_.Reset();
        videoDevice_.Reset();
        context_.Reset();
        inputViews_.clear();
        presetGuid_ = {};
        encodeConfig_ = {};
        initParams_ = {};
        asyncEnabled_ = false;
        requestedWidth_ = 0;
        requestedHeight_ = 0;
        width_ = 0;
        height_ = 0;
        fps_ = 30;
        bitrateMbps_ = 40;
        maxEncodeWidth_ = 0;
        maxEncodeHeight_ = 0;
        h264CapWidth_ = 0;
        h264CapHeight_ = 0;
        scalerInputWidth_ = 0;
        scalerInputHeight_ = 0;
        scalerOutputWidth_ = 0;
        scalerOutputHeight_ = 0;
        scalerGeneration_ = 0;
        scalerOutputFormat_ = DXGI_FORMAT_UNKNOWN;
        preferNv12Input_ = true;
        bgraFallbackLogged_ = false;
        supportsNv12Input_ = false;
        dedicatedInputPathLogged_ = false;
        nv12RegistrationFallbackLogged_ = false;
        supportsArgbInput_ = false;
        supportsDirectCaptureInput_ = false;
        directCaptureFallbackLogged_ = false;
        directBgraFallbackLogged_ = false;
        frameIndex_ = 0;
        nextKeyframePts100ns_ = 0;
        nextOutputSlot_ = 0;
        preparedSubmissionDepth_ = minimumPreparedSubmissionDepth_;
    }

    void destroy() {
        destroyEncoderResources();
        device_.Reset();
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        funcs_ = {};
    }

    HMODULE module_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST funcs_ {};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    PacketRingBuffer& packetPool_;
    std::atomic<int>* externalInFlightCount_ = nullptr;
    std::atomic<int>* externalDropCount_ = nullptr;
    std::atomic<int>* externalSurfaceDropCount_ = nullptr;
    std::atomic<int>* externalInputDropCount_ = nullptr;
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
    int maxEncodeHeight_ = 0;
    int h264CapWidth_ = 0;
    int h264CapHeight_ = 0;
    int scalerInputWidth_ = 0;
    int scalerInputHeight_ = 0;
    int scalerOutputWidth_ = 0;
    int scalerOutputHeight_ = 0;
    uint64_t scalerGeneration_ = 0;
    DXGI_FORMAT scalerOutputFormat_ = DXGI_FORMAT_UNKNOWN;
    bool preferNv12Input_ = true;
    bool bgraFallbackLogged_ = false;
    bool supportsNv12Input_ = false;
    bool dedicatedInputPathLogged_ = false;
    bool nv12RegistrationFallbackLogged_ = false;
    bool supportsArgbInput_ = false;
    bool supportsDirectCaptureInput_ = false;
    bool directCaptureFallbackLogged_ = false;
    bool directBgraFallbackLogged_ = false;
    uint32_t frameIndex_ = 0;
    int64_t nextKeyframePts100ns_ = 0;
    std::size_t nextOutputSlot_ = 0;
    static constexpr std::size_t maxRegisteredInputs_ = 32;
    static constexpr std::size_t maximumInputViewCacheSize_ = 16;
    static constexpr std::size_t outputSlotCount_ = 12;
    static constexpr std::size_t minimumPreparedSubmissionDepth_ = 2;
    static constexpr std::size_t maximumPreparedSubmissionDepth_ = outputSlotCount_ / 2;
    std::size_t preparedSubmissionDepth_ = minimumPreparedSubmissionDepth_;
    static constexpr bool useDedicatedInputSurfaces_ = true;
    static constexpr DWORD asyncDrainWaitTimeoutMs_ = 1000;
    static constexpr int64_t asyncDrainWaitTimeout100ns_ =
        static_cast<int64_t>(asyncDrainWaitTimeoutMs_) * 10'000LL;
    static constexpr auto outputSlotAvailabilityWait_ = std::chrono::milliseconds(12);
};

}  // namespace

EncoderWorker::EncoderWorker(
    FrameQueue& frames,
    PacketRingBuffer& packets,
    ReplaySegmentStore* replayStore)
    : frames_(frames), packets_(packets), replayStore_(replayStore) {}

EncoderWorker::~EncoderWorker() {
    stop();
}

void EncoderWorker::start() {
    if (running_.exchange(true)) return;

    recentInputPreparationLatency_.clear();
    recentInputMapLatency_.clear();
    recentNvencCallLatency_.clear();
    recentOutputEventWaitLatency_.clear();
    recentOutputLockLatency_.clear();
    recentOutputCopyLatency_.clear();
    recentOutputUnmapLatency_.clear();
    performanceStarted100ns_.store(monotonicNow100ns(), std::memory_order_relaxed);

    HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!module) module = LoadLibraryW(L"nvEncodeAPI.dll");
    nvencRuntimeLoaded_ = module && GetProcAddress(module, "NvEncodeAPICreateInstance");
    if (module) FreeLibrary(module);

    setStatus(nvencRuntimeLoaded_
        ? "NVENC runtime loaded; capture frames are being handed to the encoder worker."
        : "NVENC runtime was not found; encoder worker cannot produce H.264 packets.");
    encodeThread_ = std::thread(&EncoderWorker::encodeLoop, this);
    thread_ = std::thread(&EncoderWorker::run, this);
}

void EncoderWorker::stop() {
    if (!running_.exchange(false)) return;
    frames_.stop();
    submitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    submitCv_.notify_all();
    if (encodeThread_.joinable()) encodeThread_.join();
}

bool EncoderWorker::running() const {
    return running_;
}

void EncoderWorker::configure(
    int fps,
    int bitrateMbps,
    int targetWidth,
    int targetHeight,
    int maxEncodeWidth,
    int maxEncodeHeight,
    int nvencPreset,
    bool discardBufferedPackets) {
    const int nextFps = std::clamp(fps, 24, 60);
    const int nextBitrateMbps = std::clamp(bitrateMbps, 4, 120);
    const int nextTargetWidth = std::max(0, targetWidth);
    const int nextTargetHeight = std::max(0, targetHeight);
    const int nextMaxEncodeWidth = std::max(0, maxEncodeWidth);
    const int nextMaxEncodeHeight = std::max(0, maxEncodeHeight);
    const int nextPreset = std::clamp(nvencPreset, 1, 5);

    const bool changed =
        targetFps_.load() != nextFps ||
        targetBitrateMbps_.load() != nextBitrateMbps ||
        targetWidth_.load() != nextTargetWidth ||
        targetHeight_.load() != nextTargetHeight ||
        maxEncodeWidth_.load() != nextMaxEncodeWidth ||
        maxEncodeHeight_.load() != nextMaxEncodeHeight ||
        nvencPreset_.load() != nextPreset;

    targetFps_ = nextFps;
    targetBitrateMbps_ = nextBitrateMbps;
    targetWidth_ = nextTargetWidth;
    targetHeight_ = nextTargetHeight;
    maxEncodeWidth_ = nextMaxEncodeWidth;
    maxEncodeHeight_ = nextMaxEncodeHeight;
    nvencPreset_ = nextPreset;
    if (changed) {
        const int nextVersion = configVersion_.fetch_add(1) + 1;
        if (discardBufferedPackets) discardPacketsAtConfigVersion_.store(nextVersion);
    }
}

void EncoderWorker::requireFreshFrame(bool discardBufferedPackets) {
    frames_.clear();
    const int nextVersion = freshFrameVersion_.fetch_add(1) + 1;
    if (discardBufferedPackets) discardPacketsAtFreshFrameVersion_.store(nextVersion);
}

void EncoderWorker::resetAutoOutputResolution() {
    autoOutputWidth_ = 0;
    autoOutputHeight_ = 0;
    requireFreshFrame(true);
}

bool EncoderWorker::nvencRuntimeLoaded() const {
    return nvencRuntimeLoaded_;
}

int EncoderWorker::framesAccepted() const {
    return framesAccepted_;
}

int EncoderWorker::framesEncoded() const {
    return framesEncoded_;
}

int EncoderWorker::queuedEncodeFrames() const {
    std::lock_guard lock(submitMutex_);
    return static_cast<int>(pendingJobs_.size());
}

int EncoderWorker::schedulerDroppedFrames() const {
    return schedulerDroppedFrames_.load();
}

int EncoderWorker::schedulerRepeatedFrames() const {
    return schedulerRepeatedFrames_.load();
}

int EncoderWorker::encoderQueueDrops() const {
    return encoderQueueDrops_.load();
}

int EncoderWorker::encoderRepeatCoalesced() const {
    return encoderRepeatCoalesced_.load();
}

int EncoderWorker::nvencSurfaceDrops() const {
    return nvencSurfaceDrops_.load();
}

int EncoderWorker::nvencInputDrops() const {
    return nvencInputDrops_.load();
}

int EncoderWorker::encoderBackpressureDrops() const {
    return encoderBackpressureDrops_.load();
}

int EncoderWorker::nvencInFlightFrames() const {
    return nvencInFlightFrames_.load();
}

uint64_t EncoderWorker::nvencZeroCopyFrames() const {
    return nvencZeroCopyFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::nvencCopyFallbackFrames() const {
    return nvencCopyFallbackFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::nvencConvertedFrames() const {
    return nvencConvertedFrames_.load(std::memory_order_relaxed);
}

int EncoderWorker::effectiveNvencPreset() const {
    return nvencPreset_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::maximumSubmitLatency100ns() const {
    return maximumSubmitLatency100ns_.load();
}

int64_t EncoderWorker::averageScaleLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalScaleLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumScaleLatency100ns() const {
    return maximumScaleLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageInputMapLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalInputMapLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumInputMapLatency100ns() const {
    return maximumInputMapLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageNvencCallLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalNvencCallLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumNvencCallLatency100ns() const {
    return maximumNvencCallLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageOutputDrainLatency100ns() const {
    return totalOutputDrainLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::maximumOutputDrainLatency100ns() const {
    return maximumOutputDrainLatency100ns_.load(std::memory_order_relaxed);
}

EncoderRecentPerformance EncoderWorker::recentPerformance() const {
    const int64_t now100ns = monotonicNow100ns();
    EncoderRecentPerformance result;
    result.inputPreparation = recentInputPreparationLatency_.snapshot(now100ns);
    result.inputMap = recentInputMapLatency_.snapshot(now100ns);
    result.encodeCall = recentNvencCallLatency_.snapshot(now100ns);
    result.outputEventWait = recentOutputEventWaitLatency_.snapshot(now100ns);
    result.outputLock = recentOutputLockLatency_.snapshot(now100ns);
    result.outputCopy = recentOutputCopyLatency_.snapshot(now100ns);
    result.outputUnmap = recentOutputUnmapLatency_.snapshot(now100ns);
    const int64_t started100ns = performanceStarted100ns_.load(std::memory_order_relaxed);
    const int64_t observationWindow100ns = started100ns > 0
        ? std::min<int64_t>(now100ns - started100ns, 50'000'000LL)
        : 0;
    if (observationWindow100ns > 0) {
        result.inputFps = static_cast<double>(result.inputPreparation.samples) * 10'000'000.0 /
            static_cast<double>(observationWindow100ns);
        result.outputFps = static_cast<double>(result.outputLock.samples) * 10'000'000.0 /
            static_cast<double>(observationWindow100ns);
    }
    return result;
}

int EncoderWorker::sourceWidth() const {
    return sourceWidth_;
}

int EncoderWorker::sourceHeight() const {
    return sourceHeight_;
}

int EncoderWorker::outputWidth() const {
    return outputWidth_;
}

int EncoderWorker::outputHeight() const {
    return outputHeight_;
}

bool EncoderWorker::scalingActive() const {
    return scalingActive_;
}

std::string EncoderWorker::status() const {
    std::lock_guard lock(statusMutex_);
    return status_;
}

void EncoderWorker::setStatus(std::string status) {
    std::lock_guard lock(statusMutex_);
    status_ = std::move(status);
}

bool EncoderWorker::queueJob(EncodeJob job) {
    std::unique_lock lock(submitMutex_);
    const uint64_t sourceSequence = job.frame.sequence;
    const bool repeatsObservedSource = sourceSequence != 0 && sourceSequence == lastObservedSourceSequence_;
    if (sourceSequence != 0) lastObservedSourceSequence_ = sourceSequence;
    const bool matchingSourceAlreadyQueued = repeatsObservedSource &&
        std::any_of(pendingJobs_.begin(), pendingJobs_.end(), [sourceSequence](const EncodeJob& pending) {
            return pending.frame.sequence == sourceSequence;
        });

    const auto admission = encoderQueueAdmission(
        pendingJobs_.size(),
        maximumPendingJobs_,
        repeatsObservedSource,
        matchingSourceAlreadyQueued);
    if (admission == EncoderQueueAdmission::CoalesceRepeat) {
        ++encoderRepeatCoalesced_;
        return false;
    }
    if (admission == EncoderQueueAdmission::WaitForRoom) {
        submitCv_.wait_for(lock, encoderQueueWaitBudget(), [this] {
            return !running_.load() || pendingJobs_.size() < maximumPendingJobs_;
        });
    }
    if (!running_.load()) return false;
    if (pendingJobs_.size() >= maximumPendingJobs_) {
        if (repeatsObservedSource) {
            ++encoderRepeatCoalesced_;
            return false;
        }
        uint64_t previousSequence = lastDequeuedSourceSequence_;
        auto repeated = pendingJobs_.end();
        for (auto it = pendingJobs_.begin(); it != pendingJobs_.end(); ++it) {
            const uint64_t queuedSequence = it->frame.sequence;
            if (queuedSequence != 0 && queuedSequence == previousSequence) {
                repeated = it;
                break;
            }
            if (queuedSequence != 0) previousSequence = queuedSequence;
        }
        if (repeated != pendingJobs_.end()) {
            pendingJobs_.erase(repeated);
            ++encoderRepeatCoalesced_;
        } else {
            pendingJobs_.pop_front();
            ++encoderQueueDrops_;
            ++encoderBackpressureDrops_;
        }
    }
    pendingJobs_.push_back(std::move(job));
    lock.unlock();
    ++framesAccepted_;
    submitCv_.notify_one();
    return true;
}

void EncoderWorker::run() {
    std::optional<CapturedFrame> currentFrame;
    while (running_ && !currentFrame) {
        currentFrame = frames_.waitPop();
        if (auto latest = frames_.consumeAllAndGetLatest()) currentFrame = std::move(latest);
    }

    if (!running_) return;

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (avrtHandle) AvSetMmThreadPriority(avrtHandle, AVRT_PRIORITY_HIGH);

    int activeFps = std::clamp(targetFps_.load(), 24, 60);
    int activeFreshFrameVersion = freshFrameVersion_.load();
    int64_t frameSpacing100ns = 10'000'000LL / activeFps;
    VideoTimeline timeline(currentFrame->pts100ns, activeFps);
    auto interval = std::chrono::nanoseconds(frameSpacing100ns * 100);
    // Run one output interval behind capture. This gives asynchronous capture
    // delivery time to publish the frame for a tick without changing its PTS.
    constexpr int schedulerSourceArrivalCushionTicks = 1;
    auto nextWake = std::chrono::steady_clock::now() +
        interval * (1 + schedulerSourceArrivalCushionTicks);
    uint64_t lastSubmittedSourceSequence = 0;

    while (running_) {
        const int fps = std::clamp(targetFps_.load(), 24, 60);
        const int currentFreshFrameVersion = freshFrameVersion_.load();
        if (fps != activeFps || currentFreshFrameVersion != activeFreshFrameVersion) {
            {
                std::lock_guard lock(submitMutex_);
                pendingJobs_.clear();
                lastObservedSourceSequence_ = 0;
                lastDequeuedSourceSequence_ = 0;
            }
            submitCv_.notify_all();
            activeFps = fps;
            activeFreshFrameVersion = currentFreshFrameVersion;
            frameSpacing100ns = 10'000'000LL / activeFps;
            interval = std::chrono::nanoseconds(frameSpacing100ns * 100);
            currentFrame.reset();
            while (running_ && !currentFrame) {
                currentFrame = frames_.waitPop();
                if (auto latest = frames_.consumeAllAndGetLatest()) currentFrame = std::move(latest);
            }
            if (!running_ || !currentFrame) break;
            timeline.reset(currentFrame->pts100ns, activeFps);
            nextWake = std::chrono::steady_clock::now() +
                interval * (1 + schedulerSourceArrivalCushionTicks);
            lastSubmittedSourceSequence = 0;
            continue;
        }

        std::this_thread::sleep_until(nextWake);
        if (!running_) break;

        const auto now = std::chrono::steady_clock::now();
        const int64_t lateness100ns = now > nextWake
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(now - nextWake).count() / 100
            : 0;
        constexpr int64_t maximumCatchUpTicks = 8;
        const auto timelineStep = timeline.advance(lateness100ns, maximumCatchUpTicks);
        const int64_t skippedTicks = timelineStep.skippedTicks;
        if (skippedTicks > 0) {
            schedulerDroppedFrames_.fetch_add(static_cast<int>(std::min<int64_t>(skippedTicks, INT_MAX)));
        }

        for (int64_t tick = 0; tick < timelineStep.dueTicks && running_; ++tick) {
            const int64_t tickPts100ns = timelineStep.pts100ns + tick * frameSpacing100ns;
            if (auto newest = frames_.consumeLatestAtOrBefore(tickPts100ns + frameSpacing100ns / 2)) {
                currentFrame = std::move(newest);
            }
            if (currentFrame) {
                CapturedFrame frameToEncode = *currentFrame;
                frameToEncode.pts100ns = tickPts100ns;

                int outputWidth = targetWidth_.load();
                int outputHeight = targetHeight_.load();
                if (outputWidth <= 0 || outputHeight <= 0) {
                    int lockedWidth = autoOutputWidth_.load();
                    int lockedHeight = autoOutputHeight_.load();
                    if (lockedWidth <= 0 || lockedHeight <= 0) {
                        lockedWidth = frameToEncode.width;
                        lockedHeight = frameToEncode.height;
                        autoOutputWidth_ = lockedWidth;
                        autoOutputHeight_ = lockedHeight;
                    }
                    outputWidth = lockedWidth;
                    outputHeight = lockedHeight;
                }

                sourceWidth_ = frameToEncode.width;
                sourceHeight_ = frameToEncode.height;
                outputWidth_ = outputWidth;
                outputHeight_ = outputHeight;
                scalingActive_ = frameToEncode.width != outputWidth || frameToEncode.height != outputHeight;

                const uint64_t sourceSequence = frameToEncode.sequence;
                if (queueJob(EncodeJob {
                        std::move(frameToEncode),
                        fps,
                        std::clamp(targetBitrateMbps_.load(), 4, 120),
                        outputWidth,
                        outputHeight,
                        std::max(maxEncodeWidth_.load(), outputWidth),
                        std::max(maxEncodeHeight_.load(), outputHeight),
                        std::clamp(nvencPreset_.load(std::memory_order_relaxed), 1, 5),
                        configVersion_.load(),
                        currentFreshFrameVersion,
                    })) {
                    if (lastSubmittedSourceSequence != 0 && sourceSequence == lastSubmittedSourceSequence) {
                        ++schedulerRepeatedFrames_;
                    }
                    lastSubmittedSourceSequence = sourceSequence;
                }
            }
        }
        nextWake += interval * (timelineStep.skippedTicks + timelineStep.dueTicks);
    }

    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
}

void EncoderWorker::encodeLoop() {
    MmcssThreadRegistration mmcss(L"Capture", AVRT_PRIORITY_HIGH);
    auto session = std::make_unique<NvencSession>(
        packets_,
        &nvencInFlightFrames_,
        &encoderBackpressureDrops_,
        &nvencSurfaceDrops_,
        &nvencInputDrops_,
        &recentOutputEventWaitLatency_,
        &recentOutputLockLatency_,
        &recentOutputCopyLatency_,
        &recentOutputUnmapLatency_);
    int activeConfigVersion = -1;
    int activeFreshFrameVersion = -1;
    int appliedConfigDiscardVersion = 0;
    int appliedDiscardVersion = 0;
    uint64_t activeCaptureEpoch = 0;
    auto pushPackets = [this](std::vector<EncodedPacket>& packets) {
        for (auto& packet : packets) {
            if (replayStore_) replayStore_->push(packet);
            packets_.push(std::move(packet));
            ++framesEncoded_;
        }
        packets.clear();
    };

    while (true) {
        EncodeJob job;
        {
            std::unique_lock lock(submitMutex_);
            submitCv_.wait(lock, [this] { return !running_.load() || !pendingJobs_.empty(); });
            if (pendingJobs_.empty() && !running_) break;
            if (pendingJobs_.empty()) continue;
            job = std::move(pendingJobs_.front());
            pendingJobs_.pop_front();
            lastDequeuedSourceSequence_ = job.frame.sequence;
        }
        submitCv_.notify_all();

        const int64_t submitStarted100ns = monotonicNow100ns();
        const bool freshEpochChanged = activeFreshFrameVersion >= 0 &&
            job.freshFrameVersion != activeFreshFrameVersion;
        const bool sessionChanged =
            job.configVersion != activeConfigVersion ||
            job.freshFrameVersion != activeFreshFrameVersion ||
            job.frame.captureEpoch != activeCaptureEpoch;
        if (sessionChanged) {
            const int requestedConfigDiscardVersion = discardPacketsAtConfigVersion_.load();
            const bool discardForConfig =
                requestedConfigDiscardVersion > appliedConfigDiscardVersion &&
                job.configVersion >= requestedConfigDiscardVersion;
            if (discardForConfig) {
                packets_.clear();
                if (replayStore_) replayStore_->clear();
                appliedConfigDiscardVersion = requestedConfigDiscardVersion;
            }
            const int requestedDiscardVersion = discardPacketsAtFreshFrameVersion_.load();
            const bool discardForFreshFrame =
                requestedDiscardVersion > appliedDiscardVersion &&
                job.freshFrameVersion >= requestedDiscardVersion;
            if (discardForFreshFrame) {
                packets_.clear();
                if (replayStore_) replayStore_->clear();
                appliedDiscardVersion = requestedDiscardVersion;
            }
            std::vector<EncodedPacket> drainedPackets;
            std::string drainStatus;
            if (!freshEpochChanged && !discardForConfig && !discardForFreshFrame) {
                session->drainPending(drainedPackets, drainStatus);
                pushPackets(drainedPackets);
            }
            session = std::make_unique<NvencSession>(
                packets_,
                &nvencInFlightFrames_,
                &encoderBackpressureDrops_,
                &nvencSurfaceDrops_,
                &nvencInputDrops_,
                &recentOutputEventWaitLatency_,
                &recentOutputLockLatency_,
                &recentOutputCopyLatency_,
                &recentOutputUnmapLatency_);
            nvencInFlightFrames_ = 0;
            nvencPreparedFrames_ = 0;
            activeConfigVersion = job.configVersion;
            activeFreshFrameVersion = job.freshFrameVersion;
            activeCaptureEpoch = job.frame.captureEpoch;
        }

        std::string sessionStatus;
        bool encoderInitialized = false;
        bool encoded = false;
        if (nvencRuntimeLoaded_ && session->initialize(
                job.frame.texture.Get(),
                job.frame.width,
                job.frame.height,
                job.targetWidth,
                job.targetHeight,
                job.maxEncodeWidth,
                job.maxEncodeHeight,
                job.fps,
                job.bitrateMbps,
                job.nvencPreset,
                sessionStatus)) {
            encoderInitialized = true;
            outputWidth_ = session->outputWidth();
            outputHeight_ = session->outputHeight();
            scalingActive_ = job.frame.width != session->outputWidth() ||
                job.frame.height != session->outputHeight();

            std::vector<EncodedPacket> encodedPackets;
            NvencFrameTimings timings;
            encoded = session->encode(std::move(job.frame), encodedPackets, sessionStatus, timings);
            if (timings.inputPath != NvencFrameTimings::InputPath::None) {
                const int64_t timingRecorded100ns = monotonicNow100ns();
                recentInputPreparationLatency_.record(timingRecorded100ns, timings.scale100ns);
                recentInputMapLatency_.record(timingRecorded100ns, timings.inputMap100ns);
                recentNvencCallLatency_.record(timingRecorded100ns, timings.encodeCall100ns);
            }
            switch (timings.inputPath) {
                case NvencFrameTimings::InputPath::DirectCaptureBgra:
                case NvencFrameTimings::InputPath::CachedSurface:
                    nvencZeroCopyFrames_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case NvencFrameTimings::InputPath::BgraCopyFallback:
                    nvencCopyFallbackFrames_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case NvencFrameTimings::InputPath::VideoProcessor:
                    nvencConvertedFrames_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case NvencFrameTimings::InputPath::None:
                    break;
            }
            profiledSubmissions_.fetch_add(1, std::memory_order_relaxed);
            totalScaleLatency100ns_.fetch_add(timings.scale100ns, std::memory_order_relaxed);
            totalInputMapLatency100ns_.fetch_add(timings.inputMap100ns, std::memory_order_relaxed);
            totalNvencCallLatency100ns_.fetch_add(timings.encodeCall100ns, std::memory_order_relaxed);
            updateMaximum(maximumScaleLatency100ns_, timings.scale100ns);
            updateMaximum(maximumInputMapLatency100ns_, timings.inputMap100ns);
            updateMaximum(maximumNvencCallLatency100ns_, timings.encodeCall100ns);
            totalOutputDrainLatency100ns_.store(
                session->averageOutputDrainLatency100ns(), std::memory_order_relaxed);
            maximumOutputDrainLatency100ns_.store(
                session->maximumOutputDrainLatency100ns(), std::memory_order_relaxed);
            nvencInFlightFrames_ = session->inFlightCount();
            nvencPreparedFrames_ = session->preparedCount();
            pushPackets(encodedPackets);
        }

        if (!sessionStatus.empty()) setStatus(sessionStatus);
        if (encoderInitialized && !encoded) {
            std::cerr << "[encoder] Resetting NVENC session after encode failure: " << sessionStatus << std::endl;
            session = std::make_unique<NvencSession>(
                packets_,
                &nvencInFlightFrames_,
                &encoderBackpressureDrops_,
                &nvencSurfaceDrops_,
                &nvencInputDrops_,
                &recentOutputEventWaitLatency_,
                &recentOutputLockLatency_,
                &recentOutputCopyLatency_,
                &recentOutputUnmapLatency_);
            nvencInFlightFrames_ = 0;
            nvencPreparedFrames_ = 0;
            activeConfigVersion = -1;
            activeFreshFrameVersion = -1;
            activeCaptureEpoch = 0;
        }

        const int64_t submitLatency100ns = monotonicNow100ns() - submitStarted100ns;
        int64_t previousMaximum = maximumSubmitLatency100ns_.load();
        while (submitLatency100ns > previousMaximum &&
               !maximumSubmitLatency100ns_.compare_exchange_weak(previousMaximum, submitLatency100ns)) {
        }
    }

    std::vector<EncodedPacket> drainedPackets;
    std::string drainStatus;
    session->drainPending(drainedPackets, drainStatus);
    pushPackets(drainedPackets);
    nvencInFlightFrames_ = 0;
    nvencPreparedFrames_ = 0;
    if (!drainStatus.empty()) setStatus(drainStatus);
}

}  // namespace clipture

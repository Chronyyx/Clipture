#include "clipture/EncoderWorker.hpp"
#include "clipture/H264PacketAnalyzer.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
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

class NvencSession {
public:
    explicit NvencSession(PacketRingBuffer& packetPool)
        : packetPool_(packetPool) {}

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
            encodeConfig_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            encodeConfig_.rcParams.averageBitRate = static_cast<uint32_t>(std::max(1, bitrateMbps) * 1'000'000);
            encodeConfig_.rcParams.maxBitRate = encodeConfig_.rcParams.averageBitRate;
            encodeConfig_.encodeCodecConfig.h264Config.idrPeriod = encodeConfig_.gopLength;
            encodeConfig_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

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
            initialized_ = true;
            initFailureLogged_ = false;
            status = asyncEnabled_
                ? "Direct NVENC H.264 async " + nvencPresetName(boundedPreset) + " session initialized."
                : "Direct NVENC H.264 sync compatibility " + nvencPresetName(boundedPreset) + " session initialized.";
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

    bool needsReconfigure(int outputWidth, int outputHeight, int fps, int bitrateMbps) const {
        if (!initialized_) return false;
        return requestedWidth_ != std::max(1, outputWidth) ||
            requestedHeight_ != std::max(1, outputHeight) ||
            fps_ != std::max(1, fps) ||
            bitrateMbps_ != std::max(1, bitrateMbps);
    }

    int outputWidth() const {
        return width_;
    }

    int outputHeight() const {
        return height_;
    }

    bool canReconfigureTo(int outputWidth, int outputHeight) const {
        const auto encodeSize = fitEncodeDimensions(outputWidth, outputHeight, h264CapWidth_, h264CapHeight_);
        return initialized_ &&
            outputWidth > 0 &&
            outputHeight > 0 &&
            encodeSize.width <= maxEncodeWidth_ &&
            encodeSize.height <= maxEncodeHeight_;
    }

    bool reconfigure(
        int outputWidth,
        int outputHeight,
        int fps,
        int bitrateMbps,
        std::vector<EncodedPacket>& packets,
        std::string& status) {
        if (!initialized_) return true;
        outputWidth = std::max(1, outputWidth);
        outputHeight = std::max(1, outputHeight);
        fps = std::max(1, fps);
        bitrateMbps = std::max(1, bitrateMbps);
        if (!needsReconfigure(outputWidth, outputHeight, fps, bitrateMbps)) return true;
        if (!canReconfigureTo(outputWidth, outputHeight)) {
            status = "NVENC reconfigure needs a larger dynamic encode canvas.";
            return false;
        }
        const int requestedWidth = outputWidth;
        const int requestedHeight = outputHeight;
        const auto encodeSize = fitEncodeDimensions(requestedWidth, requestedHeight, h264CapWidth_, h264CapHeight_);

        if (!drainAll(packets, status)) {
            return false;
        }

        encodeConfig_.gopLength = static_cast<uint32_t>(fps);
        encodeConfig_.rcParams.averageBitRate = static_cast<uint32_t>(bitrateMbps * 1'000'000);
        encodeConfig_.rcParams.maxBitRate = encodeConfig_.rcParams.averageBitRate;
        encodeConfig_.encodeCodecConfig.h264Config.idrPeriod = encodeConfig_.gopLength;

        initParams_.encodeWidth = static_cast<uint32_t>(encodeSize.width);
        initParams_.encodeHeight = static_cast<uint32_t>(encodeSize.height);
        initParams_.darWidth = initParams_.encodeWidth;
        initParams_.darHeight = initParams_.encodeHeight;
        initParams_.frameRateNum = static_cast<uint32_t>(fps);
        initParams_.frameRateDen = 1;
        initParams_.encodeConfig = &encodeConfig_;

        NV_ENC_RECONFIGURE_PARAMS params {};
        params.version = nvencStructVersionForApi(2, apiVersion_, true);
        params.reInitEncodeParams = initParams_;
        params.resetEncoder = 1;
        params.forceIDR = 1;

        const NVENCSTATUS nvStatus = funcs_.nvEncReconfigureEncoder(encoder_, &params);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncReconfigureEncoder failed: " + statusDetails(nvStatus);
            return false;
        }

        scaledOutputView_.Reset();
        scaledTexture_.Reset();
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        requestedWidth_ = requestedWidth;
        requestedHeight_ = requestedHeight;
        width_ = encodeSize.width;
        height_ = encodeSize.height;
        fps_ = fps;
        bitrateMbps_ = bitrateMbps;
        frameIndex_ = 0;
        status = "Direct NVENC reconfigured output to " + std::to_string(width_) + "x" + std::to_string(height_) + ".";
        if (encodeSize.capped) {
            status += " Output was fit to NVENC H.264 caps from " +
                std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) + ".";
        }
        return true;
    }


    bool encode(const CapturedFrame& frame, std::vector<EncodedPacket>& packets, std::string& status) {
        if (!initialized_) {
            status = "NVENC encode called before initialization.";
            return false;
        }

        if (!drainReady(packets, status)) {
            return false;
        }
        while (inFlightOrder_.size() >= outputSlots_.size()) {
            bool drained = false;
            if (!drainOne(packets, true, status, drained)) {
                return false;
            }
            if (!drained) break;
        }

        CapturedFrame inputFrame = frame;
        if (frame.width != width_ || frame.height != height_) {
            auto scaledTexture = scaleFrameToOutput(frame, status);
            if (!scaledTexture) {
                return false;
            }
            inputFrame.texture = scaledTexture;
            inputFrame.width = width_;
            inputFrame.height = height_;
        }

        auto* registeredInput = registeredInputFor(inputFrame.texture.Get(), inputFrame.width, inputFrame.height, status);
        if (!registeredInput) {
            return false;
        }
        while (inputResourceInFlight(registeredInput->registeredResource)) {
            bool drained = false;
            if (!drainOne(packets, true, status, drained)) {
                return false;
            }
            if (!drained) break;
        }

        auto* slot = freeOutputSlot(packets, status);
        if (!slot) {
            return false;
        }
        if (asyncEnabled_ && slot->completionEvent) {
            ResetEvent(slot->completionEvent);
        }

        NV_ENC_MAP_INPUT_RESOURCE mapped {};
        mapped.version = nvencStructVersionForApi(4, apiVersion_);
        mapped.registeredResource = registeredInput->registeredResource;
        NVENCSTATUS nvStatus = funcs_.nvEncMapInputResource(encoder_, &mapped);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncMapInputResource failed: " + statusDetails(nvStatus);
            return false;
        }

        NV_ENC_PIC_PARAMS pic {};
        pic.version = nvencStructVersionForApi(7, apiVersion_, true);
        pic.inputWidth = static_cast<uint32_t>(inputFrame.width);
        pic.inputHeight = static_cast<uint32_t>(inputFrame.height);
        pic.inputPitch = static_cast<uint32_t>(inputFrame.width);
        pic.frameIdx = frameIndex_;
        pic.inputTimeStamp = static_cast<uint64_t>(inputFrame.pts100ns);
        pic.inputDuration = static_cast<uint64_t>(10'000'000LL / fps_);
        pic.inputBuffer = mapped.mappedResource;
        pic.outputBitstream = slot->bitstreamBuffer;
        pic.completionEvent = asyncEnabled_ ? slot->completionEvent : nullptr;
        pic.bufferFmt = mapped.mappedBufferFmt == NV_ENC_BUFFER_FORMAT_UNDEFINED ? NV_ENC_BUFFER_FORMAT_ARGB : mapped.mappedBufferFmt;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        if (frameIndex_ % static_cast<uint32_t>(fps_) == 0) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        nvStatus = funcs_.nvEncEncodePicture(encoder_, &pic);
        if (nvStatus == NV_ENC_ERR_NEED_MORE_INPUT) {
            funcs_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            ++frameIndex_;
            status = "Direct NVENC accepted input and is waiting for more frames.";
            return true;
        }
        if (nvStatus != NV_ENC_SUCCESS) {
            funcs_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            status = "NvEncEncodePicture failed: " + statusDetails(nvStatus);
            return false;
        }

        slot->mappedInput = mapped.mappedResource;
        slot->mappedRegisteredResource = registeredInput->registeredResource;
        slot->framePts100ns = inputFrame.pts100ns;
        slot->frameDuration100ns = 10'000'000LL / fps_;
        slot->frameEncodedWidth = inputFrame.width;
        slot->frameEncodedHeight = inputFrame.height;
        slot->frameSourceWidth = frame.width;
        slot->frameSourceHeight = frame.height;
        if (asyncEnabled_) {
            slot->inFlight = true;
            inFlightOrder_.push_back(slotIndex(slot));
            ++frameIndex_;
            status = "Direct NVENC async pipeline is queueing H.264 frames.";
            return drainReady(packets, status);
        }

        EncodedPacket packet;
        bool produced = false;
        if (!lockOutputSlot(*slot, packet, status, produced)) {
            return false;
        }
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

private:
    struct RegisteredInput {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registeredResource = nullptr;
        int width = 0;
        int height = 0;
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
    };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> scaleFrameToOutput(const CapturedFrame& frame, std::string& status) {
        if (!frame.texture || !device_ || width_ <= 0 || height_ <= 0) {
            status = "NVENC scale failed: invalid frame or output size.";
            return nullptr;
        }
        if (!ensureVideoScaler(frame.width, frame.height, status)) {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc {};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
            frame.texture.Get(),
            videoProcessorEnumerator_.Get(),
            &inputDesc,
            &inputView);
        if (FAILED(hr) || !inputView) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateVideoProcessorInputView HRESULT 0x" << std::hex << hr;
            status = message.str();
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream {};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView.Get();
        hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), scaledOutputView_.Get(), 0, 1, &stream);
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "NVENC scale failed: VideoProcessorBlt HRESULT 0x" << std::hex << hr;
            status = message.str();
            return nullptr;
        }

        return scaledTexture_;
    }

    bool ensureVideoScaler(int inputWidth, int inputHeight, std::string& status) {
        if (scaledTexture_ &&
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

        D3D11_TEXTURE2D_DESC textureDesc {};
        textureDesc.Width = static_cast<UINT>(std::max(1, width_));
        textureDesc.Height = static_cast<UINT>(std::max(1, height_));
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> scaledTexture;
        hr = device_->CreateTexture2D(&textureDesc, nullptr, &scaledTexture);
        if (FAILED(hr) || !scaledTexture) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateTexture2D HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc {};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(scaledTexture.Get(), enumerator.Get(), &outputDesc, &outputView);
        if (FAILED(hr) || !outputView) {
            std::ostringstream message;
            message << "NVENC scale failed: CreateVideoProcessorOutputView HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        RECT sourceRect { 0, 0, std::max(1, inputWidth), std::max(1, inputHeight) };
        RECT destRect { 0, 0, std::max(1, width_), std::max(1, height_) };
        videoContext_->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &sourceRect);
        videoContext_->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destRect);
        videoContext_->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &destRect);

        videoProcessorEnumerator_ = enumerator;
        videoProcessor_ = processor;
        scaledTexture_ = scaledTexture;
        scaledOutputView_ = outputView;
        scalerInputWidth_ = inputWidth;
        scalerInputHeight_ = inputHeight;
        scalerOutputWidth_ = width_;
        scalerOutputHeight_ = height_;
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

    bool inputResourceInFlight(NV_ENC_REGISTERED_PTR registeredResource) const {
        for (const auto& slot : outputSlots_) {
            if (slot.inFlight && slot.mappedRegisteredResource == registeredResource && registeredResource) {
                return true;
            }
        }
        return false;
    }

    bool drainReady(std::vector<EncodedPacket>& packets, std::string& status) {
        while (!inFlightOrder_.empty()) {
            bool drained = false;
            if (!drainOne(packets, false, status, drained)) {
                return false;
            }
            if (!drained) return true;
        }
        return true;
    }

    bool drainAll(std::vector<EncodedPacket>& packets, std::string& status) {
        while (!inFlightOrder_.empty()) {
            bool drained = false;
            if (!drainOne(packets, true, status, drained)) {
                return false;
            }
            if (!drained) return false;
        }
        return true;
    }

    bool drainOne(std::vector<EncodedPacket>& packets, bool wait, std::string& status, bool& drained) {
        drained = false;
        if (inFlightOrder_.empty()) return true;

        const std::size_t index = inFlightOrder_.front();
        if (index >= outputSlots_.size()) {
            status = "NVENC async drain failed: output slot index is invalid.";
            return false;
        }

        auto& slot = outputSlots_[index];
        if (asyncEnabled_) {
            const DWORD waitResult = WaitForSingleObject(slot.completionEvent, wait ? asyncDrainWaitTimeoutMs_ : 0);
            if (waitResult == WAIT_TIMEOUT) {
                if (!wait) return true;
                status = "NVENC async wait timed out after " + std::to_string(asyncDrainWaitTimeoutMs_) + " ms.";
                std::cerr << "[perf] nvenc_async_wait_timeout timeoutMs=" << asyncDrainWaitTimeoutMs_ << std::endl;
                return false;
            }
            if (waitResult != WAIT_OBJECT_0) {
                status = "NVENC async wait failed.";
                return false;
            }
        }

        EncodedPacket packet;
        bool produced = false;
        if (!lockOutputSlot(slot, packet, status, produced)) {
            return false;
        }
        inFlightOrder_.pop_front();
        drained = true;
        if (produced) {
            packets.push_back(std::move(packet));
        }
        return true;
    }

    bool lockOutputSlot(OutputSlot& slot, EncodedPacket& packet, std::string& status, bool& produced) {
        produced = false;
        NV_ENC_LOCK_BITSTREAM lock {};
        lock.version = nvencStructVersionForApi(2, apiVersion_, true);
        lock.outputBitstream = slot.bitstreamBuffer;
        lock.doNotWait = 0;

        NVENCSTATUS nvStatus = funcs_.nvEncLockBitstream(encoder_, &lock);
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
        packet.payload = packetPool_.acquirePayload(lock.bitstreamSizeInBytes);
        if (lock.bitstreamSizeInBytes > 0 && lock.bitstreamBufferPtr) {
            std::memcpy(mutablePayload(packet).data(), lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        }
        analyzeH264Packet(packet);

        funcs_.nvEncUnlockBitstream(encoder_, slot.bitstreamBuffer);
        if (slot.mappedInput) {
            funcs_.nvEncUnmapInputResource(encoder_, slot.mappedInput);
            slot.mappedInput = nullptr;
        }
        slot.mappedRegisteredResource = nullptr;
        slot.framePts100ns = 0;
        slot.frameDuration100ns = 0;
        slot.frameEncodedWidth = 0;
        slot.frameEncodedHeight = 0;
        slot.frameSourceWidth = 0;
        slot.frameSourceHeight = 0;
        slot.inFlight = false;
        produced = !payloadEmpty(packet);
        status = asyncEnabled_
            ? "Direct NVENC async pipeline is outputting H.264 packets."
            : "Direct NVENC is outputting H.264 packets.";
        return true;
    }

    OutputSlot* freeOutputSlot(std::vector<EncodedPacket>& packets, std::string& status) {
        if (outputSlots_.empty()) {
            status = "NVENC output slot pool is empty.";
            return nullptr;
        }

        for (std::size_t attempt = 0; attempt < outputSlots_.size(); ++attempt) {
            const std::size_t index = (nextOutputSlot_ + attempt) % outputSlots_.size();
            if (!outputSlots_[index].inFlight) {
                nextOutputSlot_ = (index + 1) % outputSlots_.size();
                return &outputSlots_[index];
            }
        }

        bool drained = false;
        if (!drainOne(packets, true, status, drained)) {
            return nullptr;
        }
        if (!drained) {
            status = "NVENC output slot pool is full.";
            return nullptr;
        }
        return freeOutputSlot(packets, status);
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
                return !inputResourceInFlight(input.registeredResource);
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
        registered.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        registered.bufferUsage = NV_ENC_INPUT_IMAGE;

        const NVENCSTATUS nvStatus = funcs_.nvEncRegisterResource(encoder_, &registered);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncRegisterResource failed: " + statusDetails(nvStatus);
            return nullptr;
        }

        RegisteredInput input {};
        input.texture = texture;
        input.registeredResource = registered.registeredResource;
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
        if (encoder_) {
            for (auto& slot : outputSlots_) {
                if (slot.mappedInput) {
                    funcs_.nvEncUnmapInputResource(encoder_, slot.mappedInput);
                    slot.mappedInput = nullptr;
                }
                slot.mappedRegisteredResource = nullptr;
                slot.framePts100ns = 0;
                slot.frameDuration100ns = 0;
                slot.frameEncodedWidth = 0;
                slot.frameEncodedHeight = 0;
                slot.frameSourceWidth = 0;
                slot.frameSourceHeight = 0;
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
            inFlightOrder_.clear();
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
        scaledOutputView_.Reset();
        scaledTexture_.Reset();
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        videoContext_.Reset();
        videoDevice_.Reset();
        context_.Reset();
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
        frameIndex_ = 0;
        nextOutputSlot_ = 0;
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
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scaledTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> scaledOutputView_;
    PacketRingBuffer& packetPool_;
    void* encoder_ = nullptr;
    NV_ENC_CONFIG encodeConfig_ {};
    NV_ENC_INITIALIZE_PARAMS initParams_ {};
    GUID presetGuid_ {};
    std::vector<RegisteredInput> registeredInputs_;
    std::vector<OutputSlot> outputSlots_;
    std::deque<std::size_t> inFlightOrder_;
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
    uint32_t frameIndex_ = 0;
    std::size_t nextOutputSlot_ = 0;
    static constexpr std::size_t maxRegisteredInputs_ = 16;
    static constexpr std::size_t outputSlotCount_ = 4;
    static constexpr DWORD asyncDrainWaitTimeoutMs_ = 1000;
};

}  // namespace

EncoderWorker::EncoderWorker(FrameQueue& frames, PacketRingBuffer& packets)
    : frames_(frames), packets_(packets) {}

EncoderWorker::~EncoderWorker() {
    stop();
}

void EncoderWorker::start() {
    if (running_.exchange(true)) return;

    HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!module) module = LoadLibraryW(L"nvEncodeAPI.dll");
    nvencRuntimeLoaded_ = module && GetProcAddress(module, "NvEncodeAPICreateInstance");
    if (module) FreeLibrary(module);

    status_ = nvencRuntimeLoaded_
        ? "NVENC runtime loaded; WGC frames are being handed to the encoder worker."
        : "NVENC runtime was not found; encoder worker cannot produce H.264 packets.";
    thread_ = std::thread(&EncoderWorker::run, this);
}

void EncoderWorker::stop() {
    if (!running_.exchange(false)) return;
    frames_.stop();
    if (thread_.joinable()) thread_.join();
}

bool EncoderWorker::running() const {
    return running_;
}

void EncoderWorker::configure(int fps, int bitrateMbps, int targetWidth, int targetHeight, int maxEncodeWidth, int maxEncodeHeight, int nvencPreset) {
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
    if (changed) ++configVersion_;
}

void EncoderWorker::requireFreshFrame() {
    frames_.clear();
    ++freshFrameVersion_;
}

void EncoderWorker::resetAutoOutputResolution() {
    autoOutputWidth_ = 0;
    autoOutputHeight_ = 0;
    requireFreshFrame();
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

int EncoderWorker::pendingFrames() const {
    return std::max(0, framesAccepted_.load() - framesEncoded_.load());
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
    return status_;
}

void EncoderWorker::run() {
    auto session = std::make_unique<NvencSession>(packets_);
    int activeConfigVersion = configVersion_.load();
    int activeFreshFrameVersion = freshFrameVersion_.load();
    auto pushPackets = [this](std::vector<EncodedPacket>& packets) {
        for (auto& packet : packets) {
            packets_.push(std::move(packet));
            ++framesEncoded_;
        }
        packets.clear();
    };

    std::optional<CapturedFrame> currentFrame;
    while (running_ && !currentFrame) {
        currentFrame = frames_.waitPop();
        if (currentFrame) {
            auto latest = frames_.consumeAllAndGetLatest();
            if (latest) currentFrame = latest;
            ++framesAccepted_;
        }
    }
    
    if (!running_) return;
    
    auto startTick = std::chrono::steady_clock::now();
    int64_t startPts100ns = currentFrame->pts100ns;
    int64_t encodedFrameCount = 0;

    while (running_) {
        const int fps = std::clamp(targetFps_.load(), 24, 60);
        const int64_t minFrameSpacing100ns = 10'000'000LL / std::max(1, fps);
        const auto interval = std::chrono::nanoseconds(minFrameSpacing100ns * 100);

        auto now = std::chrono::steady_clock::now();
        auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTick).count();
        int64_t targetFrameIndex = elapsedNs / interval.count();

        // If targetFrameIndex jumped way ahead (e.g. system sleep), drop frames to catch up without death spiral
        if (targetFrameIndex > encodedFrameCount + 300) {
            encodedFrameCount = targetFrameIndex;
        }

        while (encodedFrameCount <= targetFrameIndex && running_) {
            auto newFrame = frames_.consumeAllAndGetLatest();
            if (newFrame) {
                currentFrame = newFrame;
                ++framesAccepted_;
            }

            if (!currentFrame) break;

            CapturedFrame frameToEncode = *currentFrame;
            frameToEncode.pts100ns = startPts100ns + (encodedFrameCount * minFrameSpacing100ns);
            
            // Encode and push this frame
            const int bitrateMbps = std::clamp(targetBitrateMbps_.load(), 4, 120);
            const int nvencPreset = std::clamp(nvencPreset_.load(), 1, 5);
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
            const int maxEncodeWidth = std::max(maxEncodeWidth_.load(), outputWidth);
            const int maxEncodeHeight = std::max(maxEncodeHeight_.load(), outputHeight);
            sourceWidth_ = frameToEncode.width;
            sourceHeight_ = frameToEncode.height;
            outputWidth_ = outputWidth;
            outputHeight_ = outputHeight;
            scalingActive_ = frameToEncode.width != outputWidth || frameToEncode.height != outputHeight;
            const int currentConfigVersion = configVersion_.load();
            const int currentFreshFrameVersion = freshFrameVersion_.load();
            
            if (currentConfigVersion != activeConfigVersion) {
                std::vector<EncodedPacket> drainedPackets;
                session->drainPending(drainedPackets, status_);
                pushPackets(drainedPackets);
                session = std::make_unique<NvencSession>(packets_);
                activeConfigVersion = currentConfigVersion;
            }

            if (currentFreshFrameVersion != activeFreshFrameVersion) {
                activeFreshFrameVersion = currentFreshFrameVersion;
                session = std::make_unique<NvencSession>(packets_);
                currentFrame.reset();
                auto freshFrame = frames_.waitPop();
                if (!running_) break;
                if (freshFrame) {
                    auto latest = frames_.consumeAllAndGetLatest();
                    currentFrame = latest ? latest : freshFrame;
                    ++framesAccepted_;
                    const auto afterWait = std::chrono::steady_clock::now();
                    const auto afterWaitElapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterWait - startTick).count();
                    const int64_t afterWaitFrameIndex = afterWaitElapsedNs / interval.count();
                    encodedFrameCount = std::max(encodedFrameCount + 1, afterWaitFrameIndex);
                    continue;
                }
                break;
            }

            if (nvencRuntimeLoaded_ && session->initialize(
                    frameToEncode.texture.Get(),
                    frameToEncode.width,
                    frameToEncode.height,
                    outputWidth,
                    outputHeight,
                    maxEncodeWidth,
                    maxEncodeHeight,
                    fps,
                    bitrateMbps,
                    nvencPreset,
                    status_)) {
                std::vector<EncodedPacket> encodedPackets;
                bool encoderReady = true;
                if (session->needsReconfigure(outputWidth, outputHeight, fps, bitrateMbps) &&
                    !session->reconfigure(outputWidth, outputHeight, fps, bitrateMbps, encodedPackets, status_)) {
                    pushPackets(encodedPackets);
                    session = std::make_unique<NvencSession>(packets_);
                    encoderReady = session->initialize(
                        frameToEncode.texture.Get(),
                        frameToEncode.width,
                        frameToEncode.height,
                        outputWidth,
                        outputHeight,
                        std::max(maxEncodeWidth, outputWidth),
                        std::max(maxEncodeHeight, outputHeight),
                        fps,
                        bitrateMbps,
                        nvencPreset,
                        status_);
                }
                if (!encoderReady) {
                    ++encodedFrameCount;
                    continue;
                }
                outputWidth_ = session->outputWidth();
                outputHeight_ = session->outputHeight();
                scalingActive_ = frameToEncode.width != session->outputWidth() || frameToEncode.height != session->outputHeight();
                pushPackets(encodedPackets);
                if (session->encode(frameToEncode, encodedPackets, status_)) {
                    pushPackets(encodedPackets);
                } else {
                    pushPackets(encodedPackets);
                    std::cerr << "[encoder] Resetting NVENC session after encode failure: " << status_ << std::endl;
                    session = std::make_unique<NvencSession>(packets_);
                }
            }
            encodedFrameCount++;
        }

        auto nextTick = startTick + std::chrono::nanoseconds((encodedFrameCount) * interval.count());
        std::this_thread::sleep_until(nextTick);

        // old loop body removed
    }

    std::vector<EncodedPacket> drainedPackets;
    session->drainPending(drainedPackets, status_);
    pushPackets(drainedPackets);
}

}  // namespace clipture

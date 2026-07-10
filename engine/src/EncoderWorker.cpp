#include "clipture/EncoderWorker.hpp"

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
#include <vector>

namespace clipture {
namespace {

using NvEncodeApiCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

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
        case NV_ENC_ERR_GENERIC: return "NV_ENC_ERR_GENERIC";
        default: return "NVENC error " + std::to_string(static_cast<int>(status));
    }
}

bool startCodeAt(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    if (offset + 3 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{1}) {
        size = 3;
        return true;
    }
    if (offset + 4 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{0} &&
        data[offset + 3] == std::byte{1}) {
        size = 4;
        return true;
    }
    return false;
}

bool containsH264NalType(std::span<const std::byte> data, uint8_t wantedType) {
    for (std::size_t i = 0; i + 4 < data.size(); ++i) {
        std::size_t startCodeSize = 0;
        if (!startCodeAt(data, i, startCodeSize)) continue;
        const std::size_t nalOffset = i + startCodeSize;
        if (nalOffset >= data.size()) continue;
        const auto nalType = std::to_integer<uint8_t>(data[nalOffset]) & 0x1F;
        if (nalType == wantedType) return true;
    }
    return false;
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

        module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!module_) module_ = LoadLibraryW(L"nvEncodeAPI.dll");
        if (!module_) {
            status = "NVENC initialize failed: nvEncodeAPI DLL not found.";
            return false;
        }

        auto createInstance = reinterpret_cast<NvEncodeApiCreateInstance>(GetProcAddress(module_, "NvEncodeAPICreateInstance"));
        if (!createInstance) {
            status = "NVENC initialize failed: NvEncodeAPICreateInstance missing.";
            return false;
        }

        funcs_ = {};
        funcs_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS nvStatus = createInstance(&funcs_);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncodeAPICreateInstance failed: " + statusName(nvStatus);
            return false;
        }

        texture->GetDevice(&device_);
        if (!device_) {
            status = "NVENC initialize failed: could not get D3D11 device from captured texture.";
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams {};
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        openParams.device = device_.Get();
        openParams.apiVersion = NVENCAPI_VERSION;
        nvStatus = funcs_.nvEncOpenEncodeSessionEx(&openParams, &encoder_);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncOpenEncodeSessionEx failed: " + statusName(nvStatus);
            return false;
        }

        const int boundedPreset = std::clamp(nvencPreset, 1, 5);
        presetGuid_ = nvencPresetGuid(boundedPreset);
        const GUID presetGuid = presetGuid_;

        NV_ENC_PRESET_CONFIG presetConfig {};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
        nvStatus = funcs_.nvEncGetEncodePresetConfigEx(
            encoder_,
            NV_ENC_CODEC_H264_GUID,
            presetGuid,
            NV_ENC_TUNING_INFO_LOW_LATENCY,
            &presetConfig);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncGetEncodePresetConfigEx failed: " + statusName(nvStatus);
            return false;
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

        maxEncodeWidth_ = std::max(std::max(1, outputWidth), maxEncodeWidth);
        maxEncodeHeight_ = std::max(std::max(1, outputHeight), maxEncodeHeight);

        initParams_ = {};
        initParams_.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initParams_.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initParams_.presetGUID = presetGuid;
        initParams_.encodeWidth = static_cast<uint32_t>(std::max(1, outputWidth));
        initParams_.encodeHeight = static_cast<uint32_t>(std::max(1, outputHeight));
        initParams_.darWidth = initParams_.encodeWidth;
        initParams_.darHeight = initParams_.encodeHeight;
        initParams_.frameRateNum = static_cast<uint32_t>(std::max(1, fps));
        initParams_.frameRateDen = 1;
        asyncEnabled_ = true;
        initParams_.enableEncodeAsync = asyncEnabled_ ? 1 : 0;
        initParams_.enablePTD = 1;
        initParams_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        initParams_.maxEncodeWidth = static_cast<uint32_t>(maxEncodeWidth_);
        initParams_.maxEncodeHeight = static_cast<uint32_t>(maxEncodeHeight_);
        initParams_.encodeConfig = &encodeConfig_;
        nvStatus = funcs_.nvEncInitializeEncoder(encoder_, &initParams_);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncInitializeEncoder failed: " + statusName(nvStatus);
            return false;
        }

        if (!createOutputSlots(status)) {
            return false;
        }

        width_ = std::max(1, outputWidth);
        height_ = std::max(1, outputHeight);
        fps_ = std::max(1, fps);
        bitrateMbps_ = std::max(1, bitrateMbps);
        initialized_ = true;
        status = asyncEnabled_
            ? "Direct NVENC H.264 async " + nvencPresetName(boundedPreset) + " session initialized."
            : "Direct NVENC H.264 " + nvencPresetName(boundedPreset) + " session initialized.";
        return true;
    }

    bool needsReconfigure(int outputWidth, int outputHeight, int fps, int bitrateMbps) const {
        if (!initialized_) return false;
        return width_ != std::max(1, outputWidth) ||
            height_ != std::max(1, outputHeight) ||
            fps_ != std::max(1, fps) ||
            bitrateMbps_ != std::max(1, bitrateMbps);
    }

    bool canReconfigureTo(int outputWidth, int outputHeight) const {
        return initialized_ &&
            outputWidth > 0 &&
            outputHeight > 0 &&
            outputWidth <= maxEncodeWidth_ &&
            outputHeight <= maxEncodeHeight_;
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

        if (!drainAll(packets, status)) {
            return false;
        }

        encodeConfig_.gopLength = static_cast<uint32_t>(fps);
        encodeConfig_.rcParams.averageBitRate = static_cast<uint32_t>(bitrateMbps * 1'000'000);
        encodeConfig_.rcParams.maxBitRate = encodeConfig_.rcParams.averageBitRate;
        encodeConfig_.encodeCodecConfig.h264Config.idrPeriod = encodeConfig_.gopLength;

        initParams_.encodeWidth = static_cast<uint32_t>(outputWidth);
        initParams_.encodeHeight = static_cast<uint32_t>(outputHeight);
        initParams_.darWidth = initParams_.encodeWidth;
        initParams_.darHeight = initParams_.encodeHeight;
        initParams_.frameRateNum = static_cast<uint32_t>(fps);
        initParams_.frameRateDen = 1;
        initParams_.encodeConfig = &encodeConfig_;

        NV_ENC_RECONFIGURE_PARAMS params {};
        params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        params.reInitEncodeParams = initParams_;
        params.resetEncoder = 1;
        params.forceIDR = 1;

        const NVENCSTATUS nvStatus = funcs_.nvEncReconfigureEncoder(encoder_, &params);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncReconfigureEncoder failed: " + statusName(nvStatus);
            return false;
        }

        scaledOutputView_.Reset();
        scaledTexture_.Reset();
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        width_ = outputWidth;
        height_ = outputHeight;
        fps_ = fps;
        bitrateMbps_ = bitrateMbps;
        frameIndex_ = 0;
        status = "Direct NVENC reconfigured output to " + std::to_string(width_) + "x" + std::to_string(height_) + ".";
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
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = registeredInput->registeredResource;
        NVENCSTATUS nvStatus = funcs_.nvEncMapInputResource(encoder_, &mapped);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncMapInputResource failed: " + statusName(nvStatus);
            return false;
        }

        NV_ENC_PIC_PARAMS pic {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
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
            status = "NvEncEncodePicture failed: " + statusName(nvStatus);
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
            bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            NVENCSTATUS nvStatus = funcs_.nvEncCreateBitstreamBuffer(encoder_, &bitstream);
            if (nvStatus != NV_ENC_SUCCESS) {
                status = "NvEncCreateBitstreamBuffer failed: " + statusName(nvStatus);
                return false;
            }
            slot.bitstreamBuffer = bitstream.bitstreamBuffer;

            if (asyncEnabled_) {
                slot.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!slot.completionEvent) {
                    status = "NVENC async event creation failed.";
                    return false;
                }

                NV_ENC_EVENT_PARAMS eventParams {};
                eventParams.version = NV_ENC_EVENT_PARAMS_VER;
                eventParams.completionEvent = slot.completionEvent;
                nvStatus = funcs_.nvEncRegisterAsyncEvent(encoder_, &eventParams);
                if (nvStatus != NV_ENC_SUCCESS) {
                    status = "NvEncRegisterAsyncEvent failed: " + statusName(nvStatus);
                    return false;
                }
                slot.eventRegistered = true;
            }
        }
        return true;
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
            const DWORD waitResult = WaitForSingleObject(slot.completionEvent, wait ? INFINITE : 0);
            if (waitResult == WAIT_TIMEOUT) return true;
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
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = slot.bitstreamBuffer;
        lock.doNotWait = 0;

        NVENCSTATUS nvStatus = funcs_.nvEncLockBitstream(encoder_, &lock);
        if (nvStatus != NV_ENC_SUCCESS) {
            status = "NvEncLockBitstream failed: " + statusName(nvStatus);
            return false;
        }

        packet.kind = PacketKind::Video;
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
        packet.keyframe = packet.keyframe || containsH264NalType(payloadBytes(packet), 5);

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
        registered.version = NV_ENC_REGISTER_RESOURCE_VER;
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
            status = "NvEncRegisterResource failed: " + statusName(nvStatus);
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

    void destroy() {
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
                    eventParams.version = NV_ENC_EVENT_PARAMS_VER;
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
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
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
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrateMbps_ = 40;
    int maxEncodeWidth_ = 0;
    int maxEncodeHeight_ = 0;
    int scalerInputWidth_ = 0;
    int scalerInputHeight_ = 0;
    int scalerOutputWidth_ = 0;
    int scalerOutputHeight_ = 0;
    uint32_t frameIndex_ = 0;
    std::size_t nextOutputSlot_ = 0;
    static constexpr std::size_t maxRegisteredInputs_ = 16;
    static constexpr std::size_t outputSlotCount_ = 4;
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
                if (session->needsReconfigure(outputWidth, outputHeight, fps, bitrateMbps) &&
                    !session->reconfigure(outputWidth, outputHeight, fps, bitrateMbps, encodedPackets, status_)) {
                    pushPackets(encodedPackets);
                    session = std::make_unique<NvencSession>(packets_);
                    session->initialize(
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
                pushPackets(encodedPackets);
                if (session->encode(frameToEncode, encodedPackets, status_)) {
                    pushPackets(encodedPackets);
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

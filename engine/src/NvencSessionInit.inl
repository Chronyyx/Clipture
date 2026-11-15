// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
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

        if (!initializeD3dDevices(texture, status)) {
            return false;
        }
        traceVram(device_.Get(), "encoder-devices");

        const int boundedPreset = std::clamp(nvencPreset, 1, 5);
        struct InitAttempt {
            GUID presetGuid;
            NV_ENC_TUNING_INFO tuningInfo;
            bool async;
            const char* presetFamily;
        };
        const InitAttempt attempts[] = {
            { nvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_LOW_LATENCY,
              !kPreferObsStyleBufferedSyncNvenc, "modern" },
            { nvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_LOW_LATENCY,
              kPreferObsStyleBufferedSyncNvenc, "modern" },
            { legacyNvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_UNDEFINED,
              !kPreferObsStyleBufferedSyncNvenc, "legacy" },
            { legacyNvencPresetGuid(boundedPreset), NV_ENC_TUNING_INFO_UNDEFINED,
              kPreferObsStyleBufferedSyncNvenc, "legacy" },
        };
        // Each preset path tries capped quality first; constant bitrate is the
        // fallback for GPUs or drivers that reject it.
        struct InitStep {
            const InitAttempt* attempt;
            NvencRateControl rateControl;
        };
        std::vector<InitStep> steps;
        for (const auto& attempt : attempts) {
            steps.push_back({ &attempt, NvencRateControl::CappedQuality });
            steps.push_back({ &attempt, NvencRateControl::ConstantBitrate });
        }
        auto logInitAttemptFailure = [this](const InitStep& step, const std::string& reason) {
            if (initFailureLogged_) return;
            const auto& attempt = *step.attempt;
            std::cerr << "[encoder] NVENC init attempt failed"
                      << " rateControl=\"" << nvencRateControlName(step.rateControl) << "\""
                      << " presetFamily=" << attempt.presetFamily
                      << " tuning=" << nvencTuningName(attempt.tuningInfo)
                      << " async=" << (attempt.async ? "true" : "false")
                      << " error=\"" << reason << "\""
                      << std::endl;
        };

        std::string lastFailure;
        for (const auto& step : steps) {
            const auto& attempt = *step.attempt;
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
                logInitAttemptFailure(step, lastFailure);
                continue;
            }

            supportsNv12Input_ = supportsInputFormat(NV_ENC_BUFFER_FORMAT_NV12);
            preferNv12Input_ = supportsNv12Input_ && capturePipelinePolicy().nv12Input;
            supportsArgbInput_ = supportsInputFormat(NV_ENC_BUFFER_FORMAT_ARGB);
            supportsDirectCaptureInput_ = supportsArgbInput_;
            const int asyncSupport = queryEncodeCap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT);
            if (attempt.async && asyncSupport == 0) {
                lastFailure = "NVENC device reports async encode is unsupported.";
                logInitAttemptFailure(step, lastFailure);
                continue;
            }
            if (step.rateControl == NvencRateControl::CappedQuality &&
                !nvencSupportsCappedQuality(queryEncodeCap(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES))) {
                lastFailure = "NVENC device does not report VBR rate control.";
                logInitAttemptFailure(step, lastFailure);
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
                logInitAttemptFailure(step, lastFailure);
                continue;
            }

            encodeConfig_ = presetConfig.presetCfg;
            encodeConfig_.gopLength = static_cast<uint32_t>(std::max(1, fps) * 2);
            encodeConfig_.frameIntervalP = 1;
            encodeConfig_.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
            encodeConfig_.rcParams = nvencRateControlParams(
                step.rateControl, bitrateMbps, nvencStructVersionForApi(1, apiVersion_));
            auto& h264Config = encodeConfig_.encodeCodecConfig.h264Config;
            h264Config.idrPeriod = encodeConfig_.gopLength;
            std::cerr << "[encoder] preset h264 maxNumRefFrames=" << h264Config.maxNumRefFrames
                      << " numRefL0=" << static_cast<int>(h264Config.numRefL0) << '\n';
            if (const int refs = resourceSizingPolicy().nvencMaxRefs; refs > 0) {
                h264Config.maxNumRefFrames = static_cast<uint32_t>(refs);
            }
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
                logInitAttemptFailure(step, lastFailure);
                continue;
            }
            traceVram(device_.Get(), "nvenc-initialized");

            if (!createOutputSlots(status)) {
                lastFailure = status;
                logInitAttemptFailure(step, lastFailure);
                continue;
            }
            traceVram(device_.Get(), "nvenc-output-slots");

            width_ = encodeSize.width;
            height_ = encodeSize.height;
            fps_ = std::max(1, fps);
            bitrateMbps_ = std::max(1, bitrateMbps);
            const int requestedDepth = resourceSizingPolicy().nvencSubmissionDepth;
            preparedSubmissionDepth_ = static_cast<std::size_t>(std::clamp(
                requestedDepth > 0 ? requestedDepth : boundedPreset + 1,
                requestedDepth > 0 ? 1 : 2,
                static_cast<int>(maximumPreparedSubmissionDepth_)));
            syncOutputDelay_ = boundedNvencSyncOutputDelay(
                outputSlotCount_,
                preparedSubmissionDepth_,
                kEnableRoundTransitionOutputHeadroom
                    ? kRoundTransitionSyncOutputDelay
                    : kDefaultSyncOutputDelay);
            initialized_ = true;
            if (asyncEnabled_) startOutputThread();
            initFailureLogged_ = false;
            status = asyncEnabled_
                ? "Direct NVENC H.264 async " + nvencPresetName(boundedPreset) + " session initialized."
                : "Direct NVENC H.264 buffered sync " + nvencPresetName(boundedPreset) + " session initialized.";
            if (separateEncoderDevice_) {
                status += " Capture and NVENC use isolated D3D11 devices.";
            }
            if (useDedicatedInputSurfaces_) {
                status += preferNv12Input_
                    ? " Native-size frames use persistent per-slot NV12 input surfaces."
                    : " Native-size frames use persistent per-slot BGRA input surfaces.";
            } else if (supportsArgbInput_) {
                status += " Native-size frames use zero-copy BGRA input when the capture texture is available.";
            }
            status += " Input preparation depth is " +
                std::to_string(preparedSubmissionDepth_) + ".";
            if (!asyncEnabled_) {
                status += " Buffered output depth is " +
                    std::to_string(syncOutputDelay_) + " of " +
                    std::to_string(outputSlotCount_) + " slots.";
            }
            status += step.rateControl == NvencRateControl::CappedQuality
                ? " Single-pass capped-quality VBR (CQ " + std::to_string(kCappedQualityLevel) +
                    ", at most " + std::to_string(bitrateMbps_) + " Mbps) is active."
                : " Single-pass constant bitrate (" + std::to_string(bitrateMbps_) + " Mbps) is active.";
            if (attempt.presetFamily == std::string("legacy")) {
                status += " Legacy preset fallback is active.";
            }
            if (encodeSize.capped) {
                status += " Output was fit to NVENC H.264 caps from " +
                    std::to_string(requestedWidth) + "x" + std::to_string(requestedHeight) +
                    " to " + std::to_string(width_) + "x" + std::to_string(height_) + ".";
            }
            (void)usedLegacyPresetConfigApi;
            std::cerr << "[encoder] " << status << std::endl;
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


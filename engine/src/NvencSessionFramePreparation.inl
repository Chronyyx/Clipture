// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
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
        // Session teardown clears this cached context. NV12/scaling used to
        // recreate it incidentally; native-size isolated BGRA also needs it.
        if (!context_) device_->GetImmediateContext(&context_);
        if (!context_) {
            status = "NVENC input preparation failed: could not get D3D11 immediate context.";
            return nullptr;
        }
        if (separateEncoderDevice_) {
            return prepareSharedFrameForEncode(frame, slot, inputPath, status);
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

    // CLIPTURE_VRAM_TRACE: every 5 s, process VRAM with the encoder's own surface counts.
    void traceSteadyVram() {
        const int64_t now = monotonicNow100ns();
        if (now - lastVramTrace100ns_ < 50'000'000) return;
        lastVramTrace100ns_ = now;
        int surfaces = 0, bitstreams = 0;
        {
            std::lock_guard lock(outputMutex_);
            for (const auto& slot : outputSlots_) {
                surfaces += slot.scaledTexture ? 1 : 0;
                bitstreams += slot.bitstreamBuffer ? 1 : 0;
            }
        }
        const std::string stage = "steady inputSurfaces=" + std::to_string(surfaces) +
            " bitstreams=" + std::to_string(bitstreams) + " canonical=" + (canonicalTexture_ ? "1" : "0") +
            " registered=" + std::to_string(registeredInputs_.size());
        traceVram(device_.Get(), stage.c_str());
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
        const int64_t now = monotonicNow100ns();
        std::erase_if(inputViews_, [texture, now](const CachedVideoInputView& cached) {
            return cached.texture.Get() != texture && now - cached.lastUsed100ns > kIdleEncoderInputRelease100ns;
        });
        for (auto& cached : inputViews_) {
            if (cached.texture.Get() == texture && cached.scalerGeneration == scalerGeneration_) {
                cached.lastUsed100ns = now;
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
        inputViews_.push_back(CachedVideoInputView { texture, inputView, scalerGeneration_, now });
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


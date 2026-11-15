// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
    bool initializeD3dDevices(ID3D11Texture2D* texture, std::string& status) {
        if (!texture) {
            status = "NVENC initialize failed: captured texture is null.";
            return false;
        }
        if (captureDevice_ && device_) return true;

        texture->GetDevice(&captureDevice_);
        if (!captureDevice_) {
            status = "NVENC initialize failed: could not get the capture D3D11 device.";
            return false;
        }
        captureDevice_->GetImmediateContext(&captureContext_);
        if (!captureContext_) {
            status = "NVENC initialize failed: could not get the capture D3D11 context.";
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D11Multithread> captureMultithread;
        if (SUCCEEDED(captureContext_.As(&captureMultithread)) && captureMultithread) {
            captureMultithread->SetMultithreadProtected(TRUE);
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> captureDxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = captureDevice_.As(&captureDxgiDevice);
        if (FAILED(hr) || !captureDxgiDevice ||
            FAILED(captureDxgiDevice->GetAdapter(&adapter)) || !adapter) {
            status = "NVENC initialize failed: could not resolve the capture GPU adapter.";
            return false;
        }

        if (!capturePipelinePolicy().isolatedDevice) {
            device_ = captureDevice_;
            context_ = captureContext_;
        } else {
            D3D_FEATURE_LEVEL featureLevels[] {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
            };
            D3D_FEATURE_LEVEL selectedFeatureLevel {};
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            hr = D3D11CreateDevice(
                adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                flags,
                featureLevels,
                ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION,
                &device_,
                &selectedFeatureLevel,
                &context_);
            if (FAILED(hr)) {
                flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                hr = D3D11CreateDevice(
                    adapter.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    flags,
                    featureLevels,
                    ARRAYSIZE(featureLevels),
                    D3D11_SDK_VERSION,
                    &device_,
                    &selectedFeatureLevel,
                    &context_);
            }
            if (FAILED(hr) || !device_ || !context_) {
                std::ostringstream message;
                message << "NVENC initialize failed: separate D3D11 encoder device creation failed, HRESULT 0x"
                        << std::hex << hr;
                status = message.str();
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Multithread> encoderMultithread;
            if (SUCCEEDED(context_.As(&encoderMultithread)) && encoderMultithread) {
                encoderMultithread->SetMultithreadProtected(TRUE);
            }
            if (capturePipelinePolicy().gpuHandoff &&
                !gpuHandoff_.initialize(captureDevice_.Get(), device_.Get())) {
                // Older drivers retain the working shared-device path rather
                // than silently introducing a synchronous keyed-mutex wait.
                std::cerr << "[encoder-pipeline] Shared GPU fences unavailable; using shared-device NV12 fallback.\n";
                device_ = captureDevice_;
                context_ = captureContext_;
            }
        }

        if constexpr (kEnableEncoderGpuSchedulingHeadroom) {
            const INT encoderGpuPriority = capturePipelinePolicy().raisedGpuPriority ? capturePipelinePolicy().gpuThreadPriority : 0;
            Microsoft::WRL::ComPtr<IDXGIDevice> encoderDxgiDevice;
            const HRESULT dxgiDeviceHr = device_.As(&encoderDxgiDevice);
            INT previousGpuPriority = 0;
            INT activeGpuPriority = 0;
            HRESULT getPriorityBeforeHr = E_NOINTERFACE;
            HRESULT setPriorityHr = E_NOINTERFACE;
            HRESULT getPriorityAfterHr = E_NOINTERFACE;
            if (SUCCEEDED(dxgiDeviceHr) && encoderDxgiDevice) {
                getPriorityBeforeHr = encoderDxgiDevice->GetGPUThreadPriority(&previousGpuPriority);
                setPriorityHr = encoderDxgiDevice->SetGPUThreadPriority(encoderGpuPriority);
                getPriorityAfterHr = encoderDxgiDevice->GetGPUThreadPriority(&activeGpuPriority);
            }

            Microsoft::WRL::ComPtr<IDXGIDevice1> encoderDxgiDevice1;
            const HRESULT dxgiDevice1Hr = device_.As(&encoderDxgiDevice1);
            const HRESULT maximumLatencyHr = SUCCEEDED(dxgiDevice1Hr) && encoderDxgiDevice1
                ? encoderDxgiDevice1->SetMaximumFrameLatency(kEncoderMaximumFrameLatency)
                : E_NOINTERFACE;

            std::cerr << "[encoder] D3D11 scheduling"
                      << " gpuPriorityRequested=" << encoderGpuPriority
                      << " gpuPriorityPrevious="
                      << (SUCCEEDED(getPriorityBeforeHr) ? std::to_string(previousGpuPriority) : "unknown")
                      << " gpuPriorityActive="
                      << (SUCCEEDED(getPriorityAfterHr) ? std::to_string(activeGpuPriority) : "unknown")
                      << " gpuPrioritySetHr=" << hex32(static_cast<uint32_t>(setPriorityHr))
                      << " maximumFrameLatency=" << kEncoderMaximumFrameLatency
                      << " maximumFrameLatencyHr=" << hex32(static_cast<uint32_t>(maximumLatencyHr))
                      << ".\n";
        }
        separateEncoderDevice_ = device_.Get() != captureDevice_.Get();
        std::cerr << "[encoder-pipeline] isolated=" << separateEncoderDevice_
                  << " preferNV12=" << capturePipelinePolicy().nv12Input << '\n';
        return true;
    }

    void resetPreparedFrameBridge() {
        gpuHandoff_ = {};
        sourceSnapshot_ = {};
        earlyRetireLogged_ = false;
        canonicalOutputView_.Reset();
        canonicalTexture_.Reset();
        canonicalCaptureEpoch_ = 0;
        canonicalSourceFrameSequence_ = 0;
        canonicalScalerGeneration_ = 0;
        freshConversion_ = {};
        encoderBridgeMutex_.Reset();
        captureBridgeMutex_.Reset();
        encoderBridgeTexture_.Reset();
        captureBridgeTexture_.Reset();
        bridgeCaptureDevice_.Reset();
        bridgeWidth_ = 0;
        bridgeHeight_ = 0;
        bridgeFormat_ = DXGI_FORMAT_UNKNOWN;
        inputViews_.clear();
    }

    bool ensureSharedCaptureBridge(const CapturedFrame& frame, std::string& status) {
        if (!frame.texture || !captureDevice_ || !device_) {
            status = "NVENC shared-frame bridge failed: capture or encoder device is unavailable.";
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> frameDevice;
        frame.texture->GetDevice(&frameDevice);
        D3D11_TEXTURE2D_DESC frameDesc {};
        frame.texture->GetDesc(&frameDesc);
        if (captureBridgeTexture_ && encoderBridgeTexture_ &&
            bridgeCaptureDevice_.Get() == frameDevice.Get() &&
            bridgeWidth_ == frameDesc.Width &&
            bridgeHeight_ == frameDesc.Height &&
            bridgeFormat_ == frameDesc.Format) {
            return true;
        }

        resetPreparedFrameBridge();
        if (!frameDevice || frameDevice.Get() != captureDevice_.Get()) {
            status = "NVENC shared-frame bridge failed: captured texture changed D3D11 devices.";
            return false;
        }

        D3D11_TEXTURE2D_DESC bridgeDesc {};
        bridgeDesc.Width = frameDesc.Width;
        bridgeDesc.Height = frameDesc.Height;
        bridgeDesc.MipLevels = 1;
        bridgeDesc.ArraySize = 1;
        bridgeDesc.Format = frameDesc.Format;
        bridgeDesc.SampleDesc.Count = 1;
        bridgeDesc.Usage = D3D11_USAGE_DEFAULT;
        bridgeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const bool gpuHandoff = capturePipelinePolicy().gpuHandoff &&
            gpuHandoff_.initialize(captureDevice_.Get(), device_.Get());
        if (capturePipelinePolicy().gpuHandoff && !gpuHandoff) {
            status = "NVENC shared-frame bridge failed: GPU fence initialization failed.";
            return false;
        }
        bridgeDesc.MiscFlags = gpuHandoff
            ? D3D11_RESOURCE_MISC_SHARED : D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = captureDevice_->CreateTexture2D(&bridgeDesc, nullptr, &captureBridgeTexture_);
        if (FAILED(hr) || !captureBridgeTexture_) {
            std::ostringstream message;
            message << "NVENC shared-frame bridge creation failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIResource> sharedResource;
        HANDLE sharedHandle = nullptr;
        hr = captureBridgeTexture_.As(&sharedResource);
        if (FAILED(hr) || !sharedResource || FAILED(sharedResource->GetSharedHandle(&sharedHandle)) ||
            !sharedHandle) {
            status = "NVENC shared-frame bridge failed: shared texture handle is unavailable.";
            resetPreparedFrameBridge();
            return false;
        }
        hr = device_->OpenSharedResource(
            sharedHandle,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(encoderBridgeTexture_.GetAddressOf()));
        if (FAILED(hr) || !encoderBridgeTexture_) {
            std::ostringstream message;
            message << "NVENC shared-frame bridge open failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            resetPreparedFrameBridge();
            return false;
        }
        if (!gpuHandoff) {
            hr = captureBridgeTexture_.As(&captureBridgeMutex_);
            if (FAILED(hr) || !captureBridgeMutex_ ||
                FAILED(encoderBridgeTexture_.As(&encoderBridgeMutex_)) || !encoderBridgeMutex_) {
                status = "NVENC shared-frame bridge failed: keyed mutex is unavailable.";
                resetPreparedFrameBridge();
                return false;
            }
        }
        std::cerr << "[encoder-pipeline] handoff=" << (gpuHandoff ? "gpu-fence" : "keyed-mutex") << '\n';

        bridgeCaptureDevice_ = frameDevice;
        bridgeWidth_ = frameDesc.Width;
        bridgeHeight_ = frameDesc.Height;
        bridgeFormat_ = frameDesc.Format;
        return true;
    }

    bool ensureCanonicalSurface(int inputWidth, int inputHeight, std::string& status) {
        const DXGI_FORMAT format = supportsNv12Input_ &&
            (preferNv12Input_ || !supportsArgbInput_)
            ? DXGI_FORMAT_NV12
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool processing = format == DXGI_FORMAT_NV12 || inputWidth != width_ || inputHeight != height_;
        if (processing && !ensureVideoScaler(inputWidth, inputHeight, status)) {
            return false;
        }

        if (canonicalTexture_) {
            D3D11_TEXTURE2D_DESC existing {};
            canonicalTexture_->GetDesc(&existing);
            const bool generationMatches = !processing ||
                canonicalScalerGeneration_ == scalerGeneration_;
            if (existing.Width == static_cast<UINT>(width_) &&
                existing.Height == static_cast<UINT>(height_) &&
                existing.Format == format && generationMatches) {
                return true;
            }
            canonicalOutputView_.Reset();
            canonicalTexture_.Reset();
        }

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
        HRESULT hr = device_->CreateTexture2D(&textureDesc, nullptr, &canonicalTexture_);
        if (FAILED(hr) || !canonicalTexture_) {
            std::ostringstream message;
            message << "NVENC canonical surface creation failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }

        if (processing) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc {};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;
            hr = videoDevice_->CreateVideoProcessorOutputView(
                canonicalTexture_.Get(),
                videoProcessorEnumerator_.Get(),
                &outputDesc,
                &canonicalOutputView_);
            if (FAILED(hr) || !canonicalOutputView_) {
                std::ostringstream message;
                message << "NVENC canonical processor view creation failed, HRESULT 0x" << std::hex << hr;
                status = message.str();
                canonicalTexture_.Reset();
                return false;
            }
            if (videoContext1_) {
                videoContext1_->VideoProcessorSetOutputColorSpace1(
                    videoProcessor_.Get(),
                    format == DXGI_FORMAT_NV12 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            }
        }
        canonicalScalerGeneration_ = scalerGeneration_;
        canonicalCaptureEpoch_ = 0;
        canonicalSourceFrameSequence_ = 0;
        return true;
    }

    bool beginLegacyFrameHandoff(const CapturedFrame& frame, std::string& status) {
        constexpr DWORD bridgeWaitMs = 1000;
        HRESULT hr = captureBridgeMutex_->AcquireSync(0, bridgeWaitMs);
        if (hr != S_OK) {
            std::ostringstream message;
            message << "NVENC shared-frame producer wait failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }
        captureContext_->CopyResource(captureBridgeTexture_.Get(), frame.texture.Get());
        hr = captureBridgeMutex_->ReleaseSync(1);
        if (FAILED(hr)) {
            status = "NVENC shared-frame producer release failed.";
            return false;
        }

        hr = encoderBridgeMutex_->AcquireSync(1, bridgeWaitMs);
        if (hr != S_OK) {
            std::ostringstream message;
            message << "NVENC shared-frame consumer wait failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }
        return true;
    }

    bool prepareCanonicalFrame(const CapturedFrame& frame, std::string& status, OutputSlot* directOutput = nullptr) {
        bool direct = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
        if (capturePipelinePolicy().directTextureRead && frame.gpuReadState && !directReadUnavailable_) {
            if (directReader_.active() || directReader_.initialize(captureDevice_.Get(), device_.Get())) {
                source = directReader_.open(frame.texture.Get(), frame.gpuReadState, status);
                direct = source != nullptr;
            }
            if (!direct) {
                directReadUnavailable_ = true;
                directReader_ = {};
                std::cerr << "[encoder-pipeline] Direct source sharing unavailable; retaining GPU bridge fallback.\n";
                status.clear();
            } else if (!directReadLogged_) {
                std::cerr << "[encoder-pipeline] handoff=direct-texture-read producerWait=false\n";
                directReadLogged_ = true;
            }
        }
        if (!direct) {
            if (!ensureSharedCaptureBridge(frame, status)) return false;
            source = encoderBridgeTexture_;
        }
        if (!ensureCanonicalSurface(frame.width, frame.height, status)) {
            if (!preferNv12Input_ || !supportsArgbInput_) return false;
            preferNv12Input_ = false;
            canonicalOutputView_.Reset();
            canonicalTexture_.Reset();
            std::cerr << "[encoder-pipeline] NV12 canonical conversion unavailable; retaining isolated BGRA fallback.\n";
            if (!ensureCanonicalSurface(frame.width, frame.height, status)) return false;
        }
        D3D11_TEXTURE2D_DESC outputDesc {};
        canonicalTexture_->GetDesc(&outputDesc);
        const bool directOutputActive = directOutput && direct && canonicalOutputView_ &&
            outputDesc.Format == DXGI_FORMAT_NV12;
        if (directOutputActive) {
            // Allocate/validate before arming the source fence. The slot is
            // reserved and unmapped; no in-flight NVENC surface is overwritten.
            if (!ensureEncoderInputSurface(*directOutput, status)) return false;
            if (FAILED(nv12ConversionTarget(videoDevice_.Get(), videoProcessorEnumerator_.Get(),
                    directOutput->scaledTexture.Get(), directOutput->scaledOutputView))) {
                status = "NVENC direct conversion output view creation failed.";
                return false;
            }
        }
        const bool earlyRetire = direct && canonicalOutputView_ && capturePipelinePolicy().earlySourceRetire;
        if (earlyRetire) {
            source = sourceSnapshot_.copyAndRetire(device_.Get(), context_.Get(),
                directReader_, source.Get(), frame.gpuReadState, status);
            if (!source) return false; // Armed-reader errors fail closed; no unsafe fallback.
            if (!earlyRetireLogged_) {
                std::cerr << "[encoder-pipeline] source-retirement=after-private-copy before-conversion=true\n";
                earlyRetireLogged_ = true;
            }
        } else if (direct) {
            if (!directReader_.begin(frame.gpuReadState, status)) return false;
        } else if (gpuHandoff_.active()) {
            if (!gpuHandoff_.begin(captureBridgeTexture_.Get(), frame.texture.Get(), status)) return false;
        } else if (!beginLegacyFrameHandoff(frame, status)) {
            return false;
        }

        bool prepared = true;
        HRESULT hr = S_OK;
        D3D11_TEXTURE2D_DESC canonicalDesc {};
        canonicalTexture_->GetDesc(&canonicalDesc);
        if (canonicalOutputView_) {
            auto inputView = videoProcessorInputView(source.Get(), status);
            if (!inputView) {
                prepared = false;
            } else {
                D3D11_VIDEO_PROCESSOR_STREAM stream {};
                stream.Enable = TRUE;
                stream.pInputSurface = inputView.Get();
                auto sample = conversionProbe_.scope(context_.Get());
                hr = videoContext_->VideoProcessorBlt(
                    videoProcessor_.Get(), directOutputActive ? directOutput->scaledOutputView.Get() : canonicalOutputView_.Get(), 0, 1, &stream);
                if (FAILED(hr)) {
                    std::ostringstream message;
                    message << "NVENC canonical conversion failed, HRESULT 0x" << std::hex << hr;
                    status = message.str();
                    prepared = false;
                }
            }
        } else {
            context_->CopyResource(canonicalTexture_.Get(), source.Get());
        }

        const HRESULT releaseHr = earlyRetire ? S_OK : direct ? (directReader_.end(status) ? S_OK : E_FAIL) : gpuHandoff_.active()
            ? (gpuHandoff_.end(status) ? S_OK : E_FAIL)
            : encoderBridgeMutex_->ReleaseSync(0);
        if (FAILED(releaseHr)) {
            status = "NVENC shared-frame consumer release failed.";
            return false;
        }
        if (!prepared) return false;

        if (directOutputActive) {
            directOutput->surfaceCaptureEpoch = frame.captureEpoch;
            directOutput->surfaceSourceFrameSequence = frame.sequence;
            freshConversion_.converted(frame.captureEpoch, frame.sequence);
            if (!directFreshLogged_) {
                std::cerr << "[encoder-pipeline] conversion=direct-fresh-nv12 repeatCache=lazy priorityUnchanged=true\n";
                directFreshLogged_ = true;
            }
            return true; // Canonical pixels/identity were not updated.
        }

        canonicalCaptureEpoch_ = frame.captureEpoch;
        canonicalSourceFrameSequence_ = frame.sequence;
        return true;
    }

    // BGRA input without the canonical intermediate: the capture slot is copied
    // straight into the NVENC input surface. A repeated tick copies it again;
    // the scheduler keeps the slot leased while it is the newest frame.
    bool copySharedFrameInto(const CapturedFrame& frame, ID3D11Texture2D* destination, std::string& status) {
        bool direct = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
        if (capturePipelinePolicy().directTextureRead && frame.gpuReadState && !directReadUnavailable_) {
            if (directReader_.active() || directReader_.initialize(captureDevice_.Get(), device_.Get())) {
                source = directReader_.open(frame.texture.Get(), frame.gpuReadState, status);
                direct = source != nullptr;
            }
            if (!direct) {
                directReadUnavailable_ = true;
                directReader_ = {};
                std::cerr << "[encoder-pipeline] Direct source sharing unavailable; retaining GPU bridge fallback.\n";
                status.clear();
            }
        }
        if (!direct) {
            if (!ensureSharedCaptureBridge(frame, status)) return false;
            source = encoderBridgeTexture_;
        }
        if (direct) {
            if (!directReader_.begin(frame.gpuReadState, status)) return false;
        } else if (gpuHandoff_.active()) {
            if (!gpuHandoff_.begin(captureBridgeTexture_.Get(), frame.texture.Get(), status)) return false;
        } else if (!beginLegacyFrameHandoff(frame, status)) {
            return false;
        }
        context_->CopyResource(destination, source.Get());
        const bool released = direct ? directReader_.end(status)
            : gpuHandoff_.active() ? gpuHandoff_.end(status)
            : SUCCEEDED(encoderBridgeMutex_->ReleaseSync(0));
        if (!released) {
            status = "NVENC shared-frame consumer release failed.";
            return false;
        }
        return true;
    }

    bool ensureEncoderInputSurface(OutputSlot& slot, std::string& status) {
        if (!canonicalTexture_) {
            status = "NVENC input surface failed: canonical frame is unavailable.";
            return false;
        }
        D3D11_TEXTURE2D_DESC canonicalDesc {};
        canonicalTexture_->GetDesc(&canonicalDesc);
        if (slot.scaledTexture) {
            D3D11_TEXTURE2D_DESC existing {};
            slot.scaledTexture->GetDesc(&existing);
            if (existing.Width == canonicalDesc.Width &&
                existing.Height == canonicalDesc.Height &&
                existing.Format == canonicalDesc.Format && slot.scaledViewGeneration == canonicalScalerGeneration_) {
                return true;
            }
            slot.scaledOutputView.Reset();
            slot.scaledTexture.Reset();
            slot.surfaceCaptureEpoch = 0;
            slot.surfaceSourceFrameSequence = 0;
        }

        canonicalDesc.MiscFlags = 0;
        canonicalDesc.CPUAccessFlags = 0;
        canonicalDesc.Usage = D3D11_USAGE_DEFAULT;
        const HRESULT hr = device_->CreateTexture2D(&canonicalDesc, nullptr, &slot.scaledTexture);
        if (FAILED(hr) || !slot.scaledTexture) {
            std::ostringstream message;
            message << "NVENC registered input surface creation failed, HRESULT 0x" << std::hex << hr;
            status = message.str();
            return false;
        }
        slot.scaledViewGeneration = canonicalScalerGeneration_;
        slot.surfaceCaptureEpoch = 0;
        slot.surfaceSourceFrameSequence = 0;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> prepareSharedFrameForEncode(
        const CapturedFrame& frame,
        OutputSlot& slot,
        NvencFrameTimings::InputPath& inputPath,
        std::string& status) {
        if (capturePipelinePolicy().skipCanonicalCopy && !preferNv12Input_ && supportsArgbInput_ &&
            frame.width == width_ && frame.height == height_) {
            if (!ensureBgraCopyOutput(slot, status)) return nullptr;
            const bool slotAlreadyContainsFrame = frame.sequence != 0 &&
                slot.surfaceCaptureEpoch == frame.captureEpoch &&
                slot.surfaceSourceFrameSequence == frame.sequence;
            if (!slotAlreadyContainsFrame) {
                if (!copySharedFrameInto(frame, slot.scaledTexture.Get(), status)) return nullptr;
                slot.surfaceCaptureEpoch = frame.captureEpoch;
                slot.surfaceSourceFrameSequence = frame.sequence;
            }
            inputPath = slotAlreadyContainsFrame
                ? NvencFrameTimings::InputPath::CachedSurface
                : NvencFrameTimings::InputPath::BgraCopyFallback;
            return slot.scaledTexture;
        }
        const bool directFresh = freshConversion_.direct(
            capturePipelinePolicy().directFreshConversion && !capturePipelinePolicy().earlySourceRetire &&
                preferNv12Input_ && supportsNv12Input_ &&
                (canonicalCaptureEpoch_ != frame.captureEpoch || canonicalSourceFrameSequence_ != frame.sequence),
            frame.captureEpoch, frame.sequence);
        if (directFresh) {
            if (!prepareCanonicalFrame(frame, status, &slot)) return nullptr;
            if (slot.surfaceCaptureEpoch == frame.captureEpoch && slot.surfaceSourceFrameSequence == frame.sequence) {
                inputPath = NvencFrameTimings::InputPath::VideoProcessor;
                return slot.scaledTexture;
            }
            // Capability fallback prepared canonical pixels, not a direct slot.
        }
        const bool canonicalChanged =
            !canonicalTexture_ ||
            canonicalCaptureEpoch_ != frame.captureEpoch ||
            canonicalSourceFrameSequence_ != frame.sequence;
        if (canonicalChanged && !prepareCanonicalFrame(frame, status)) return nullptr;
        if (!ensureEncoderInputSurface(slot, status)) return nullptr;

        const bool slotAlreadyContainsFrame =
            frame.sequence != 0 &&
            slot.surfaceCaptureEpoch == frame.captureEpoch &&
            slot.surfaceSourceFrameSequence == frame.sequence;
        if (!slotAlreadyContainsFrame) {
            context_->CopyResource(slot.scaledTexture.Get(), canonicalTexture_.Get());
            slot.surfaceCaptureEpoch = frame.captureEpoch;
            slot.surfaceSourceFrameSequence = frame.sequence;
        }
        inputPath = canonicalChanged
            ? NvencFrameTimings::InputPath::VideoProcessor
            : (slotAlreadyContainsFrame
                ? NvencFrameTimings::InputPath::CachedSurface
                : NvencFrameTimings::InputPath::BgraCopyFallback);
        return slot.scaledTexture;
    }


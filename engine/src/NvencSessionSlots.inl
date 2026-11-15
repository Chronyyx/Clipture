// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
    bool createOutputSlots(std::string& status) {
        outputSlots_.resize(outputSlotCount_);
        for (std::size_t index = 0; index < warmNvencOutputSlots() && index < outputSlots_.size(); ++index) {
            if (!ensureOutputSlotBuffers(outputSlots_[index], status)) return false;
        }
        return true;
    }

    bool ensureOutputSlotBuffers(OutputSlot& slot, std::string& status) {
        if (!slot.bitstreamBuffer) {
            NV_ENC_CREATE_BITSTREAM_BUFFER bitstream {};
            bitstream.version = nvencStructVersionForApi(1, apiVersion_);
            const NVENCSTATUS nvStatus = funcs_.nvEncCreateBitstreamBuffer(encoder_, &bitstream);
            if (nvStatus != NV_ENC_SUCCESS) {
                status = "NvEncCreateBitstreamBuffer failed: " + statusDetails(nvStatus);
                return false;
            }
            slot.bitstreamBuffer = bitstream.bitstreamBuffer;
            observeOutputSlots(allocatedOutputSlots_.fetch_add(1) + 1);
        }

        if (asyncEnabled_ && !slot.eventRegistered) {
            if (!slot.completionEvent) slot.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
            const NVENCSTATUS nvStatus = funcs_.nvEncRegisterAsyncEvent(encoder_, &eventParams);
            if (nvStatus != NV_ENC_SUCCESS) {
                status = "NvEncRegisterAsyncEvent failed: " + statusDetails(nvStatus);
                return false;
            }
            slot.eventRegistered = true;
        }
        return true;
    }

    void releaseOutputSlotBuffers(OutputSlot& slot) {
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
            observeOutputSlots(allocatedOutputSlots_.fetch_sub(1) - 1);
        }
        if (slot.completionEvent) {
            CloseHandle(slot.completionEvent);
            slot.completionEvent = nullptr;
        }
    }

    void releaseOutputSlotSurface(OutputSlot& slot) {
        if (slot.scaledTexture) {
            std::erase_if(registeredInputs_, [this, &slot](const RegisteredInput& input) {
                if (input.texture.Get() != slot.scaledTexture.Get()) return false;
                if (input.registeredResource) funcs_.nvEncUnregisterResource(encoder_, input.registeredResource);
                return true;
            });
        }
        slot.scaledOutputView.Reset();
        slot.scaledTexture.Reset();
        slot.scaledViewGeneration = 0;
        slot.surfaceCaptureEpoch = 0;
        slot.surfaceSourceFrameSequence = 0;
    }

    // Frees one slot that has not been reserved recently. Reservations take the
    // lowest free index, so idle slots collect at the top. Past the warm set the
    // whole slot goes; warm slots keep their small bitstream buffer but drop
    // their full-frame input surface, so surfaces follow the frames actually in
    // flight rather than the largest burst since the session started.
    void releaseIdleOutputSlot() {
        const int64_t now = monotonicNow100ns();
        OutputSlot* idle = nullptr;
        bool warm = false;
        {
            std::lock_guard lock(outputMutex_);
            for (std::size_t index = outputSlots_.size(); index > 0; --index) {
                auto& slot = outputSlots_[index - 1];
                warm = index <= warmNvencOutputSlots();
                if (slot.inFlight || (!slot.scaledTexture && (warm || !slot.bitstreamBuffer))) continue;
                if (now - slot.lastReserved100ns < kIdleNvencOutputSlotRelease100ns) continue;
                // Reserve it so no frame picks the slot while its buffers are freed.
                slot.inFlight = true;
                idle = &slot;
                break;
            }
        }
        if (!idle) return;
        releaseOutputSlotSurface(*idle);
        if (!warm) releaseOutputSlotBuffers(*idle);
        releaseOutputSlotReservation(*idle);
        if (outputSlotUsage_) ++outputSlotUsage_->idleReleases;
    }

    void observeOutputSlots(int count) {
        if (outputSlotUsage_) outputSlotUsage_->observe(count);
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
        // Buffered sync output is drained by submitPrepared only after the
        // configured pipeline depth is reached. Draining here would serialize
        // every frame and make syncOutputDelay_ ineffective.
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
        if (!asyncEnabled_) {
            while (inFlightCount_.load(std::memory_order_relaxed) > 0) {
                if (!drainOldestSyncOutput(packets, status)) return false;
            }
            return true;
        }
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

    bool drainOldestSyncOutput(
        std::vector<EncodedPacket>& packets,
        std::string& status) {
        std::size_t index = 0;
        {
            std::lock_guard lock(outputMutex_);
            if (inFlightOrder_.empty()) return true;
            index = inFlightOrder_.front();
        }
        if (index >= outputSlots_.size()) {
            status = "NVENC buffered sync drain failed: output slot index is invalid.";
            return false;
        }

        auto& slot = outputSlots_[index];
        EncodedPacket packet;
        bool produced = false;
        NvencOutputTimings outputTimings;
        const int64_t drainStarted100ns = monotonicNow100ns();
        if (!lockOutputSlot(slot, packet, status, produced, outputTimings)) return false;
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
        }
        outputCv_.notify_all();
        if (produced) packets.push_back(std::move(packet));
        status = "Direct NVENC buffered sync pipeline is outputting H.264 packets.";
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
        lock.version = nvencStructVersionForApi(2, apiVersion_, true);
        lock.outputBitstream = slot.bitstreamBuffer;
        // NVIDIA recommends a blocking lock for DirectX input. In async mode
        // the completion event has already fired before this call.
        lock.doNotWait = 0;
        const NVENCSTATUS nvStatus = funcs_.nvEncLockBitstream(encoder_, &lock);
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
        packet.sourceHadDesktopPresent = slot.sourceHadDesktopPresent;
        packet.sourceHadPointerUpdate = slot.sourceHadPointerUpdate;
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
            auto reserve = [](OutputSlot& slot) {
                slot.inFlight = true;
                slot.lastReserved100ns = monotonicNow100ns();
                return &slot;
            };
            if (frame.sequence != 0) {
                for (auto& slot : outputSlots_) {
                    if (!slot.inFlight &&
                        slot.surfaceCaptureEpoch == frame.captureEpoch &&
                        slot.surfaceSourceFrameSequence == frame.sequence) {
                        return reserve(slot);
                    }
                }
            }
            // Lowest index first keeps steady-state work on the warm slots.
            for (auto& slot : outputSlots_) {
                if (!slot.inFlight) return reserve(slot);
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
        slot.sourceHadDesktopPresent = false;
        slot.sourceHadPointerUpdate = false;
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

        const int64_t now = monotonicNow100ns();
        std::erase_if(registeredInputs_, [this, texture, now](const RegisteredInput& input) {
            if (input.texture.Get() == texture || now - input.lastUsed100ns <= kIdleEncoderInputRelease100ns) return false;
            if (inputResourcePendingOrInFlight(input.registeredResource)) return false;
            if (input.registeredResource) funcs_.nvEncUnregisterResource(encoder_, input.registeredResource);
            return true;
        });
        for (auto& input : registeredInputs_) {
            if (input.texture.Get() == texture && input.width == width && input.height == height) {
                input.lastUsed100ns = now;
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

        texture->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);

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
        input.lastUsed100ns = now;
        registeredInputs_.push_back(std::move(input));
        return &registeredInputs_.back();
    }

    void destroyEncoderResources() {
        conversionProbe_ = GpuStageProbe("canonical-conversion");
        initialized_ = false;
        if (encoder_) {
            std::vector<EncodedPacket> ignoredPackets;
            std::string ignoredStatus;
            drainAll(ignoredPackets, ignoredStatus);
        }
        stopOutputThread();
        directReader_ = {};
        directReadUnavailable_ = directReadLogged_ = false;
        if (encoder_) {
            for (auto& slot : outputSlots_) {
                if (slot.mappedInput) {
                    funcs_.nvEncUnmapInputResource(encoder_, slot.mappedInput);
                    slot.mappedInput = nullptr;
                }
                resetOutputSlotFrameState(slot);
                slot.scaledViewGeneration = 0;
                releaseOutputSlotBuffers(slot);
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
        resetPreparedFrameBridge();
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
        preferNv12Input_ = false;
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
        preparedSubmissionDepth_ = minimumPreparedSubmissionDepth_;
        syncOutputDelay_ = kDefaultSyncOutputDelay;
    }

    void destroy() {
        destroyEncoderResources();
        device_.Reset();
        captureContext_.Reset();
        captureDevice_.Reset();
        separateEncoderDevice_ = false;
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        funcs_ = {};
    }


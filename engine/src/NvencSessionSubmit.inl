// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
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
        traceSteadyVram();

        auto* slot = freeOutputSlot(frame, packets, status);
        if (!slot) {
            if (externalDropCount_) externalDropCount_->fetch_add(1, std::memory_order_relaxed);
            if (externalSurfaceDropCount_) externalSurfaceDropCount_->fetch_add(1, std::memory_order_relaxed);
            status = "NVENC output pool remained busy after a bounded wait; dropping this frame.";
            return true;
        }
        if (!ensureOutputSlotBuffers(*slot, status)) {
            releaseOutputSlotReservation(*slot);
            return false;
        }
        releaseIdleOutputSlot();

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
            // Invalidate a cached NV12 canonical image as well as the slot.
            // Otherwise isolated-device retry would register NV12 again.
            if (separateEncoderDevice_) resetPreparedFrameBridge();
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
        slot->sourceHadDesktopPresent = frame.sourceHadDesktopPresent;
        slot->sourceHadPointerUpdate = frame.sourceHadPointerUpdate;
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
        if (frameIndex_ == 0 || nextKeyframePts100ns_ == 0) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            nextKeyframePts100ns_ = 1;
        }

        const int64_t encodeCallStarted100ns = monotonicNow100ns();
        nvStatus = funcs_.nvEncEncodePicture(encoder_, &pic);
        timings.encodeCall100ns = monotonicNow100ns() - encodeCallStarted100ns;
        const bool needsMoreInput = nvStatus == NV_ENC_ERR_NEED_MORE_INPUT;
        if (nvStatus != NV_ENC_SUCCESS && !needsMoreInput) {
            funcs_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            releaseOutputSlotReservation(*slot);
            status = "NvEncEncodePicture failed: " + statusDetails(nvStatus);
            return false;
        }

        slot->mappedInput = mapped.mappedResource;
        slot->mappedRegisteredResource = prepared.registeredResource;
        // Keep several submitted outputs queued before locking the oldest one.
        // This is the same bounded synchronous pipeline used by OBS: input
        // preparation, NVENC, and bitstream retrieval can overlap without
        // enabling DirectX asynchronous event mode.
        {
            std::lock_guard lock(outputMutex_);
            inFlightOrder_.push_back(slotIndex(slot));
            const int current = inFlightCount_.fetch_add(1) + 1;
            if (externalInFlightCount_) externalInFlightCount_->store(current);
        }
        if (asyncEnabled_) {
            outputCv_.notify_one();
            ++frameIndex_;
            status = "Direct NVENC async pipeline is queueing H.264 frames.";
            collectCompletedPackets(packets);
            return true;
        }

        ++frameIndex_;
        const int currentInFlight = inFlightCount_.load(std::memory_order_relaxed);
        if (!shouldDrainNvencSyncOutput(
                static_cast<std::size_t>(std::max(0, currentInFlight)),
                syncOutputDelay_)) {
            status = needsMoreInput
                ? "Direct NVENC accepted input and is waiting for more frames."
                : "Direct NVENC buffered sync pipeline is priming output.";
            return true;
        }
        return drainOldestSyncOutput(packets, status);
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


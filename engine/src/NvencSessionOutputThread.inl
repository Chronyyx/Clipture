// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
    void startOutputThread() {
        if (outputThreadRunning_.exchange(true)) return;
        outputFatal_.store(false);
        outputThread_ = std::thread([this] {
            nameCurrentThread(L"nvenc-output");
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


// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
void EncoderWorker::run() {
    nameCurrentThread(L"encoder-scheduler");
    MmcssThreadRegistration mmcss(L"Games", AVRT_PRIORITY_HIGH);
    EncoderCadence cadence(std::clamp(targetFps_.load(std::memory_order_relaxed), 24, 240), monotonicNow100ns());
    DeadlineWait wait(capturePipelinePolicy().encoderSpinTail100ns);
    std::optional<CapturedFrame> heldFrame;
    uint64_t previousSequence = 0;
    uint64_t previousEpoch = 0;

    while (running_.load(std::memory_order_relaxed)) {
        const int fps = std::clamp(targetFps_.load(std::memory_order_relaxed), 24, 240);
        if (fps != cadence.fps()) {
            cadence = EncoderCadence(fps, monotonicNow100ns());
        }
        frames_.announceNextDeadline(cadence.deadline100ns());
        frames_.announceSelectionTicks(mediaTimeFromSystemRelative100ns(cadence.deadline100ns()), cadence.fps());
        wait.until(cadence.deadline100ns(), [this] { return running_.load(std::memory_order_relaxed); });
        if (!running_.load(std::memory_order_relaxed)) break;
        injectTestEncoderStall();
        // Queue capture's held frame while this tick is still announced.
        frames_.beforeSelect();
        const auto wokeAt100ns = monotonicNow100ns();
        const auto tick = cadence.advance(wokeAt100ns);
        recentSchedulerWakeLateness_.record(wokeAt100ns, tick.lateness100ns);
        frames_.announceNextDeadline(cadence.deadline100ns());
        frames_.announceSelectionTicks(mediaTimeFromSystemRelative100ns(cadence.deadline100ns()), cadence.fps());
        schedulerDroppedFrames_.fetch_add(static_cast<int>(std::min<uint64_t>(tick.skipped, INT_MAX)));

        if constexpr (kEnableFrameDuplication) {
            // Late ticks take what was on screen at their own time (CatchUpSelection.hpp).
            const bool moreTicksDue = cadence.deadline100ns() <= wokeAt100ns;
            auto newest = capturePipelinePolicy().timedCatchUp && selectByDeadline(moreTicksDue, tick.lateness100ns)
                ? frames_.consumeLatestAtOrBefore(mediaTimeFromSystemRelative100ns(tick.deadline100ns))
                : frames_.consumeAllAndGetLatest(tick.deadline100ns, fps);
            if (newest) heldFrame = std::move(newest);
            if (!heldFrame) continue;

            int outputWidth = targetWidth_.load(std::memory_order_relaxed);
            int outputHeight = targetHeight_.load(std::memory_order_relaxed);
            if (outputWidth <= 0 || outputHeight <= 0) {
                int lockedWidth = autoOutputWidth_.load(std::memory_order_relaxed);
                int lockedHeight = autoOutputHeight_.load(std::memory_order_relaxed);
                if (lockedWidth <= 0 || lockedHeight <= 0) {
                    lockedWidth = heldFrame->width;
                    lockedHeight = heldFrame->height;
                    autoOutputWidth_.store(lockedWidth, std::memory_order_relaxed);
                    autoOutputHeight_.store(lockedHeight, std::memory_order_relaxed);
                }
                outputWidth = lockedWidth;
                outputHeight = lockedHeight;
            }

            sourceWidth_.store(heldFrame->width, std::memory_order_relaxed);
            sourceHeight_.store(heldFrame->height, std::memory_order_relaxed);
            outputWidth_.store(outputWidth, std::memory_order_relaxed);
            outputHeight_.store(outputHeight, std::memory_order_relaxed);
            scalingActive_.store(
                heldFrame->width != outputWidth || heldFrame->height != outputHeight,
                std::memory_order_relaxed);

            CapturedFrame frameToEncode = *heldFrame;
            frameToEncode.pts100ns = mediaTimeFromSystemRelative100ns(tick.deadline100ns);
            if (heldFrame->sequence != 0 && heldFrame->sequence == previousSequence &&
                heldFrame->captureEpoch == previousEpoch) ++schedulerRepeatedFrames_;
            previousSequence = heldFrame->sequence;
            previousEpoch = heldFrame->captureEpoch;

            queueFrame(EncodeJob {
                std::move(frameToEncode),
                fps,
                std::clamp(targetBitrateMbps_.load(std::memory_order_relaxed), 4, 120),
                outputWidth,
                outputHeight,
                std::max(maxEncodeWidth_.load(std::memory_order_relaxed), outputWidth),
                std::max(maxEncodeHeight_.load(std::memory_order_relaxed), outputHeight),
                std::clamp(nvencPreset_.load(std::memory_order_relaxed), 1, 5),
                configVersion_.load(std::memory_order_relaxed),
                freshFrameVersion_.load(std::memory_order_relaxed),
            });
        } else {
            auto newest = frames_.consumeAllAndGetLatest();
            if (!newest) continue;

            auto& currentFrame = *newest;

            int outputWidth = targetWidth_.load(std::memory_order_relaxed);
            int outputHeight = targetHeight_.load(std::memory_order_relaxed);
            if (outputWidth <= 0 || outputHeight <= 0) {
                int lockedWidth = autoOutputWidth_.load(std::memory_order_relaxed);
                int lockedHeight = autoOutputHeight_.load(std::memory_order_relaxed);
                if (lockedWidth <= 0 || lockedHeight <= 0) {
                    lockedWidth = currentFrame.width;
                    lockedHeight = currentFrame.height;
                    autoOutputWidth_.store(lockedWidth, std::memory_order_relaxed);
                    autoOutputHeight_.store(lockedHeight, std::memory_order_relaxed);
                }
                outputWidth = lockedWidth;
                outputHeight = lockedHeight;
            }

            sourceWidth_.store(currentFrame.width, std::memory_order_relaxed);
            sourceHeight_.store(currentFrame.height, std::memory_order_relaxed);
            outputWidth_.store(outputWidth, std::memory_order_relaxed);
            outputHeight_.store(outputHeight, std::memory_order_relaxed);
            scalingActive_.store(
                currentFrame.width != outputWidth || currentFrame.height != outputHeight,
                std::memory_order_relaxed);

            queueFrame(EncodeJob {
                std::move(currentFrame),
                fps,
                std::clamp(targetBitrateMbps_.load(std::memory_order_relaxed), 4, 120),
                outputWidth,
                outputHeight,
                std::max(maxEncodeWidth_.load(std::memory_order_relaxed), outputWidth),
                std::max(maxEncodeHeight_.load(std::memory_order_relaxed), outputHeight),
                std::clamp(nvencPreset_.load(std::memory_order_relaxed), 1, 5),
                configVersion_.load(std::memory_order_relaxed),
                freshFrameVersion_.load(std::memory_order_relaxed),
            });
        }
    }
    frames_.announceNextDeadline(0);
    frames_.announceSelectionTicks(0, 0);
}

void EncoderWorker::encodeLoop() {
    nameCurrentThread(L"encoder-submit");
    MmcssThreadRegistration mmcss(L"Capture", AVRT_PRIORITY_HIGH);
    auto session = std::make_unique<NvencSession>(
        packets_,
        &nvencInFlightFrames_,
        &encoderBackpressureDrops_,
        &nvencSurfaceDrops_,
        &nvencInputDrops_,
        &nvencOutputSlots_,
        &recentOutputEventWaitLatency_,
        &recentOutputLockLatency_,
        &recentOutputCopyLatency_,
        &recentOutputUnmapLatency_);
    int activeConfigVersion = -1;
    int activeFreshFrameVersion = -1;
    int appliedConfigDiscardVersion = 0;
    int appliedDiscardVersion = 0;
    uint64_t activeCaptureEpoch = 0;
    int64_t previousInputPts100ns = 0;
    uint64_t previousInputCaptureEpoch = 0;
    int64_t previousOutputPts100ns = 0;
    uint32_t previousOutputEncoderEpoch = 0;
    uint64_t previousOutputSourceSequence = 0;
    int64_t lastFreshMotionTimestamp100ns = 0;
    int consecutiveFreshPresents = 0;
    int pendingMotionRepeats = 0;
    auto pushPackets = [
        this,
        &previousOutputPts100ns,
        &previousOutputEncoderEpoch,
        &previousOutputSourceSequence,
        &lastFreshMotionTimestamp100ns,
        &consecutiveFreshPresents,
        &pendingMotionRepeats](std::vector<EncodedPacket>& packets) {
        for (auto& packet : packets) {
            const int64_t recordedAt100ns = monotonicNow100ns();
            if (packet.encoderEpoch != previousOutputEncoderEpoch ||
                packet.pts100ns <= previousOutputPts100ns) {
                previousOutputPts100ns = 0;
                previousOutputSourceSequence = 0;
                lastFreshMotionTimestamp100ns = 0;
                consecutiveFreshPresents = 0;
                pendingMotionRepeats = 0;
            }
            if (previousOutputPts100ns > 0) {
                recentOutputPtsInterval_.record(
                    recordedAt100ns,
                    packet.pts100ns - previousOutputPts100ns);
            }
            previousOutputPts100ns = packet.pts100ns;
            previousOutputEncoderEpoch = packet.encoderEpoch;

            if (packet.sourceFrameSequence == 0) {
                ++unknownSourceOutputFrames_;
                recentUnknownSourceOutputEvents_.record(recordedAt100ns, 0);
                previousOutputSourceSequence = 0;
                consecutiveFreshPresents = 0;
                pendingMotionRepeats = 0;
            } else if (packet.sourceFrameSequence == previousOutputSourceSequence) {
                ++repeatedSourceOutputFrames_;
                recentRepeatedSourceOutputEvents_.record(recordedAt100ns, 0);
                const bool withinActiveMotionStream = consecutiveFreshPresents >= 3 &&
                    lastFreshMotionTimestamp100ns > 0 &&
                    (recordedAt100ns - lastFreshMotionTimestamp100ns) <= 180'000LL;
                if (withinActiveMotionStream) {
                    ++pendingMotionRepeats;
                } else {
                    pendingMotionRepeats = 0;
                }
                consecutiveFreshPresents = 0;
            } else {
                ++distinctSourceOutputFrames_;
                recentDistinctSourceOutputEvents_.record(recordedAt100ns, 0);
                if (packet.sourceHadDesktopPresent) {
                    const bool wasRecent = lastFreshMotionTimestamp100ns > 0 &&
                        (recordedAt100ns - lastFreshMotionTimestamp100ns) <= 180'000LL;
                    if (wasRecent) {
                        ++consecutiveFreshPresents;
                    } else {
                        consecutiveFreshPresents = 1;
                        pendingMotionRepeats = 0;
                    }

                    if (pendingMotionRepeats > 0 && wasRecent) {
                        motionFramesTotal_ += pendingMotionRepeats;
                        motionFramesRepeated_ += pendingMotionRepeats;
                        for (int i = 0; i < pendingMotionRepeats; ++i) {
                            recentMotionOutputEvents_.record(recordedAt100ns, 0);
                            recentMotionRepeatEvents_.record(recordedAt100ns, 0);
                        }
                    }
                    pendingMotionRepeats = 0;

                    if (consecutiveFreshPresents == 3) {
                        motionFramesTotal_ += 3;
                        recentMotionOutputEvents_.record(recordedAt100ns, 0);
                        recentMotionOutputEvents_.record(recordedAt100ns, 0);
                        recentMotionOutputEvents_.record(recordedAt100ns, 0);
                    } else if (consecutiveFreshPresents > 3) {
                        ++motionFramesTotal_;
                        recentMotionOutputEvents_.record(recordedAt100ns, 0);
                    }
                    lastFreshMotionTimestamp100ns = recordedAt100ns;
                } else {
                    consecutiveFreshPresents = 0;
                    pendingMotionRepeats = 0;
                }
                previousOutputSourceSequence = packet.sourceFrameSequence;
            }

            const int64_t packetEnd100ns = packet.pts100ns + std::max<int64_t>(0, packet.duration100ns);
            if (replayStore_) {
                replayStore_->push(packet);
                EncodedPacket headerOnly = packet;
                headerOnly.payload.reset();
                packets_.push(std::move(headerOnly));
            } else {
                packets_.push(std::move(packet));
            }
            ++framesEncoded_;
            encodedFrontier_.advance(packetEnd100ns);
        }
        packets.clear();
    };

    uint64_t deviceChecks = 0;
    while (true) {
        EncodeJob job;
        {
            std::unique_lock lock(submitMutex_);
            submitCv_.wait(lock, [this] { return !running_.load() || !pendingJobs_.empty(); });
            if (pendingJobs_.empty() && !running_) break;
            if (pendingJobs_.empty()) continue;
            job = std::move(pendingJobs_.front());
            pendingJobs_.pop_front();
            pendingOutputFrames_ -= std::min<std::size_t>(pendingOutputFrames_, 1);
            pendingFreshFrames_ -= std::min<std::size_t>(pendingFreshFrames_, 1);
        }
        submitCv_.notify_all();

        if (job.configVersion != configVersion_.load() ||
            job.freshFrameVersion != freshFrameVersion_.load()) {
            continue;
        }

        CapturedFrame frameForTick = std::move(job.frame);
        const int64_t submitStarted100ns = monotonicNow100ns();
        if (frameForTick.captureEpoch != previousInputCaptureEpoch ||
            frameForTick.pts100ns <= previousInputPts100ns) {
            previousInputPts100ns = 0;
        }
        if (previousInputPts100ns > 0) {
            recentInputPtsInterval_.record(
                submitStarted100ns,
                frameForTick.pts100ns - previousInputPts100ns);
        }
        previousInputPts100ns = frameForTick.pts100ns;
        previousInputCaptureEpoch = frameForTick.captureEpoch;
        if (job.enqueuedAt100ns > 0 && submitStarted100ns >= job.enqueuedAt100ns) {
            recentQueueResidenceLatency_.record(
                submitStarted100ns,
                submitStarted100ns - job.enqueuedAt100ns);
        }
        const bool freshEpochChanged = activeFreshFrameVersion >= 0 &&
            job.freshFrameVersion != activeFreshFrameVersion;
        const bool sessionChanged =
            job.configVersion != activeConfigVersion ||
            job.freshFrameVersion != activeFreshFrameVersion ||
            frameForTick.captureEpoch != activeCaptureEpoch;
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
        &nvencOutputSlots_,
                &recentOutputEventWaitLatency_,
                &recentOutputLockLatency_,
                &recentOutputCopyLatency_,
                &recentOutputUnmapLatency_);
            nvencInFlightFrames_ = 0;
            nvencPreparedFrames_ = 0;
            activeConfigVersion = job.configVersion;
            activeFreshFrameVersion = job.freshFrameVersion;
            activeCaptureEpoch = frameForTick.captureEpoch;
        }

        std::string sessionStatus;
        if (frameForTick.preparation && !frameForTick.preparation->prepare(sessionStatus)) {
            ++encoderBackpressureDrops_;
            setStatus("Deferred capture preparation failed: " + sessionStatus);
            continue;
        }
        bool encoderInitialized = false;
        bool encoded = false;
        const bool settingUp = nvencRuntimeLoaded_ && !session->isInitialized();
        if (settingUp && setupObserver_) setupObserver_();
        if (++deviceChecks % 60 == 0 && deviceRemovedHr_.load(std::memory_order_relaxed) == 0) {
            const HRESULT removed = session->deviceRemovedReason();
            if (FAILED(removed)) deviceRemovedHr_.store(static_cast<long>(removed), std::memory_order_release);
        }
        if (nvencRuntimeLoaded_ && session->initialize(
                frameForTick.texture.Get(),
                frameForTick.width,
                frameForTick.height,
                job.targetWidth,
                job.targetHeight,
                job.maxEncodeWidth,
                job.maxEncodeHeight,
                job.fps,
                job.bitrateMbps,
                job.nvencPreset,
                sessionStatus)) {
            encoderInitialized = true;
            if (settingUp) ++encoderSetups_;
            outputWidth_ = session->outputWidth();
            outputHeight_ = session->outputHeight();
            scalingActive_ = frameForTick.width != session->outputWidth() ||
                frameForTick.height != session->outputHeight();

            std::vector<EncodedPacket> encodedPackets;
            NvencFrameTimings timings;
            encoded = session->encode(std::move(frameForTick), encodedPackets, sessionStatus, timings);
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
        &nvencOutputSlots_,
                &recentOutputEventWaitLatency_,
                &recentOutputLockLatency_,
                &recentOutputCopyLatency_,
                &recentOutputUnmapLatency_);
            nvencInFlightFrames_ = 0;
            nvencPreparedFrames_ = 0;
            activeConfigVersion = -1;
            activeFreshFrameVersion = -1;
            activeCaptureEpoch = 0;
            continue;
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


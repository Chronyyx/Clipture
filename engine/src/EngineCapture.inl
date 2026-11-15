// Part of Engine.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
void Engine::arm() {
    diagnostics_.bufferDurationSeconds = settings_.clipLengthSeconds;

    if (diagnostics_.activeEncoder != EncoderName::Nvenc) {
        diagnostics_.engineRunning = false;
        diagnostics_.d3d11Ready = false;
        diagnostics_.captureReady = false;
        diagnostics_.audioReady = false;
        diagnostics_.muxReady = false;
        diagnostics_.degraded = true;
        return;
    }

    const auto runtime = initializeNativeRuntime(true);
    diagnostics_.gpu = runtime.adapterName.empty() ? diagnostics_.gpu : runtime.adapterName;
    diagnostics_.resolution = runtime.resolution;
    diagnostics_.engineRunning = runtime.d3d11Ready;
    diagnostics_.d3d11Ready = runtime.d3d11Ready;

    if (videoReplayStore_) videoReplayStore_->start();
    if (aacReplayStore_) aacReplayStore_->start();
    if (pcmRecoveryStore_) pcmRecoveryStore_->start();
    encoderWorker_->start();
    {
        const auto [maxEncodeWidth, maxEncodeHeight] = encoderMaxDimensions(settings_);
        encoderWorker_->configure(
            settings_.fps,
            settings_.bitrateMbps,
            settings_.targetWidth,
            settings_.targetHeight,
            maxEncodeWidth,
            maxEncodeHeight,
            settings_.nvencPreset);
    }
    const bool captureStarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
    diagnostics_.captureReady = captureStarted;
    if (captureStarted) {
        diagnostics_.resolution = captureSession_->resolution();
        diagnostics_.display = captureSession_->displayName();
        diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
    }
    diagnostics_.audioReady = false;
    diagnostics_.muxReady = false;
    diagnostics_.degraded = !runtime.d3d11Ready || !captureStarted;
    diagnostics_.status = captureStarted ? captureSession_->status() : captureSession_->status();
    refreshAudioRouting();
    audioReplayCoordinator_->start();
    audioCaptureWorker_->setCoreSources(coreSystemAudioNeeded(settings_), coreMicrophoneNeeded(settings_));
    audioCaptureWorker_->start(false);
    diagnostics_.audioReady = audioCaptureWorker_->running();
    refreshPacketCounts();
}

void Engine::refreshPacketCounts() {
    maybeResetAutoVideoResolution();
    if (armed_ && captureSession_ && !captureSession_->running()) {
        const int64_t currentTime100ns = now100ns();
        if (currentTime100ns >= nextCaptureRestart100ns_) {
            nextCaptureRestart100ns_ = currentTime100ns + 2LL * 10'000'000LL;
            if (encoderWorker_) encoderWorker_->requireFreshFrame();
            const bool restarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
            diagnostics_.captureReady = restarted;
            if (restarted) {
                nextCaptureRestart100ns_ = 0;
                diagnostics_.resolution = captureSession_->resolution();
                diagnostics_.display = captureSession_->displayName();
                diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
            }
        }
    }
    diagnostics_.bufferedVideoPackets = static_cast<int>(
        videoReplayStore_ ? videoReplayStore_->size() : videoPackets_.size());
    diagnostics_.bufferedAudioPackets = static_cast<int>(audioPackets_.size());
    const auto videoArchive = videoReplayStore_ ? videoReplayStore_->stats() : ReplaySegmentStoreStats {};
    const auto audioArchive = aacReplayStore_ ? aacReplayStore_->stats() : ReplaySegmentStoreStats {};
    const auto pcmRecoveryArchive = pcmRecoveryStore_ ? pcmRecoveryStore_->stats() : ReplaySegmentStoreStats {};
    const auto audioReplay = audioReplayCoordinator_ ? audioReplayCoordinator_->stats() : AudioReplayStats {};
    diagnostics_.videoReplayArchiveHealthy = videoArchive.healthy;
    diagnostics_.audioReplayArchiveHealthy = audioArchive.healthy && pcmRecoveryArchive.healthy;
    diagnostics_.replayArchiveDiskBytes =
        videoArchive.diskBytes + audioArchive.diskBytes + pcmRecoveryArchive.diskBytes + inPlaceArchive_->diskBytes();
    diagnostics_.replayArchiveRamFallbackBytes =
        videoArchive.ramFallbackBytes + audioArchive.ramFallbackBytes + pcmRecoveryArchive.ramFallbackBytes;
    diagnostics_.replayArchiveResidentBytes =
        videoArchive.residentPayloadBytes + audioArchive.residentPayloadBytes +
        pcmRecoveryArchive.residentPayloadBytes;
    diagnostics_.replayArchiveResidentBudgetBytes =
        videoArchive.residentPayloadBudgetBytes + audioArchive.residentPayloadBudgetBytes +
        (audioReplay.pcmRecoveryActive ? pcmRecoveryArchive.residentPayloadBudgetBytes : 0);
    diagnostics_.replayArchiveReadCacheBytes =
        videoArchive.readCacheBytes + audioArchive.readCacheBytes + pcmRecoveryArchive.readCacheBytes;
    diagnostics_.replayArchiveResidentPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.residentPackets + audioArchive.residentPackets + pcmRecoveryArchive.residentPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveDiskBackedPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.diskBackedPackets + audioArchive.diskBackedPackets +
            pcmRecoveryArchive.diskBackedPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveQueuedBytes =
        videoArchive.queuedBytes + audioArchive.queuedBytes + pcmRecoveryArchive.queuedBytes;
    diagnostics_.replayArchivePersistedPackets =
        videoArchive.persistedPackets + audioArchive.persistedPackets + pcmRecoveryArchive.persistedPackets;
    diagnostics_.replayArchiveSpillCandidateInspections =
        videoArchive.spillCandidateInspections + audioArchive.spillCandidateInspections +
        pcmRecoveryArchive.spillCandidateInspections;
    diagnostics_.replayArchiveWriteFailures =
        videoArchive.writeFailures + audioArchive.writeFailures + pcmRecoveryArchive.writeFailures;
    diagnostics_.replayArchiveQueuedPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.queuedPackets + audioArchive.queuedPackets + pcmRecoveryArchive.queuedPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveSegments = static_cast<int>(std::min<std::size_t>(
        videoArchive.activeSegments + audioArchive.activeSegments + pcmRecoveryArchive.activeSegments,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveMaximumWriteBytes = static_cast<int>(std::min<std::size_t>(
        std::max({
            videoArchive.maximumWriteBytes,
            audioArchive.maximumWriteBytes,
            pcmRecoveryArchive.maximumWriteBytes
        }),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.pcmRecoveryActive = audioReplay.pcmRecoveryActive;
    diagnostics_.queuedFrames = static_cast<int>(frameQueue_.size());
    diagnostics_.capturedFrames = captureSession_ ? captureSession_->capturedFrames() : 0;
    diagnostics_.encoderAcceptedFrames = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
    diagnostics_.encoderOutputPackets = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
    if (encoderWorker_) {
        const auto encoderStatus = encoderWorker_->status();
        if (!encoderStatus.empty()) {
            const auto captureStatus = captureSession_ ? captureSession_->status() : std::string("Capture status unavailable.");
            diagnostics_.status = "Capture: " + captureStatus + " Encoder: " + encoderStatus;
        }
        const int sourceWidth = encoderWorker_->sourceWidth();
        const int sourceHeight = encoderWorker_->sourceHeight();
        const int outputWidth = encoderWorker_->outputWidth();
        const int outputHeight = encoderWorker_->outputHeight();
        diagnostics_.videoSourceResolution = sourceWidth > 0 && sourceHeight > 0
            ? formatResolution(sourceWidth, sourceHeight)
            : diagnostics_.resolution;
        diagnostics_.videoOutputResolution = outputWidth > 0 && outputHeight > 0
            ? formatResolution(outputWidth, outputHeight)
            : "Waiting for first frame";
        diagnostics_.videoScaling = encoderWorker_->scalingActive() ? "GPU fit" : "None";
        if (pendingAutoVideoResolutionReset100ns_ > 0) {
            diagnostics_.videoScaling += " / relock pending";
        }
        diagnostics_.clipTargetResolution = settings_.targetWidth > 0 && settings_.targetHeight > 0
            ? formatResolution(settings_.targetWidth, settings_.targetHeight)
            : "Clip-aware system";
    }
    diagnostics_.audioCapturedPackets = audioCaptureWorker_ ? audioCaptureWorker_->packetsCaptured() : 0;
    if (audioCaptureWorker_) {
        diagnostics_.microphoneDevice = audioCaptureWorker_->microphoneStatus();
    }
    const auto captureStats = captureSession_
        ? captureSession_->runtimeStats()
        : CaptureRuntimeStats {};
    diagnostics_.captureApi = captureStats.activeBackend;
    diagnostics_.requestedCaptureBackend = captureStats.requestedBackend;
    diagnostics_.activeCaptureBackend = captureStats.activeBackend;
    diagnostics_.captureFallbackReason = captureStats.fallbackReason;
    diagnostics_.captureTargetKind = captureStats.targetKind;
    diagnostics_.captureTargetName = captureStats.targetName;
    diagnostics_.displayRefreshNumerator = captureStats.refreshNumerator;
    diagnostics_.displayRefreshDenominator = captureStats.refreshDenominator;
    diagnostics_.displayRefreshHz = captureStats.refreshHz;
    diagnostics_.captureAcquiredUpdates = captureStats.acquiredUpdates;
    diagnostics_.captureDesktopPresents = captureStats.desktopPresents;
    diagnostics_.capturePointerUpdates = captureStats.pointerUpdates;
    diagnostics_.capturePublishedFrames = captureStats.publishedFrames;
    diagnostics_.captureAccumulatedFrames = captureStats.accumulatedFrames;
    diagnostics_.captureAccumulationEvents = captureStats.accumulationEvents;
    diagnostics_.captureSamplerRejections = captureStats.samplerRejections;
    diagnostics_.captureNonMonotonicTimestamps = captureStats.nonMonotonicTimestamps;
    diagnostics_.captureAcquireTimeouts = captureStats.acquireTimeouts;
    diagnostics_.captureAcquireImmediateMisses = captureStats.acquireImmediateMisses;
    diagnostics_.captureAcquireGraceHits = captureStats.acquireGraceHits;
    diagnostics_.captureAcquireGraceTimeouts = captureStats.acquireGraceTimeouts;
    diagnostics_.captureAccessLosses = captureStats.accessLosses;
    diagnostics_.captureRecreationAttempts = captureStats.recreationAttempts;
    diagnostics_.captureRecreationSuccesses = captureStats.recreationSuccesses;
    diagnostics_.captureFallbacks = captureStats.fallbackCount;
    diagnostics_.captureClockMode = captureStats.clockMode;
    diagnostics_.captureClockTickRequests = captureStats.clockTickRequests;
    diagnostics_.captureClockTickWakeups = captureStats.clockTickWakeups;
    diagnostics_.captureClockTickCoalesced = captureStats.clockTickCoalesced;
    diagnostics_.captureClockTickCompletions = captureStats.clockTickCompletions;
    diagnostics_.captureClockTickCompletionWaits = captureStats.clockTickCompletionWaits;
    diagnostics_.captureClockTickCompletionTimeouts = captureStats.clockTickCompletionTimeouts;
    diagnostics_.presentLatchWaits = encoderWorker_ ? encoderWorker_->presentLatchWaits() : 0;
    diagnostics_.presentLatchHits = encoderWorker_ ? encoderWorker_->presentLatchHits() : 0;
    diagnostics_.presentLatchTimeouts = encoderWorker_ ? encoderWorker_->presentLatchTimeouts() : 0;
    diagnostics_.catchUpEvents = encoderWorker_ ? encoderWorker_->catchUpEvents() : 0;
    diagnostics_.historicalFramesRecovered = encoderWorker_
        ? encoderWorker_->historicalFramesRecovered()
        : 0;
    diagnostics_.catchUpRepeatedTicks = encoderWorker_ ? encoderWorker_->catchUpRepeatedTicks() : 0;
    diagnostics_.desktopPresentFps = captureStats.desktopPresentFps;
    diagnostics_.publishedFreshFps = captureStats.publishedFreshFps;
    diagnostics_.recentPublishedFreshFps = captureStats.recentPublishedFreshFps;
    diagnostics_.recentCaptureAcquireP95_100ns = captureStats.acquireWaitLatency.p95_100ns;
    diagnostics_.recentCaptureSourceIntervalP50_100ns = captureStats.sourceUpdateInterval.p50_100ns;
    diagnostics_.recentCaptureSourceIntervalP95_100ns = captureStats.sourceUpdateInterval.p95_100ns;
    diagnostics_.recentCaptureSourceIntervalMaximum100ns = captureStats.sourceUpdateInterval.maximum100ns;
    diagnostics_.recentPublishedPtsIntervalP50_100ns = captureStats.publishedPtsInterval.p50_100ns;
    diagnostics_.recentPublishedPtsIntervalP95_100ns = captureStats.publishedPtsInterval.p95_100ns;
    diagnostics_.recentPublishedPtsIntervalMaximum100ns = captureStats.publishedPtsInterval.maximum100ns;
    diagnostics_.recentPublishedWallIntervalP50_100ns = captureStats.publishedWallInterval.p50_100ns;
    diagnostics_.recentPublishedWallIntervalP95_100ns = captureStats.publishedWallInterval.p95_100ns;
    diagnostics_.recentPublishedWallIntervalMaximum100ns = captureStats.publishedWallInterval.maximum100ns;
    diagnostics_.recentCapturePreparationP50_100ns = captureStats.framePreparationLatency.p50_100ns;
    diagnostics_.recentCapturePreparationP95_100ns = captureStats.framePreparationLatency.p95_100ns;
    diagnostics_.recentCaptureCursorP95_100ns = captureStats.cursorCompositeLatency.p95_100ns;
    diagnostics_.recentCaptureProcessingP50_100ns = captureStats.frameProcessingLatency.p50_100ns;
    diagnostics_.recentCaptureProcessingP95_100ns = captureStats.frameProcessingLatency.p95_100ns;

    const auto queueStats = frameQueue_.stats();
    const auto boundedCounter = [](uint64_t value) {
        return static_cast<int>(std::min<uint64_t>(
            value,
            static_cast<uint64_t>(std::numeric_limits<int>::max())));
    };
    diagnostics_.captureOverflowDrops = boundedCounter(queueStats.overflowDrops);
    diagnostics_.captureCoalescedDrops = boundedCounter(queueStats.coalescedDrops);
    diagnostics_.frameQueueMaxDepth = queueStats.maximumDepth;
    diagnostics_.sourceFramesSuperseded = captureSession_
        ? boundedCounter(captureSession_->sourceFramesSuperseded())
        : 0;
    diagnostics_.captureSlotDrops = captureSession_ ? boundedCounter(captureSession_->ownedSlotDrops()) : 0;
    if (captureSession_) {
        const auto& slots = captureSession_->captureTextureSlots();
        diagnostics_.captureTextureSlots = slots.allocated.load(std::memory_order_relaxed);
        diagnostics_.captureTextureSlotsPeak = slots.peak.load(std::memory_order_relaxed);
        diagnostics_.captureTextureSlotIdleReleases = slots.idleReleases.load(std::memory_order_relaxed);
    }
    diagnostics_.captureCallbackErrors = captureSession_ ? boundedCounter(captureSession_->callbackErrors()) : 0;
    diagnostics_.maximumCaptureGap100ns = captureSession_ ? captureSession_->maximumFrameInterval100ns() : 0;
    diagnostics_.captureEpoch = captureSession_ ? captureSession_->captureEpoch() : 0;
    diagnostics_.schedulerDroppedFrames = encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0;
    diagnostics_.schedulerRepeatedFrames = encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0;
    diagnostics_.stillFrameDuplicationEnabled = encoderWorker_
        ? encoderWorker_->stillFrameDuplicationEnabled()
        : false;
    diagnostics_.encoderDistinctSourceFrames = encoderWorker_
        ? encoderWorker_->distinctSourceOutputFrames()
        : 0;
    diagnostics_.encoderRepeatedSourceFrames = encoderWorker_
        ? encoderWorker_->repeatedSourceOutputFrames()
        : 0;
    diagnostics_.encoderUnknownSourceFrames = encoderWorker_
        ? encoderWorker_->unknownSourceOutputFrames()
        : 0;
    diagnostics_.motionFramesTotal = encoderWorker_
        ? encoderWorker_->motionFramesTotal()
        : 0;
    diagnostics_.motionFramesRepeated = encoderWorker_
        ? encoderWorker_->motionFramesRepeated()
        : 0;
    diagnostics_.encodedRepeatRatio = diagnostics_.encoderOutputPackets > 0
        ? static_cast<double>(diagnostics_.encoderRepeatedSourceFrames) /
            static_cast<double>(diagnostics_.encoderOutputPackets)
        : 0.0;
    diagnostics_.encodedMotionRepeatRatioPercent = diagnostics_.motionFramesTotal > 0
        ? (static_cast<double>(diagnostics_.motionFramesRepeated) * 100.0) /
            static_cast<double>(diagnostics_.motionFramesTotal)
        : 0.0;
    diagnostics_.encoderQueueDrops = encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0;
    diagnostics_.encoderRepeatCoalesced = encoderWorker_ ? encoderWorker_->encoderRepeatCoalesced() : 0;
    diagnostics_.encoderAdmissionRejections = encoderWorker_
        ? encoderWorker_->encoderAdmissionRejections()
        : 0;
    diagnostics_.encoderQueuedFreshFrames = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
    diagnostics_.encoderQueuedRepeatFrames = encoderWorker_ ? encoderWorker_->queuedRepeatEncodeFrames() : 0;
    diagnostics_.nvencSurfaceDrops = encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0;
    diagnostics_.nvencInputDrops = encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0;
    diagnostics_.encoderBackpressureDrops = encoderWorker_ ? encoderWorker_->encoderBackpressureDrops() : 0;
    diagnostics_.nvencInFlightFrames = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
    if (encoderWorker_) {
        const auto& slots = encoderWorker_->nvencOutputSlots();
        diagnostics_.nvencOutputSlots = slots.allocated.load(std::memory_order_relaxed);
        diagnostics_.nvencOutputSlotsPeak = slots.peak.load(std::memory_order_relaxed);
        diagnostics_.nvencOutputSlotIdleReleases = slots.idleReleases.load(std::memory_order_relaxed);
    }
    diagnostics_.maximumSubmitLatency100ns = encoderWorker_ ? encoderWorker_->maximumSubmitLatency100ns() : 0;
    diagnostics_.averageScaleLatency100ns = encoderWorker_ ? encoderWorker_->averageScaleLatency100ns() : 0;
    diagnostics_.maximumScaleLatency100ns = encoderWorker_ ? encoderWorker_->maximumScaleLatency100ns() : 0;
    diagnostics_.averageInputMapLatency100ns = encoderWorker_ ? encoderWorker_->averageInputMapLatency100ns() : 0;
    diagnostics_.maximumInputMapLatency100ns = encoderWorker_ ? encoderWorker_->maximumInputMapLatency100ns() : 0;
    diagnostics_.averageNvencCallLatency100ns = encoderWorker_ ? encoderWorker_->averageNvencCallLatency100ns() : 0;
    diagnostics_.maximumNvencCallLatency100ns = encoderWorker_ ? encoderWorker_->maximumNvencCallLatency100ns() : 0;
    diagnostics_.averageOutputDrainLatency100ns = encoderWorker_ ? encoderWorker_->averageOutputDrainLatency100ns() : 0;
    diagnostics_.maximumOutputDrainLatency100ns = encoderWorker_ ? encoderWorker_->maximumOutputDrainLatency100ns() : 0;
    const auto encoderRecent = encoderWorker_
        ? encoderWorker_->recentPerformance()
        : EncoderRecentPerformance {};
    diagnostics_.recentEncoderInputFps = encoderRecent.inputFps;
    diagnostics_.recentEncoderOutputFps = encoderRecent.outputFps;
    diagnostics_.recentEncoderDistinctSourceFps = encoderRecent.distinctSourceOutputFps;
    diagnostics_.recentEncoderRepeatedSourceFrames = encoderRecent.repeatedSourceOutputFrames;
    diagnostics_.recentEncoderUnknownSourceFrames = encoderRecent.unknownSourceOutputFrames;
    diagnostics_.recentMotionFramesTotal = encoderRecent.motionFramesTotal;
    diagnostics_.recentMotionFramesRepeated = encoderRecent.motionFramesRepeated;
    diagnostics_.recentMotionRepeatRatioPercent = encoderRecent.motionFramesTotal > 0
        ? (static_cast<double>(encoderRecent.motionFramesRepeated) * 100.0) /
            static_cast<double>(encoderRecent.motionFramesTotal)
        : 0.0;
    diagnostics_.recentSchedulerWakeLatenessP50_100ns = encoderRecent.schedulerWakeLateness.p50_100ns;
    diagnostics_.recentSchedulerWakeLatenessP95_100ns = encoderRecent.schedulerWakeLateness.p95_100ns;
    diagnostics_.recentSchedulerWakeLatenessMaximum100ns = encoderRecent.schedulerWakeLateness.maximum100ns;
    diagnostics_.recentEncoderQueueResidenceP50_100ns = encoderRecent.queueResidence.p50_100ns;
    diagnostics_.recentEncoderQueueResidenceP95_100ns = encoderRecent.queueResidence.p95_100ns;
    diagnostics_.recentEncoderQueueResidenceMaximum100ns = encoderRecent.queueResidence.maximum100ns;
    diagnostics_.recentEncoderInputIntervalP50_100ns = encoderRecent.inputPtsInterval.p50_100ns;
    diagnostics_.recentEncoderInputIntervalP95_100ns = encoderRecent.inputPtsInterval.p95_100ns;
    diagnostics_.recentEncoderInputIntervalMaximum100ns = encoderRecent.inputPtsInterval.maximum100ns;
    diagnostics_.recentEncoderOutputIntervalP50_100ns = encoderRecent.outputPtsInterval.p50_100ns;
    diagnostics_.recentEncoderOutputIntervalP95_100ns = encoderRecent.outputPtsInterval.p95_100ns;
    diagnostics_.recentEncoderOutputIntervalMaximum100ns = encoderRecent.outputPtsInterval.maximum100ns;
    diagnostics_.recentInputPreparationP50_100ns = encoderRecent.inputPreparation.p50_100ns;
    diagnostics_.recentInputPreparationP95_100ns = encoderRecent.inputPreparation.p95_100ns;
    diagnostics_.recentInputMapP50_100ns = encoderRecent.inputMap.p50_100ns;
    diagnostics_.recentInputMapP95_100ns = encoderRecent.inputMap.p95_100ns;
    diagnostics_.recentNvencCallP50_100ns = encoderRecent.encodeCall.p50_100ns;
    diagnostics_.recentNvencCallP95_100ns = encoderRecent.encodeCall.p95_100ns;
    diagnostics_.recentOutputEventWaitP50_100ns = encoderRecent.outputEventWait.p50_100ns;
    diagnostics_.recentOutputEventWaitP95_100ns = encoderRecent.outputEventWait.p95_100ns;
    diagnostics_.recentOutputLockP95_100ns = encoderRecent.outputLock.p95_100ns;
    diagnostics_.recentOutputCopyP95_100ns = encoderRecent.outputCopy.p95_100ns;
    diagnostics_.recentOutputUnmapP95_100ns = encoderRecent.outputUnmap.p95_100ns;
    diagnostics_.nvencZeroCopyFrames = encoderWorker_ ? encoderWorker_->nvencZeroCopyFrames() : 0;
    diagnostics_.nvencCopyFallbackFrames = encoderWorker_ ? encoderWorker_->nvencCopyFallbackFrames() : 0;
    diagnostics_.nvencConvertedFrames = encoderWorker_ ? encoderWorker_->nvencConvertedFrames() : 0;
    diagnostics_.droppedFrames = boundedCounter(
        queueStats.overflowDrops +
        static_cast<uint64_t>(std::max(0, diagnostics_.captureSlotDrops)) +
        static_cast<uint64_t>(std::max(0, diagnostics_.schedulerDroppedFrames)) +
        static_cast<uint64_t>(std::max(0, diagnostics_.encoderBackpressureDrops)));

    const int64_t frameInterval100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
    const int64_t oldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
    if (diagnostics_.queuedFrames >= 4 ||
        diagnostics_.encoderQueuedFreshFrames >= 4 ||
        oldestFrameAge100ns > frameInterval100ns * 4) {
        diagnostics_.capturePressure = "critical";
    } else if (diagnostics_.queuedFrames >= 2 ||
               diagnostics_.encoderQueuedFreshFrames >= 2 ||
               oldestFrameAge100ns > frameInterval100ns * 2) {
        diagnostics_.capturePressure = "elevated";
    } else {
        diagnostics_.capturePressure = "healthy";
    }
}

void Engine::maybeResetAutoVideoResolution() {
    if (pendingAutoVideoResolutionReset100ns_ <= 0) return;
    pendingAutoVideoResolutionReset100ns_ = 0;
}


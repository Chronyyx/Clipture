// Part of Engine.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
SaveClipResult Engine::saveClip(const SaveClipRequest& request) {
    SaveClipResult result;
    diagnostics_.lastClipCadence = {};
    const auto totalStartedAt = SaveTimingClock::now();
    logEngineSaveTiming("start", totalStartedAt, "durationSeconds=" + std::to_string(request.durationSeconds));
    auto currentVideoDropCount = [this]() {
        return static_cast<int>(std::min<uint64_t>(videoDropTotal(), static_cast<uint64_t>(std::numeric_limits<int>::max())));
    };
    const int saveStartDroppedFrames = currentVideoDropCount();
    const auto saveStartFrameQueueStats = frameQueue_.stats();
    const auto saveStartCaptureStats = captureSession_
        ? captureSession_->runtimeStats()
        : CaptureRuntimeStats {};
    const uint64_t saveStartSourceSuperseded = captureSession_ ? captureSession_->sourceFramesSuperseded() : 0;
    const uint64_t saveStartCaptureSlotDrops = captureSession_ ? captureSession_->ownedSlotDrops() : 0;
    const int saveStartSchedulerSkips = encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0;
    const int saveStartSchedulerRepeats = encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0;
    const int saveStartEncoderQueueDrops = encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0;
    const int saveStartNvencSurfaceDrops = encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0;
    const int saveStartNvencInputDrops = encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0;
    const int saveStartEncoderAccepted = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
    const int saveStartEncoderEncoded = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
    const int saveStartFrameQueueDepth = static_cast<int>(frameQueue_.size());
    const int64_t saveStartOldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
    const int saveStartEncoderQueueDepth = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
    const int saveStartNvencInFlight = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
    const int64_t saveStartCaptureGap100ns = captureSession_ ? captureSession_->lastFrameInterval100ns() : 0;
    const int64_t saveStartCapturePublicationAge100ns = captureSession_
        ? captureSession_->lastPublishedAge100ns()
        : 0;
    auto saveStutterDeltaDetails = [&]() {
        const int currentDroppedFrames = currentVideoDropCount();
        const int currentEncoderAccepted = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
        const int currentEncoderEncoded = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
        const auto currentFrameQueueStats = frameQueue_.stats();
        const auto currentCaptureStats = captureSession_
            ? captureSession_->runtimeStats()
            : CaptureRuntimeStats {};
        return " droppedFramesDelta=" + std::to_string(currentDroppedFrames - saveStartDroppedFrames) +
            " captureBackend=\"" + jsonEscape(currentCaptureStats.activeBackend) + "\"" +
            " acquiredUpdateDelta=" + std::to_string(
                currentCaptureStats.acquiredUpdates - saveStartCaptureStats.acquiredUpdates) +
            " accumulationEventDelta=" + std::to_string(
                currentCaptureStats.accumulationEvents - saveStartCaptureStats.accumulationEvents) +
            " accumulatedFrameDelta=" + std::to_string(
                currentCaptureStats.accumulatedFrames - saveStartCaptureStats.accumulatedFrames) +
            " captureOverflowDelta=" + std::to_string(
                currentFrameQueueStats.overflowDrops - saveStartFrameQueueStats.overflowDrops) +
            " sourceSupersededDelta=" + std::to_string(
                (captureSession_ ? captureSession_->sourceFramesSuperseded() : 0) - saveStartSourceSuperseded) +
            " captureSlotDropDelta=" + std::to_string(
                (captureSession_ ? captureSession_->ownedSlotDrops() : 0) - saveStartCaptureSlotDrops) +
            " schedulerSkipDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0) - saveStartSchedulerSkips) +
            " schedulerRepeatDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0) - saveStartSchedulerRepeats) +
            " encoderQueueDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0) - saveStartEncoderQueueDrops) +
            " nvencSurfaceDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0) - saveStartNvencSurfaceDrops) +
            " nvencInputDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0) - saveStartNvencInputDrops) +
            " encoderAcceptedDelta=" + std::to_string(currentEncoderAccepted - saveStartEncoderAccepted) +
            " encoderEncodedDelta=" + std::to_string(currentEncoderEncoded - saveStartEncoderEncoded);
    };
    auto saveTotalDetails = [&](const std::string& details) {
        return details + saveStutterDeltaDetails();
    };
    MuxWritePacing savePacing {
        [this,
         saveStartDroppedFrames,
         saveStartFrameQueueDepth,
         saveStartOldestFrameAge100ns,
         saveStartEncoderQueueDepth,
         saveStartNvencInFlight,
         saveStartCaptureGap100ns,
         saveStartCapturePublicationAge100ns,
         currentVideoDropCount,
         previousDroppedFrames = saveStartDroppedFrames,
         previousPublishedFrames = saveStartCaptureStats.publishedFrames,
         previousDesktopPresents = saveStartCaptureStats.desktopPresents]() mutable {
            MuxPressureSample sample;
            sample.frameQueueDepth = frameQueue_.size();
            sample.oldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
            sample.encoderQueueDepth = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
            sample.nvencInFlight = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
            // Sampled before every write: two counters, never runtimeStats(),
            // whose latency windows share locks with the capture threads this
            // low-priority thread must not hold up.
            const uint64_t publishedFrames = captureSession_ ? captureSession_->publishedFrames() : 0;
            const uint64_t desktopPresents = captureSession_ ? captureSession_->desktopPresents() : 0;
            sample.captureGap100ns = captureSession_ ? captureSession_->lastFrameInterval100ns() : 0;
            sample.capturePublicationAge100ns = captureSession_
                ? captureSession_->lastPublishedAge100ns()
                : 0;
            const bool captureWasActive =
                publishedFrames > previousPublishedFrames ||
                desktopPresents > previousDesktopPresents;
            previousPublishedFrames = publishedFrames;
            previousDesktopPresents = desktopPresents;
            const int currentDroppedFrames = currentVideoDropCount();
            const bool newFrameDrop = currentDroppedFrames > previousDroppedFrames;
            previousDroppedFrames = currentDroppedFrames;
            sample.droppedFramesDelta = currentDroppedFrames - saveStartDroppedFrames;
            const int64_t frameInterval100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
            const auto pressure = classifyCaptureWritePressure({
                sample.frameQueueDepth,
                sample.oldestFrameAge100ns,
                sample.encoderQueueDepth,
                sample.nvencInFlight,
                sample.captureGap100ns,
                sample.capturePublicationAge100ns,
                frameInterval100ns,
                captureWasActive,
                newFrameDrop,
            });
            if (pressure == AdaptiveWritePressure::Critical) {
                sample.level = MuxPressureLevel::Critical;
            } else if (pressure == AdaptiveWritePressure::Elevated) {
                sample.level = MuxPressureLevel::Elevated;
            }
            return sample;
        }
    };
    savePacing.burstBytes = kSaveWriteBurstBytes;
    savePacing.storageAwareRate = true;
    if (learnedSaveWriteFolder_ == request.saveFolder) {
        savePacing.learnedWriteBytesPerSecond = learnedSaveWriteBytesPerSecond_;
    }
    savePacing.analyzeIo = request.analyzeIo;
    logEngineSaveTiming(
        "stutter_baseline",
        totalStartedAt,
        "droppedFrames=" + std::to_string(saveStartDroppedFrames) +
            " encoderAccepted=" + std::to_string(saveStartEncoderAccepted) +
            " encoderEncoded=" + std::to_string(saveStartEncoderEncoded) +
            " frameQueueDepth=" + std::to_string(saveStartFrameQueueDepth) +
            " oldestFrameAge100ns=" + std::to_string(saveStartOldestFrameAge100ns) +
            " encoderQueueDepth=" + std::to_string(saveStartEncoderQueueDepth) +
            " nvencInFlight=" + std::to_string(saveStartNvencInFlight) +
            " captureGap100ns=" + std::to_string(saveStartCaptureGap100ns) +
            " capturePublicationAge100ns=" + std::to_string(saveStartCapturePublicationAge100ns));

    if (diagnostics_.activeEncoder != EncoderName::Nvenc) {
        result.message = "Cannot save clip: direct NVENC is not available, and this MVP requires NVENC first.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=nvenc_unavailable"));
        return result;
    }

    const std::size_t bufferedVideoPacketCount = videoReplayStore_
        ? videoReplayStore_->size()
        : videoPackets_.size();
    if (bufferedVideoPacketCount == 0) {
        result.message = "No encoded H.264 packets are buffered yet. Keep diagnostics open and wait for Encoder packets to rise above 0.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_video_packets"));
        return result;
    }

    const int duration = std::clamp(request.durationSeconds, 5, 600);
    // The clip ends now. Frames captured just before are usually still inside
    // the encoder (a few frame times); wait for them so the clip ends on real
    // frames rather than holding the last finished one until this moment.
    const int64_t saveHorizon100ns = mediaNow100ns();
    const auto catchUpStartedAt = SaveTimingClock::now();
    const bool encoderCaughtUp = encoderWorker_->waitForEncodedThrough(saveHorizon100ns, kSaveEncoderCatchUpLimit);
    logEngineSaveTiming("encoder_catch_up", catchUpStartedAt,
        std::string("reached=") + (encoderCaughtUp ? "true" : "false"));
    const auto snapshotStartedAt = SaveTimingClock::now();
    auto videoSnapshot = videoReplayStore_
        ? videoReplayStore_->snapshot()
        : videoPackets_.snapshot();
    const auto diskBackedVideoPackets = std::count_if(
        videoSnapshot.begin(),
        videoSnapshot.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    const auto videoArchiveStats = videoReplayStore_
        ? videoReplayStore_->stats()
        : ReplaySegmentStoreStats {};
    logEngineSaveTiming(
        "video_snapshot",
        snapshotStartedAt,
        "packets=" + std::to_string(videoSnapshot.size()) +
            " diskBacked=" + std::to_string(diskBackedVideoPackets) +
            " queued=" + std::to_string(videoArchiveStats.queuedPackets) +
            " queuedBytes=" + std::to_string(videoArchiveStats.queuedBytes) +
            " residentBytes=" + std::to_string(videoArchiveStats.residentPayloadBytes) +
            " residentBudgetBytes=" + std::to_string(videoArchiveStats.residentPayloadBudgetBytes) +
            " ramFallbackBytes=" + std::to_string(videoArchiveStats.ramFallbackBytes) +
            " rehomeQueued=" + std::to_string(videoArchiveStats.rehomeQueuedPackets) +
            " rehomedBytes=" + std::to_string(videoArchiveStats.rehomedBytes) +
            " rehomeBytesPerSecond=" + std::to_string(videoArchiveStats.rehomeBytesPerSecond));

    const auto videoSelectStartedAt = SaveTimingClock::now();
    auto clipPackets = settings_.saveInPlace
        ? replay::selectReplayVideo(std::move(videoSnapshot), duration, saveHorizon100ns, consumedWindow_.start())
        : selectVideoWindowForClip(std::move(videoSnapshot), duration, saveHorizon100ns);
    logEngineSaveTiming("video_select", videoSelectStartedAt, "clipPackets=" + std::to_string(clipPackets.size()));
    if (clipPackets.empty()) {
        result.message = "No complete keyframe-starting H.264 window is buffered yet. Wait about one second and try again.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_keyframe_window"));
        return result;
    }
    int actualDuration = actualClipDurationSeconds(clipPackets, duration);
    {
        // Judged from the encoded clip and facts captured when the source last
        // changed: nothing runs while recording for this.
        ClipSourceFacts source;
        if (captureSession_) {
            const auto stats = captureSession_->runtimeStats();
            source.recordingGameWindow = stats.recordingGameWindow;
            if (stats.recordedSourceSince100ns > 0) {
                source.sourceSeconds = static_cast<double>(monotonicNow100ns() - stats.recordedSourceSince100ns) / 10'000'000.0;
            }
            if (const auto recorded = static_cast<HMONITOR>(captureSession_->activeMonitor())) {
                MONITORINFO info {};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(recorded, &info)) source.recordedMonitorPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
                if (const HWND foreground = GetForegroundWindow()) {
                    const HMONITOR shown = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
                    source.foregroundOnRecordedMonitor = !shown || shown == recorded;
                }
            }
        }
        result.contentCheckJson = toJson(
            checkClipContent(std::span<const EncodedPacket>(clipPackets.data(), clipPackets.size())), source);
    }
    diagnostics_.lastClipCadence = analyzeVideoCadence(
        std::span<const EncodedPacket>(clipPackets.data(), clipPackets.size()),
        diagnostics_.fps);
    if (clipPackets.size() >= 2) {
        const auto videoTimelineStartedAt = SaveTimingClock::now();
        const int64_t targetFrameDuration100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
        int64_t maxGap100ns = 0;
        std::size_t longVfrGaps = 0;
        uint64_t unfilledTargetIntervals = 0;
        uint64_t uniqueSourceSequences = 0;
        uint64_t repeatedSourceTicks = 0;
        uint64_t previousSourceSequence = 0;
        for (const auto& packet : clipPackets) {
            if (packet.sourceFrameSequence == 0) continue;
            if (packet.sourceFrameSequence == previousSourceSequence) {
                ++repeatedSourceTicks;
            } else {
                ++uniqueSourceSequences;
                previousSourceSequence = packet.sourceFrameSequence;
            }
        }
        for (std::size_t i = 1; i < clipPackets.size(); ++i) {
            const int64_t gap100ns = std::max<int64_t>(1, clipPackets[i].pts100ns - clipPackets[i - 1].pts100ns);
            maxGap100ns = std::max(maxGap100ns, gap100ns);
            const int64_t missing = std::max<int64_t>(
                0,
                (gap100ns + targetFrameDuration100ns / 2) / targetFrameDuration100ns - 1);
            if (missing > 0) {
                ++longVfrGaps;
                unfilledTargetIntervals += static_cast<uint64_t>(missing);
            }
        }
        const int64_t span100ns = std::max<int64_t>(
            0,
            clipPackets.back().pts100ns - clipPackets.front().pts100ns +
                std::max<int64_t>(clipPackets.back().duration100ns, targetFrameDuration100ns));
        const uint64_t expectedOutputTicks = static_cast<uint64_t>(
            (span100ns + targetFrameDuration100ns / 2) / targetFrameDuration100ns);
        const double spanSeconds = static_cast<double>(span100ns) / 10'000'000.0;
        const double freshSourceFps = spanSeconds > 0.0
            ? static_cast<double>(uniqueSourceSequences) / spanSeconds
            : 0.0;
        const double repeatRatio = clipPackets.empty()
            ? 0.0
            : static_cast<double>(repeatedSourceTicks) / clipPackets.size();
        const auto captureStats = captureSession_
            ? captureSession_->runtimeStats()
            : CaptureRuntimeStats {};
        const auto encoderRecent = encoderWorker_
            ? encoderWorker_->recentPerformance()
            : EncoderRecentPerformance {};
        logEngineSaveTiming(
            "video_timeline",
            videoTimelineStartedAt,
            "span100ns=" + std::to_string(span100ns) +
                " targetFrame100ns=" + std::to_string(targetFrameDuration100ns) +
                " maxGap100ns=" + std::to_string(maxGap100ns) +
                " longVfrGaps=" + std::to_string(longVfrGaps) +
                " unfilledTargetIntervals=" + std::to_string(unfilledTargetIntervals) +
                " expectedOutputTicks=" + std::to_string(expectedOutputTicks) +
                " encodedPackets=" + std::to_string(clipPackets.size()) +
                " uniqueSourceSequences=" + std::to_string(uniqueSourceSequences) +
                " repeatedSourceTicks=" + std::to_string(repeatedSourceTicks) +
                " freshSourceFps=" + fixedNumber(freshSourceFps) +
                " repeatRatio=" + fixedNumber(repeatRatio, 4) +
                " worstSecondFreshSourceFps=" + fixedNumber(
                    diagnostics_.lastClipCadence.worstSecondDistinctSourceFps) +
                " desktopPresentSourceFps=" + fixedNumber(
                    diagnostics_.lastClipCadence.desktopPresentSourceFps) +
                " pointerOnlySourceFrames=" + std::to_string(
                    diagnostics_.lastClipCadence.pointerOnlySourceFrames) +
                " underTargetSeconds=" + std::to_string(
                    diagnostics_.lastClipCadence.underTargetSeconds) +
                " longestHeldRunSamples=" + std::to_string(
                    diagnostics_.lastClipCadence.longestHeldRunSamples) +
                " stillFrameDuplication=" + std::string(
                    diagnostics_.stillFrameDuplicationEnabled ? "true" : "false") +
                " captureBackend=\"" + jsonEscape(captureStats.activeBackend) + "\"" +
                " desktopPresentFps=" + fixedNumber(captureStats.desktopPresentFps) +
                " publishedFreshFps=" + fixedNumber(captureStats.publishedFreshFps) +
                " recentPublishedFreshFps=" + fixedNumber(captureStats.recentPublishedFreshFps) +
                " recentEncoderInputFps=" + fixedNumber(encoderRecent.inputFps) +
                " recentEncoderOutputFps=" + fixedNumber(encoderRecent.outputFps) +
                " capturePublicationAge100ns=" + std::to_string(
                    captureSession_ ? captureSession_->lastPublishedAge100ns() : 0) +
                " hdrTonemapping=" + std::string(
                    captureSession_ && captureSession_->hdrTonemappingActive() ? "true" : "false") +
                " encoderQueueDropsTotal=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0) +
                " configuredNvencPreset=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->effectiveNvencPreset() : 0) +
                " nvencSurfaceDropsTotal=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0) +
                " averageScaleLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageScaleLatency100ns() : 0) +
                " maximumScaleLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumScaleLatency100ns() : 0) +
                " averageInputMapLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageInputMapLatency100ns() : 0) +
                " maximumInputMapLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumInputMapLatency100ns() : 0) +
                " averageNvencCallLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageNvencCallLatency100ns() : 0) +
                " maximumNvencCallLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumNvencCallLatency100ns() : 0) +
                " averageOutputDrainLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageOutputDrainLatency100ns() : 0) +
                " maximumOutputDrainLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumOutputDrainLatency100ns() : 0) +
                " capturePreparationP95_100ns=" + std::to_string(
                    captureStats.framePreparationLatency.p95_100ns) +
                " captureProcessingP95_100ns=" + std::to_string(
                    captureStats.frameProcessingLatency.p95_100ns) +
                " inputPreparationP95_100ns=" + std::to_string(
                    encoderRecent.inputPreparation.p95_100ns) +
                " inputMapP95_100ns=" + std::to_string(encoderRecent.inputMap.p95_100ns) +
                " nvencCallP95_100ns=" + std::to_string(encoderRecent.encodeCall.p95_100ns) +
                " outputEventWaitP95_100ns=" + std::to_string(
                    encoderRecent.outputEventWait.p95_100ns) +
                " outputLockP95_100ns=" + std::to_string(encoderRecent.outputLock.p95_100ns) +
                " outputCopyP95_100ns=" + std::to_string(encoderRecent.outputCopy.p95_100ns) +
                " outputUnmapP95_100ns=" + std::to_string(encoderRecent.outputUnmap.p95_100ns) +
                " nvencZeroCopyFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencZeroCopyFrames() : 0) +
                " nvencCopyFallbackFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencCopyFallbackFrames() : 0) +
                " nvencConvertedFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencConvertedFrames() : 0) +
                " maximumSubmitLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumSubmitLatency100ns() : 0));
    }

    auto [width, height] = encodedResolutionFromPackets(clipPackets);
    if (width <= 0 || height <= 0) {
        const auto fallbackResolution = parseResolution(diagnostics_.resolution);
        width = fallbackResolution.first;
        height = fallbackResolution.second;
    }
    const auto clipResolution = formatResolution(width, height);
    auto [recommendedWidth, recommendedHeight] = lowestSourceResolutionFromPackets(clipPackets);
    if (recommendedWidth <= 0 || recommendedHeight <= 0) {
        recommendedWidth = width;
        recommendedHeight = height;
    }
    const auto recommendedResolution = formatResolution(recommendedWidth, recommendedHeight);
    const int64_t clipStart = clipPackets.front().pts100ns;
    const int64_t clipEnd = clipPackets.back().pts100ns + std::max<int64_t>(clipPackets.back().duration100ns, 0);
    const int64_t requestedClipStart = std::max<int64_t>(
        std::max(clipStart, consumedWindow_.start()),
        clipEnd - static_cast<int64_t>(duration) * 10'000'000LL);
    actualDuration = std::clamp(static_cast<int>((clipEnd - requestedClipStart + 5'000'000LL) / 10'000'000LL), 1, duration);
    savePacing.presentationStartPts100ns = requestedClipStart;
    savePacing.presentationEndPts100ns = clipEnd;
    const auto audioSelectStartedAt = SaveTimingClock::now();
    constexpr int64_t kAudioFrameLookbehind100ns = 2'000'000LL;
    const int64_t audioSelectionStart = std::max<int64_t>(0, clipStart - kAudioFrameLookbehind100ns);
    const int64_t liveAudioTarget100ns = std::max<int64_t>(
        audioSelectionStart,
        clipEnd - kAudioFrameLookbehind100ns);
    const bool liveAudioCaughtUp = audioReplayCoordinator_ &&
        audioReplayCoordinator_->waitUntil(liveAudioTarget100ns, std::chrono::milliseconds(25));
    auto encodedAudioPackets = aacReplayStore_
        ? aacReplayStore_->selectWindow(audioSelectionStart, clipEnd)
        : aacAudioPackets_.selectWindow(audioSelectionStart, clipEnd);
    const auto replayStats = audioReplayCoordinator_ ? audioReplayCoordinator_->stats() : AudioReplayStats {};
    const bool boundedPcmTail = liveAudioCaughtUp &&
        !replayStats.pcmRecoveryActive && replayStats.committedPts100ns > 0;
    const int64_t pcmSelectionStart = boundedPcmTail
        ? std::max(audioSelectionStart, replayStats.committedPts100ns - kAudioFrameLookbehind100ns)
        : audioSelectionStart;
    auto capturedAudioPackets = audioReplayCoordinator_
        ? audioReplayCoordinator_->selectPcmWindow(pcmSelectionStart, clipEnd)
        : audioPackets_.selectWindow(pcmSelectionStart, clipEnd);
    const auto diskBackedAudioPackets = std::count_if(
        encodedAudioPackets.begin(),
        encodedAudioPackets.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    const auto diskBackedPcmPackets = std::count_if(
        capturedAudioPackets.begin(),
        capturedAudioPackets.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    logEngineSaveTiming(
        "audio_select",
        audioSelectStartedAt,
            "aacPackets=" + std::to_string(encodedAudioPackets.size()) +
            " diskBackedAac=" + std::to_string(diskBackedAudioPackets) +
            " pcmFallbackPackets=" + std::to_string(capturedAudioPackets.size()) +
            " diskBackedPcm=" + std::to_string(diskBackedPcmPackets) +
            " liveCaughtUp=" + std::string(liveAudioCaughtUp ? "true" : "false") +
            " pcmTailOnly=" + std::string(boundedPcmTail ? "true" : "false") +
            " pcmSelectionStart100ns=" + std::to_string(pcmSelectionStart) +
            " queue=" + std::to_string(replayStats.queuedPackets) +
            " queueHighWatermark=" + std::to_string(replayStats.queueHighWatermark) +
            " queueOverflows=" + std::to_string(replayStats.queueOverflows) +
            " encoderRestarts=" + std::to_string(replayStats.encoderRestarts) +
            " pcmRecoveryActive=" + std::string(replayStats.pcmRecoveryActive ? "true" : "false") +
            " committedPts100ns=" + std::to_string(replayStats.committedPts100ns));

    const auto audioGroupStartedAt = SaveTimingClock::now();
    std::map<std::string, std::string> appTrackAliases;
    {
        std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
        appTrackAliases = activeAppAudioTrackAliases_;
    }

    std::map<std::string, std::vector<const EncodedPacket*>> audioByTrack;
    for (const auto& packet : capturedAudioPackets) {
        if (!packet.logicalTrackId.empty()) {
            audioByTrack[packet.logicalTrackId].push_back(&packet);
        } else if (packet.sourceId == "system-loopback-pcm" && settings_.includeSystemAudio) {
            audioByTrack["system-loopback-pcm"].push_back(&packet);
        } else if (packet.sourceId == "microphone-pcm" && settings_.includeMicrophoneAudio) {
            audioByTrack["microphone-pcm"].push_back(&packet);
        } else if (packet.sourceId.rfind("game:", 0) == 0 && settings_.captureGameAudio) {
            std::string exeName = packet.sourceId.substr(5);
            std::string gameName = getGameName(exeName);
            if (isMinimizedRobloxName(gameName)) continue;
            audioByTrack["game:" + gameName].push_back(&packet);
        } else if (packet.sourceId.rfind("app:", 0) == 0) {
            const auto alias = appTrackAliases.find(packet.sourceId);
            audioByTrack[alias == appTrackAliases.end() ? packet.sourceId : alias->second].push_back(&packet);
        }
    }
    logEngineSaveTiming(
        "audio_group",
        audioGroupStartedAt,
        "tracks=" + std::to_string(audioByTrack.size()) +
            " packets=" + std::to_string(capturedAudioPackets.size()) +
            " processChecks=0");
    
    std::vector<EncodedPacket> normalizedAudioPackets;
    std::vector<float> audioMixScratch;
    const auto audioMixStartedAt = SaveTimingClock::now();
    std::size_t mixedAudioPacketCount = 0;
    for (const auto& [trackId, packets] : audioByTrack) {
        const auto trackMixStartedAt = SaveTimingClock::now();
        int64_t retainedStart = clipEnd;
        int64_t retainedEnd = clipStart;
        for (const auto* packet : packets) {
            if (!packet) continue;
            retainedStart = std::min(retainedStart, packet->pts100ns);
            retainedEnd = std::max(
                retainedEnd,
                packet->pts100ns + std::max<int64_t>(1, packet->duration100ns));
        }
        retainedStart = std::max(clipStart, retainedStart);
        retainedEnd = std::min(clipEnd, retainedEnd);
        auto mixedTrack = mixPcmPackets(packets, trackId, retainedStart, retainedEnd, audioMixScratch);
        logEngineSaveTiming(
            "audio_mix_track",
            trackMixStartedAt,
            "track=\"" + jsonEscape(trackId) + "\" inputPackets=" + std::to_string(packets.size()) +
                " outputPackets=" + std::to_string(mixedTrack.size()) +
                " retainedSpan100ns=" + std::to_string(std::max<int64_t>(0, retainedEnd - retainedStart)));
        mixedAudioPacketCount += mixedTrack.size();
        normalizedAudioPackets.insert(normalizedAudioPackets.end(), mixedTrack.begin(), mixedTrack.end());
    }
    logEngineSaveTiming(
        "audio_mix",
        audioMixStartedAt,
        "tracks=" + std::to_string(audioByTrack.size()) +
            " outputPackets=" + std::to_string(mixedAudioPacketCount));

    std::vector<EncodedPacket> audioPackets;
    audioPackets.reserve(encodedAudioPackets.size() + normalizedAudioPackets.size());
    audioPackets.insert(
        audioPackets.end(),
        std::make_move_iterator(encodedAudioPackets.begin()),
        std::make_move_iterator(encodedAudioPackets.end()));
    audioPackets.insert(
        audioPackets.end(),
        std::make_move_iterator(normalizedAudioPackets.begin()),
        std::make_move_iterator(normalizedAudioPackets.end()));
    auto videoSegments = splitVideoSegmentRangesByEncodedResolution(clipPackets);
    std::vector<std::string> segmentFiles;
    std::vector<std::string> segmentResolutions;
    std::vector<std::vector<std::string>> segmentAudioTracks;
    std::vector<std::string> savedAudioTracks;
    std::string outputFilePath;
    std::string muxMessage;
    const std::size_t selectedVideoPacketCount = clipPackets.size();
    std::vector<std::string> ioAnalysisEntries;
    auto rememberIoAnalysis = [&](const MuxResult& mux) {
        if (!mux.ioAnalysis.enabled) return;
        ioAnalysisEntries.push_back(saveIoAnalysisToJson(mux.ioAnalysis));
        std::ostringstream json;
        json << '[';
        for (std::size_t i = 0; i < ioAnalysisEntries.size(); ++i) {
            if (i > 0) json << ',';
            json << ioAnalysisEntries[i];
        }
        json << ']';
        result.ioAnalysisJson = json.str();
    };

    if (videoSegments.size() > 1) {
        for (const auto& segment : videoSegments) {
            if (segment.begin >= segment.end || segment.end > clipPackets.size()) continue;
            std::vector<EncodedPacket> segmentMuxPackets(
                clipPackets.begin() + static_cast<std::ptrdiff_t>(segment.begin),
                clipPackets.begin() + static_cast<std::ptrdiff_t>(segment.end));
            const auto& firstPacket = clipPackets[segment.begin];
            const auto& lastPacket = clipPackets[segment.end - 1];
            const int64_t segmentStart = firstPacket.pts100ns;
            const int64_t segmentEnd = lastPacket.pts100ns + std::max<int64_t>(lastPacket.duration100ns, 0);
            if (segmentEnd <= requestedClipStart) continue;
            for (const auto& audioPacket : audioPackets) {
                const int64_t audioPacketEnd = audioPacket.pts100ns +
                    std::max<int64_t>(1, audioPacket.duration100ns);
                if (audioPacket.pts100ns < segmentEnd && audioPacketEnd > segmentStart) {
                    segmentMuxPackets.push_back(audioPacket);
                }
            }

            const int segmentWidth = segment.width > 0 ? segment.width : width;
            const int segmentHeight = segment.height > 0 ? segment.height : height;
            auto segmentPacing = savePacing;
            segmentPacing.presentationStartPts100ns = std::max(requestedClipStart, segmentStart);
            segmentPacing.presentationEndPts100ns = std::min(clipEnd, segmentEnd);
            const auto muxStartedAt = SaveTimingClock::now();
            const auto segmentMux = muxH264ToMp4(
                segmentMuxPackets,
                request.saveFolder,
                segmentWidth,
                segmentHeight,
                diagnostics_.fps,
                diagnostics_.bitrateMbps,
                std::move(segmentPacing));
            rememberIoAnalysis(segmentMux);
            logEngineSaveTiming(
                "mux_segment",
                muxStartedAt,
                "ok=" + std::string(segmentMux.ok ? "true" : "false") +
                    " packets=" + std::to_string(segmentMuxPackets.size()) +
                    " resolution=\"" + formatResolution(segmentWidth, segmentHeight) + "\"");
            if (!segmentMux.ok) {
                result.message = segmentMux.message;
                logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=mux_segment_failed"));
                return result;
            }
            segmentFiles.push_back(segmentMux.filePath);
            segmentAudioTracks.push_back(segmentMux.audioTracks);
            for (const auto& track : segmentMux.audioTracks) {
                if (std::find(savedAudioTracks.begin(), savedAudioTracks.end(), track) == savedAudioTracks.end()) {
                    savedAudioTracks.push_back(track);
                }
            }
            segmentResolutions.push_back(formatResolution(segmentWidth, segmentHeight));
            muxMessage = segmentMux.message;
        }
        if (segmentFiles.empty()) {
            result.message = "Segmented MP4 muxing failed: no video segments were writable.";
            logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_segment_files"));
            return result;
        }
        outputFilePath = stitchedPathForSegments(segmentFiles.front());
    } else {
        auto inPlaceSave = settings_.saveInPlace ? inPlaceArchive_->takeForSave() : nullptr;
        savePacing.experimentalInPlace = inPlaceSave.get();
        clipPackets.reserve(clipPackets.size() + audioPackets.size());
        clipPackets.insert(clipPackets.end(), audioPackets.begin(), audioPackets.end());
        if (inPlaceSave && clipEnd > clipStart) {
            uint64_t clipBytes = 0;
            for (const auto& packet : clipPackets) clipBytes += payloadSize(packet);
            const auto footageBytesPerSecond = static_cast<double>(clipBytes) * 10'000'000.0 /
                static_cast<double>(clipEnd - clipStart);
            inPlaceSave->setCopyPace(static_cast<uint64_t>(footageBytesPerSecond * replay::BackfillPace::saveCopyMultiplier));
        }
        const auto muxStartedAt = SaveTimingClock::now();
        const auto mux = muxH264ToMp4(
            clipPackets,
            request.saveFolder,
            width,
            height,
            diagnostics_.fps,
            diagnostics_.bitrateMbps,
            savePacing);
        rememberIoAnalysis(mux);
        if (inPlaceSave) {
            logEngineSaveTiming("in_place_copy", muxStartedAt,
                "copiedBytes=" + std::to_string(inPlaceSave->copiedBytes()));
        }
        logEngineSaveTiming(
            "mux",
            muxStartedAt,
            "ok=" + std::string(mux.ok ? "true" : "false") +
                " packets=" + std::to_string(clipPackets.size()) +
                " videoPackets=" + std::to_string(selectedVideoPacketCount) +
                " audioPackets=" + std::to_string(audioPackets.size()));

        if (!mux.ok) {
            result.message = mux.message;
            logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=mux_failed"));
            return result;
        }
        outputFilePath = mux.filePath;
        savedAudioTracks = mux.audioTracks;
        if (mux.learnedWriteBytesPerSecond > 0) {
            learnedSaveWriteFolder_ = request.saveFolder;
            learnedSaveWriteBytesPerSecond_ = mux.learnedWriteBytesPerSecond;
        }
        muxMessage = mux.message;
    }

    HMONITOR activeMonitor = captureSession_ ? static_cast<HMONITOR>(captureSession_->activeMonitor()) : nullptr;
    auto [fgApp, fgIsGame] = detectForegroundAppInfo(readForegroundProcess(), activeMonitor);
    std::vector<std::string> focusedApps;
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(actualDuration);
        std::map<std::string, std::pair<int, bool>> appCounts;
        for (const auto& entry : foregroundHistory_) {
            if (entry.timestamp >= cutoff) {
                appCounts[entry.appName].first++;
                appCounts[entry.appName].second = entry.isGame;
            }
        }
        int maxAppCount = 0;
        int maxGameCount = 0;
        std::string topApp;
        std::string topGame;

        for (const auto& [appName, data] : appCounts) {
            if (appName != "Foreground app") {
                focusedApps.push_back(appName);
            }
            if (data.second) {
                if (data.first > maxGameCount) {
                    maxGameCount = data.first;
                    topGame = appName;
                }
            } else {
                if (data.first > maxAppCount) {
                    maxAppCount = data.first;
                    topApp = appName;
                }
            }
        }

        if (maxGameCount > 0) {
            fgApp = topGame;
            fgIsGame = true;
        } else if (maxAppCount > 0) {
            fgApp = topApp;
            fgIsGame = false;
        }
    }
    std::ostringstream clip;
    clip << "{"
         << "\"id\":\"clip-" << nowIdSuffix() << "\","
         << "\"title\":\"Clipture\","
         << "\"gameOrApp\":\"" << jsonEscape(fgApp) << "\","
         << "\"isGame\":" << (fgIsGame ? "true" : "false") << ","
         << "\"createdAt\":\"" << nowIsoLike() << "\","
         << "\"durationSeconds\":" << actualDuration << ","
         << "\"filePath\":\"" << jsonEscape(outputFilePath) << "\","
         << "\"resolution\":\"" << clipResolution << "\","
         << "\"recommendedResolution\":\"" << recommendedResolution << "\","
         << "\"fps\":" << diagnostics_.fps << ","
         << "\"encoder\":\"" << encoderName(diagnostics_.activeEncoder) << "\","
         << "\"audioTracks\":[";
    bool wroteTrack = false;
    for (const auto& trackName : savedAudioTracks) {
        if (wroteTrack) clip << ",";
        clip << "\"" << jsonEscape(trackName) << "\"";
        wroteTrack = true;
    }
    clip << "]";
    if (!segmentFiles.empty()) {
        clip << R"(,"segmentAudioTracks":[)";
        for (std::size_t segment = 0; segment < segmentAudioTracks.size(); ++segment) {
            if (segment > 0) clip << ",";
            clip << "[";
            for (std::size_t track = 0; track < segmentAudioTracks[segment].size(); ++track) {
                if (track > 0) clip << ",";
                clip << R"(")" << jsonEscape(segmentAudioTracks[segment][track]) << R"(")";
            }
            clip << "]";
        }
        clip << "]";
        clip << ",\"segmentFiles\":[";
        for (std::size_t i = 0; i < segmentFiles.size(); ++i) {
            if (i > 0) clip << ",";
            clip << "\"" << jsonEscape(segmentFiles[i]) << "\"";
        }
        clip << "]";
        clip << ",\"segmentResolutions\":[";
        for (std::size_t i = 0; i < segmentResolutions.size(); ++i) {
            if (i > 0) clip << ",";
            clip << "\"" << jsonEscape(segmentResolutions[i]) << "\"";
        }
        clip << "]";
    }
    
    clip << ",\"focusedApps\":[";
    for (std::size_t i = 0; i < focusedApps.size(); ++i) {
        if (i > 0) clip << ",";
        clip << "\"" << jsonEscape(focusedApps[i]) << "\"";
    }
    clip << "]";
    
    clip << "}";

    result.ok = true;
    consumedWindow_.commit(clipEnd);
    if (settings_.saveInPlace) {
        // No later save shows this footage again, so stop holding the saved
        // clip open for it (until now it stayed open, and could not be
        // trimmed, until the footage expired). Only the keyframe preroll and
        // AAC lookbehind the next save needs are copied back, at once.
        if (videoReplayStore_) videoReplayStore_->discardBefore(clipEnd);
        if (aacReplayStore_) aacReplayStore_->discardBefore(clipEnd - kAudioFrameLookbehind100ns);
        if (videoReplayStore_) videoReplayStore_->scheduleInPlaceRehome(replay::BackfillPace::maximumMultiplier);
        if (aacReplayStore_) aacReplayStore_->scheduleInPlaceRehome(replay::BackfillPace::maximumMultiplier);
    }
    result.message = segmentFiles.empty()
        ? muxMessage
        : "Saved segmented MP4 clip for export stitching.";
    result.clipJson = clip.str();
    logEngineSaveTiming(
        "total",
        totalStartedAt,
        saveTotalDetails("ok=true videoPackets=" + std::to_string(selectedVideoPacketCount) +
            " audioPackets=" + std::to_string(audioPackets.size()) +
            " segments=" + std::to_string(segmentFiles.size())));
    return result;
}


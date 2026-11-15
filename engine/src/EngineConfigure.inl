// Part of Engine.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
const Diagnostics& Engine::configure(const EngineSettings& settings) {
    if (adminHotkeyRelay_) adminHotkeyRelay_->setEnabled(settings.adminGameHotkey);
    const auto previousMonitorId = settings_.monitorId;
    const auto previousTargetWidth = settings_.targetWidth;
    const auto previousTargetHeight = settings_.targetHeight;
    const auto previousCaptureGameAudio = settings_.captureGameAudio;
    const auto previousCaptureForegroundSystemAudio = settings_.captureForegroundSystemAudio;
    const auto previousMicVolume = settings_.micVolume;
    const auto previousMicIsolation = settings_.micIsolation;
    const auto previousMicIsolationWeight = settings_.micIsolationWeight;
    const auto previousNoiseGateEnabled = settings_.noiseGateEnabled;
    const auto previousAutoNoiseGate = settings_.autoNoiseGate;
    const auto previousNoiseGateThreshold = settings_.noiseGateThreshold;
    const auto previousNoiseGateDebounceMs = settings_.noiseGateDebounceMs;
    const auto previousMicDeviceId = settings_.micDeviceId;
    const auto previousMicDeviceMatchKey = settings_.micDeviceMatchKey;
    const auto previousMicDeviceName = settings_.micDeviceName;
    const auto previousAppAudioProcesses = settings_.appAudioProcesses;
    const auto previousSystemAudioProcesses = settings_.systemAudioProcesses;

    settings_.fps = std::clamp(settings.fps, 24, 240);
    settings_.bitrateMbps = std::clamp(settings.bitrateMbps, 4, 120);
    settings_.nvencPreset = std::clamp(settings.nvencPreset, 1, 5);
    settings_.clipLengthSeconds = std::clamp(settings.clipLengthSeconds, 5, 600);
    settings_.saveInPlace = settings.saveInPlace;
    settings_.saveFolder = settings.saveFolder;
    settings_.gpuPriorityBoost = settings.gpuPriorityBoost;
    settings_.gpuPriorityMemoryPath = settings.gpuPriorityMemoryPath;
    gpuPriority_.apply(settings_.gpuPriorityBoost, settings_.gpuPriorityMemoryPath, diagnostics_);
    // Each in-place save starts where the previous one ended (ADR 0026), so no
    // saved footage has to be copied back into the live buffer.
    consumedWindow_.enable(settings_.saveInPlace);
    settings_.monitorId = settings.monitorId.empty() ? "primary" : settings.monitorId;
    settings_.targetWidth = std::max(0, settings.targetWidth);
    settings_.targetHeight = std::max(0, settings.targetHeight);
    const bool videoOutputTargetChanged =
        previousTargetWidth != settings_.targetWidth ||
        previousTargetHeight != settings_.targetHeight;
    if (videoOutputTargetChanged) {
        videoPackets_.clear();
        if (videoReplayStore_) videoReplayStore_->clear();
    }
    if (settings_.targetWidth > 0 && settings_.targetHeight > 0) {
        pendingAutoVideoResolutionReset100ns_ = 0;
    }
    settings_.includeMixedAudio = settings.includeMixedAudio;
    settings_.includeSystemAudio = settings.includeSystemAudio;
    settings_.includeMicrophoneAudio = settings.includeMicrophoneAudio;
    if (audioCaptureWorker_) {
        audioCaptureWorker_->setCoreSources(coreSystemAudioNeeded(settings_), coreMicrophoneNeeded(settings_));
    }
    settings_.captureGameAudio = settings.captureGameAudio;
    settings_.captureForegroundSystemAudio = settings.captureForegroundSystemAudio;
    if (!settings_.captureForegroundSystemAudio) {
        foregroundSystemProcesses_.clear();
    }
    settings_.micVolume = settings.micVolume;
    settings_.micIsolation = settings.micIsolation;
    settings_.micIsolationWeight = settings.micIsolationWeight;
    settings_.noiseGateEnabled = settings.noiseGateEnabled;
    settings_.autoNoiseGate = settings.autoNoiseGate;
    settings_.noiseGateThreshold = settings.noiseGateThreshold;
    settings_.noiseGateDebounceMs = settings.noiseGateDebounceMs;
    settings_.micDeviceId = settings.micDeviceId;
    settings_.micDeviceMatchKey = settings.micDeviceMatchKey;
    settings_.micDeviceName = settings.micDeviceName;
    settings_.appAudioProcesses = settings.appAudioProcesses;
    settings_.systemAudioProcesses = settings.systemAudioProcesses;
    if (encoderWorker_) {
        const auto [maxEncodeWidth, maxEncodeHeight] = encoderMaxDimensions(settings_);
        encoderWorker_->configure(
            settings_.fps,
            settings_.bitrateMbps,
            settings_.targetWidth,
            settings_.targetHeight,
            maxEncodeWidth,
            maxEncodeHeight,
            settings_.nvencPreset,
            videoOutputTargetChanged);
    }
    if (captureSession_) {
        captureSession_->setTargetFps(settings_.fps);
        if (captureSession_->running() && previousMonitorId != settings_.monitorId) {
            if (encoderWorker_) {
                if (settings_.targetWidth <= 0 || settings_.targetHeight <= 0) {
                    videoPackets_.clear();
                    if (videoReplayStore_) videoReplayStore_->clear();
                    encoderWorker_->resetAutoOutputResolution();
                } else {
                    encoderWorker_->requireFreshFrame(true);
                }
            }
            const bool captureStarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
            diagnostics_.captureReady = captureStarted;
            diagnostics_.degraded = !diagnostics_.hardwareAcceleration || !captureStarted;
            diagnostics_.status = captureSession_->status();
            if (captureStarted) {
                diagnostics_.resolution = captureSession_->resolution();
                diagnostics_.display = captureSession_->displayName();
                diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
            }
        }
    }
    const bool appAudioChanged =
        previousCaptureGameAudio != settings_.captureGameAudio ||
        previousCaptureForegroundSystemAudio != settings_.captureForegroundSystemAudio ||
        previousAppAudioProcesses != settings_.appAudioProcesses ||
        previousSystemAudioProcesses != settings_.systemAudioProcesses;
    const bool micSettingsChanged =
        previousMicVolume != settings_.micVolume ||
        previousMicIsolation != settings_.micIsolation ||
        previousMicIsolationWeight != settings_.micIsolationWeight ||
        previousNoiseGateEnabled != settings_.noiseGateEnabled ||
        previousAutoNoiseGate != settings_.autoNoiseGate ||
        previousNoiseGateThreshold != settings_.noiseGateThreshold ||
        previousNoiseGateDebounceMs != settings_.noiseGateDebounceMs ||
        previousMicDeviceId != settings_.micDeviceId ||
        previousMicDeviceMatchKey != settings_.micDeviceMatchKey ||
        previousMicDeviceName != settings_.micDeviceName;

    if (audioCaptureWorker_ && appAudioChanged) {
        {
            std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
            activeAppAudioSources_ = settings_.appAudioProcesses;
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                if (!containsProcessName(activeAppAudioSources_, systemProcess)) {
                    activeAppAudioSources_.push_back(systemProcess);
                }
            }
            activeAppAudioTrackAliases_.clear();
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                activeAppAudioTrackAliases_[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
            }
        }
        std::vector<std::string> captureSources = settings_.appAudioProcesses;
        for (const auto& systemProcess : settings_.systemAudioProcesses) {
            if (!containsProcessName(captureSources, systemProcess)) captureSources.push_back(systemProcess);
        }
        audioCaptureWorker_->configureAppSources(captureSources);
    }
    refreshAudioRouting();
    if (audioCaptureWorker_ && micSettingsChanged) {
        audioCaptureWorker_->setMicSettings(settings_.micVolume, settings_.micIsolation, settings_.micIsolationWeight, settings_.noiseGateEnabled, settings_.autoNoiseGate, settings_.noiseGateThreshold, settings_.noiseGateDebounceMs, settings_.micDeviceId, settings_.micDeviceMatchKey, settings_.micDeviceName);
    }

    diagnostics_.fps = settings_.fps;
    diagnostics_.bitrateMbps = settings_.bitrateMbps;
    diagnostics_.bufferDurationSeconds = settings_.clipLengthSeconds;
    diagnostics_.encoderMode = "NVENC P" + std::to_string(settings_.nvencPreset) + " (buffered sync preferred; async compatibility fallback)";
    diagnostics_.microphoneDevice = audioCaptureWorker_ ? audioCaptureWorker_->microphoneStatus() : microphoneDeviceName(settings_.micDeviceId);
    diagnostics_.clipTargetResolution = settings_.targetWidth > 0 && settings_.targetHeight > 0
        ? formatResolution(settings_.targetWidth, settings_.targetHeight)
        : "Clip-aware system";

    const auto retention100ns = static_cast<int64_t>(settings_.clipLengthSeconds + 5) * 10'000'000LL;
    const auto replayBudgets = replayMemoryBudgets(settings_);
    // The cap bounds arena growth even if snapshots pin old regions or variable
    // packet sizes fragment free space. Full/failed arenas use the legacy spill.
    const auto arenaLimit = static_cast<uint64_t>(settings_.bitrateMbps + 16) * 125000ULL *
        static_cast<uint64_t>(settings_.clipLengthSeconds + 10) * 2;
    const std::u8string archiveFolder(settings_.saveFolder.begin(), settings_.saveFolder.end());
    inPlaceArchive_->configure(std::filesystem::path(archiveFolder), settings_.saveInPlace, arenaLimit);
    const auto archive = settings_.saveInPlace ? inPlaceArchive_ : nullptr;
    videoPackets_.setRetention(kHotReplayRetention100ns);
    if (videoReplayStore_) {
        videoReplayStore_->setRetention(retention100ns);
        videoReplayStore_->setInPlaceArchive(archive);
        videoReplayStore_->setResidentPayloadBudget(settings_.saveInPlace ? 0 : replayBudgets.videoBytes);
    }
    if (aacReplayStore_) {
        aacReplayStore_->setRetention(retention100ns);
        aacReplayStore_->setInPlaceArchive(archive);
        aacReplayStore_->setResidentPayloadBudget(settings_.saveInPlace ? 0 : replayBudgets.audioBytes);
    }
    if (pcmRecoveryStore_) {
        pcmRecoveryStore_->setRetention(retention100ns);
        pcmRecoveryStore_->setResidentPayloadBudget(replayBudgets.pcmRecoveryBytes);
    }
    if (audioReplayCoordinator_) {
        audioReplayCoordinator_->setRetention(retention100ns);
        aacAudioPackets_.setRetention(kHotReplayRetention100ns);
    } else {
        audioPackets_.setRetention(retention100ns);
        aacAudioPackets_.setRetention(kHotReplayRetention100ns);
    }
    if (!armed_) {
        armed_ = true;
        arm();
        gameDetectionRunning_ = true;
        audioSessionWatcher_ = std::make_unique<AudioSessionWatcher>([this] { onAudioSessionsChanged(); });
        gameDetectionThread_ = std::thread([this] { gameDetectionLoop(); });
    }
    refreshPacketCounts();
    return diagnostics_;
}

std::vector<EncodedPacket> mixPcmPackets(
    const std::vector<const EncodedPacket*>& packets,
    const std::string& newSourceId,
    int64_t clipStart,
    int64_t clipEnd,
    std::vector<float>& mixScratch) {
    if (packets.empty()) return {};

    int sampleRate = 0;
    int channels = 0;

    for (const auto* packet : packets) {
        if (!packet) continue;
        if (sampleRate == 0) sampleRate = packet->sampleRate;
        if (channels == 0) channels = packet->channelCount;
    }

    if (clipStart >= clipEnd || sampleRate == 0 || channels == 0) return {};

    struct PcmInputView {
        const EncodedPacket* packet = nullptr;
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        int frames = 0;
    };

    const int bytesPerFrame = channels * static_cast<int>(sizeof(int16_t));
    auto timelineFrameFrom100ns = [sampleRate](int64_t offset100ns) {
        return (offset100ns * sampleRate) / 10'000'000LL;
    };

    const int64_t totalDuration = clipEnd - clipStart;
    const int64_t totalSamples = timelineFrameFrom100ns(totalDuration);
    if (totalSamples <= 0) return {};

    std::vector<PcmInputView> inputs;
    inputs.reserve(packets.size());
    bool hasAudio = false;
    for (const auto* packet : packets) {
        if (!packet) continue;
        const auto& p = *packet;
        if (p.sampleRate != sampleRate || p.channelCount != channels) continue;
        if (!p.audible || payloadEmpty(p)) continue;

        const int frames = static_cast<int>(payloadSize(p) / static_cast<std::size_t>(bytesPerFrame));
        if (frames <= 0) continue;
        const int64_t startFrame = timelineFrameFrom100ns(p.pts100ns - clipStart);
        const int64_t endFrame = startFrame + frames;
        if (endFrame <= 0 || startFrame >= totalSamples) continue;

        hasAudio = true;
        inputs.push_back(PcmInputView { &p, startFrame, endFrame, frames });
    }

    if (inputs.empty() || !hasAudio) {
        return {};
    }

    std::sort(inputs.begin(), inputs.end(), [](const PcmInputView& a, const PcmInputView& b) {
        return a.startFrame < b.startFrame;
    });

    std::vector<EncodedPacket> mixed;
    const int framesPerPacket = std::max(1, sampleRate / 100);
    mixed.reserve(static_cast<std::size_t>((totalSamples + framesPerPacket - 1) / framesPerPacket));
    mixScratch.resize(static_cast<std::size_t>(framesPerPacket) * static_cast<std::size_t>(channels));
    std::vector<std::byte> inputReadScratch;

    std::size_t firstCandidate = 0;
    for (int64_t sampleOffset = 0; sampleOffset < totalSamples; sampleOffset += framesPerPacket) {
        const int frames = static_cast<int>(std::min<int64_t>(framesPerPacket, totalSamples - sampleOffset));
        const std::size_t scratchSamples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
        std::fill(mixScratch.begin(), mixScratch.begin() + static_cast<std::ptrdiff_t>(scratchSamples), 0.0f);

        const int64_t chunkStart100ns = clipStart + (sampleOffset * 10'000'000LL) / sampleRate;

        while (firstCandidate < inputs.size() &&
               inputs[firstCandidate].endFrame <= sampleOffset) {
            ++firstCandidate;
        }

        for (std::size_t inputIndex = firstCandidate; inputIndex < inputs.size(); ++inputIndex) {
            const auto& input = inputs[inputIndex];
            if (input.startFrame >= sampleOffset + frames) break;

            const int64_t overlapStartFrame = std::max(sampleOffset, input.startFrame);
            const int64_t overlapEndFrame = std::min(sampleOffset + frames, input.endFrame);
            if (overlapEndFrame <= overlapStartFrame) continue;

            const int64_t outputStartFrame = overlapStartFrame - sampleOffset;
            const int64_t inputStartFrame = overlapStartFrame - input.startFrame;
            const int64_t copyFrames = std::min<int64_t>({
                overlapEndFrame - overlapStartFrame,
                frames - outputStartFrame,
                input.frames - inputStartFrame
            });
            if (copyFrames <= 0) continue;

            const std::size_t inputByteOffset =
                static_cast<std::size_t>(inputStartFrame) * static_cast<std::size_t>(bytesPerFrame);
            const std::size_t inputByteCount =
                static_cast<std::size_t>(copyFrames) * static_cast<std::size_t>(bytesPerFrame);
            const int16_t* inputSamples = nullptr;
            const auto residentBytes = payloadBytes(*input.packet);
            if (!residentBytes.empty()) {
                if (inputByteOffset > residentBytes.size() ||
                    inputByteCount > residentBytes.size() - inputByteOffset) {
                    continue;
                }
                inputSamples = reinterpret_cast<const int16_t*>(residentBytes.data() + inputByteOffset);
            } else {
                inputReadScratch.resize(inputByteCount);
                if (!readPayload(*input.packet, inputByteOffset, inputReadScratch)) continue;
                inputSamples = reinterpret_cast<const int16_t*>(inputReadScratch.data());
            }

            for (int64_t frame = 0; frame < copyFrames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    const std::size_t outIndex = static_cast<std::size_t>((outputStartFrame + frame) * channels + ch);
                    const std::size_t inIndex = static_cast<std::size_t>(frame * channels + ch);
                    mixScratch[outIndex] += inputSamples[inIndex];
                }
            }
        }

        float peak = 0.0f;
        for (std::size_t sample = 0; sample < scratchSamples; ++sample) {
            peak = std::max(peak, std::abs(mixScratch[sample]));
        }
        const float limiterGain = peak > 32767.0f ? 32767.0f / peak : 1.0f;

        EncodedPacket mp;
        mp.kind = PacketKind::Audio;
        mp.codec = PacketCodec::PcmS16;
        mp.pts100ns = chunkStart100ns;
        mp.dts100ns = mp.pts100ns;
        mp.duration100ns = (frames * 10'000'000) / sampleRate;
        mp.sourceId = newSourceId;
        mp.logicalTrackId = newSourceId;
        mp.encoderId = "PCM_S16";
        mp.sampleRate = sampleRate;
        mp.channelCount = channels;
        mp.bitsPerSample = 16;
        mp.audioFrameCount = static_cast<uint32_t>(frames);
        mp.audible = true;
        mp.payload = std::make_shared<PacketPayload>(frames * channels * sizeof(int16_t));
        
        int16_t* outPcm = reinterpret_cast<int16_t*>(mutablePayload(mp).data());
        for (int j = 0; j < frames; ++j) {
            for (int ch = 0; ch < channels; ++ch) {
                float val = mixScratch[static_cast<std::size_t>(j * channels + ch)] * limiterGain;
                outPcm[j * channels + ch] = static_cast<int16_t>(std::clamp(val, -32768.0f, 32767.0f));
            }
        }
        mixed.push_back(std::move(mp));
    }
    return mixed;
}

uint64_t Engine::videoDropTotal() const {
    uint64_t total = frameQueue_.stats().overflowDrops;
    if (captureSession_) total += captureSession_->ownedSlotDrops();
    if (encoderWorker_) {
        total += static_cast<uint64_t>(std::max(0, encoderWorker_->schedulerDroppedFrames()));
        total += static_cast<uint64_t>(std::max(0, encoderWorker_->encoderBackpressureDrops()));
    }
    return total;
}


// Part of Mp4Muxer.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
MuxResult muxH264ToMp4(
    const std::vector<EncodedPacket>& packets,
    const std::string& saveFolder,
    int width,
    int height,
    int fps,
    int /*bitrateMbps*/,
    MuxWritePacing pacing) {
    MuxResult result;
    const auto totalStartedAt = SaveTimingClock::now();
    logMuxSaveTiming(
        "start",
        totalStartedAt,
        "packets=" + std::to_string(packets.size()) +
            " resolution=\"" + std::to_string(width) + "x" + std::to_string(height) + "\"" +
            " fps=" + std::to_string(fps));

    bool hasVideoPacket = false;
    std::vector<VideoSamplePlan> videoSamples;
    std::vector<PcmAudioTrack> pcmAudioTracks;
    std::vector<AacAudioTrack> audioTracks;
    std::vector<std::byte> sps;
    std::vector<std::byte> pps;
    std::size_t videoPacketCount = 0;
    std::size_t pcmPacketCount = 0;
    std::size_t aacPacketCount = 0;
    std::size_t writableNaluCount = 0;
    uint64_t videoSourceBytes = 0;
    uint64_t pcmSourceBytes = 0;
    uint64_t residentEncodedSourceBytes = 0;
    uint64_t diskBackedEncodedSourceBytes = 0;

    auto countEncodedSourceStorage = [&](const EncodedPacket& packet) {
        if (packet.payload) {
            residentEncodedSourceBytes += payloadSize(packet);
        } else if (packet.payloadReader) {
            diskBackedEncodedSourceBytes += payloadSize(packet);
        }
    };

    const auto prepassStartedAt = SaveTimingClock::now();
    const auto inPlaceBefore = pacing.experimentalInPlace ? pacing.experimentalInPlace->io() : replay::InPlaceIo{};
    for (const auto& packet : packets) {
        if (isVideoPacket(packet)) {
            hasVideoPacket = true;
            ++videoPacketCount;
            videoSourceBytes += payloadSize(packet);
            countEncodedSourceStorage(packet);
            VideoSamplePlan sample;
            sample.packet = &packet;
            sample.info.keyframe = packet.keyframe;
            auto collectNalu = [&](const NalUnit& nalu) {
                if (nalu.type == 7 && sps.empty()) {
                    sps = copyNaluPayload(packet, nalu);
                } else if (nalu.type == 8 && pps.empty()) {
                    pps = copyNaluPayload(packet, nalu);
                }

                if (isWritableVideoNalu(nalu)) {
                    sample.writableNalus.push_back(nalu);
                }
            };

            if (packet.codec == PacketCodec::H264Avcc) {
                if (packet.h264Config) {
                    if (sps.empty()) sps = packet.h264Config->sps;
                    if (pps.empty()) pps = packet.h264Config->pps;
                }
                sample.info.size = payloadSize(packet);
            } else if (packet.h264.analyzed) {
                forEachH264Nal(packet.h264, collectNalu);
                sample.info.size = packet.h264.avccSampleSize;
            } else {
                std::vector<std::byte> materialized;
                if (copyPayloadRange(packet, 0, payloadSize(packet), materialized)) {
                    const auto nalus = parseAnnexBNalus(materialized);
                    for (const auto& nalu : nalus) collectNalu(nalu);
                    sample.info.size = avccSampleSize(sample.writableNalus);
                }
            }
            if (sample.info.size > 0) {
                writableNaluCount += sample.writableNalus.size();
                videoSamples.push_back(std::move(sample));
            }
        } else if (isAacAudioPacket(packet)) {
            ++aacPacketCount;
            countEncodedSourceStorage(packet);
            const auto& trackId = packetTrackId(packet);
            auto track = std::find_if(audioTracks.begin(), audioTracks.end(), [&](const AacAudioTrack& candidate) {
                return candidate.sourceId == trackId &&
                    candidate.sampleRate == packet.sampleRate &&
                    candidate.channels == packet.channelCount;
            });
            if (track == audioTracks.end()) {
                audioTracks.push_back(AacAudioTrack {
                    trackId,
                    packet.sampleRate,
                    packet.channelCount,
                    makeAacAudioSpecificConfig(packet.sampleRate, packet.channelCount),
                    {},
                    packet.pts100ns
                });
                track = std::prev(audioTracks.end());
            }
            OwnedSample sample;
            sample.sharedPayload = packet.payload;
            sample.payloadReader = packet.payloadReader;
            sample.info.size = payloadSize(packet);
            sample.info.duration = packet.audioFrameCount;
            sample.pts100ns = packet.pts100ns;
            sample.encoderEpoch = packet.encoderEpoch;
            sample.primingFrames = packet.audioPrimingFrames;
            sample.encodedFrameCount = packet.audioFrameCount;
            track->samples.push_back(std::move(sample));
            if (track->firstPts100ns == 0 || packet.pts100ns < track->firstPts100ns) {
                track->firstPts100ns = packet.pts100ns;
            }
        } else if (isPcmAudioPacket(packet)) {
            ++pcmPacketCount;
            const auto& trackId = packetTrackId(packet);
            auto track = std::find_if(pcmAudioTracks.begin(), pcmAudioTracks.end(), [&](const PcmAudioTrack& candidate) {
                return candidate.sourceId == trackId &&
                    candidate.sampleRate == packet.sampleRate &&
                    candidate.channels == packet.channelCount;
            });
            if (track == pcmAudioTracks.end()) {
                pcmAudioTracks.push_back(PcmAudioTrack {
                    trackId,
                    packet.sampleRate,
                    packet.channelCount,
                    {}
                });
                track = std::prev(pcmAudioTracks.end());
            }

            const auto bytes = payloadSize(packet);
            pcmSourceBytes += bytes;
            const auto bytesPerFrame = static_cast<std::size_t>(std::max(1, packet.channelCount) * 2);
            const auto frames = static_cast<uint32_t>(std::max<std::size_t>(1, bytes / bytesPerFrame));
            track->samples.push_back(PcmSampleView {
                packet.payload,
                packet.payloadReader,
                bytes,
                packet.pts100ns,
                frames
            });
        }
    }
    logMuxSaveTiming(
        "prepass",
        prepassStartedAt,
        "videoPackets=" + std::to_string(videoPacketCount) +
            " videoSamples=" + std::to_string(videoSamples.size()) +
            " writableNalus=" + std::to_string(writableNaluCount) +
            " videoBytes=" + std::to_string(videoSourceBytes) +
            " pcmPackets=" + std::to_string(pcmPacketCount) +
            " pcmTracks=" + std::to_string(pcmAudioTracks.size()) +
            " pcmBytes=" + std::to_string(pcmSourceBytes) +
            " aacPackets=" + std::to_string(aacPacketCount) +
            " aacTracks=" + std::to_string(audioTracks.size()) +
            " residentEncodedBytes=" + std::to_string(residentEncodedSourceBytes) +
            " diskBackedEncodedBytes=" + std::to_string(diskBackedEncodedSourceBytes));
    if (!hasVideoPacket) {
        result.message = "No encoded H.264 packets are buffered yet.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=no_video_packets");
        return result;
    }

    const auto avcConfig = buildAvcDecoderConfig(sps, pps);
    if (avcConfig.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window has no SPS/PPS decoder header yet.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=missing_avc_config");
        return result;
    }

    // The video media timescale is 10,000,000, so each stts duration can carry
    // the exact 100 ns delta between consecutive hardware capture timestamps.
    const auto durationStartedAt = SaveTimingClock::now();
    if (videoSamples.size() >= 2) {
        for (std::size_t i = 0; i + 1 < videoSamples.size(); ++i) {
            videoSamples[i].info.duration = videoSampleDuration100ns(
                videoSamples[i].packet->pts100ns,
                videoSamples[i + 1].packet->pts100ns);
        }
        videoSamples.back().info.duration = finalVideoSampleDuration100ns(
            videoSamples.back().packet->duration100ns, fps);
    } else if (videoSamples.size() == 1) {
        videoSamples[0].info.duration = finalVideoSampleDuration100ns(
            videoSamples[0].packet->duration100ns, fps);
    }
    if (videoSamples.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window contains no writable frame samples.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=no_writable_samples");
        return result;
    }
    const uint64_t plannedVideoDuration100ns = samplesDuration(videoSamples);
    const int64_t videoMediaStartPts100ns = videoSamples.front().packet
        ? videoSamples.front().packet->pts100ns
        : 0;
    const int64_t boundedMediaDuration100ns = static_cast<int64_t>(std::min<uint64_t>(
        plannedVideoDuration100ns,
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    const int64_t videoMediaEndPts100ns = videoMediaStartPts100ns >
            std::numeric_limits<int64_t>::max() - boundedMediaDuration100ns
        ? std::numeric_limits<int64_t>::max()
        : videoMediaStartPts100ns + boundedMediaDuration100ns;
    int64_t videoPresentationStartPts100ns = pacing.presentationStartPts100ns > 0
        ? std::clamp(
            pacing.presentationStartPts100ns,
            videoMediaStartPts100ns,
            videoMediaEndPts100ns)
        : videoMediaStartPts100ns;
    int64_t videoPresentationEndPts100ns = pacing.presentationEndPts100ns > videoPresentationStartPts100ns
        ? std::clamp(
            pacing.presentationEndPts100ns,
            videoPresentationStartPts100ns,
            videoMediaEndPts100ns)
        : videoMediaEndPts100ns;
    if (videoPresentationEndPts100ns <= videoPresentationStartPts100ns) {
        videoPresentationStartPts100ns = videoMediaStartPts100ns;
        videoPresentationEndPts100ns = videoMediaEndPts100ns;
    }
    const uint64_t movieDuration100ns = static_cast<uint64_t>(
        videoPresentationEndPts100ns - videoPresentationStartPts100ns);
    const uint64_t videoMediaStart100ns = static_cast<uint64_t>(
        videoPresentationStartPts100ns - videoMediaStartPts100ns);
    const uint32_t targetFrameDuration100ns = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    uint32_t maxSampleDuration100ns = 0;
    std::size_t longVfrSamples = 0;
    uint64_t unfilledTargetIntervals = 0;
    for (const auto& sample : videoSamples) {
        maxSampleDuration100ns = std::max(maxSampleDuration100ns, sample.info.duration);
        const uint64_t roundedTicks =
            (static_cast<uint64_t>(sample.info.duration) + targetFrameDuration100ns / 2u) /
            targetFrameDuration100ns;
        const uint64_t missing = roundedTicks > 1 ? roundedTicks - 1 : 0;
        if (missing > 0) {
            ++longVfrSamples;
            unfilledTargetIntervals += missing;
        }
    }
    logMuxSaveTiming(
        "duration_plan",
        durationStartedAt,
        "videoSamples=" + std::to_string(videoSamples.size()) +
            " plannedDuration100ns=" + std::to_string(plannedVideoDuration100ns) +
            " presentationDuration100ns=" + std::to_string(movieDuration100ns) +
            " decoderPreroll100ns=" + std::to_string(videoMediaStart100ns) +
            " maxSampleDuration100ns=" + std::to_string(maxSampleDuration100ns) +
            " longVfrSamples=" + std::to_string(longVfrSamples) +
            " unfilledTargetIntervals=" + std::to_string(unfilledTargetIntervals));

    if (!pcmAudioTracks.empty()) {
        const auto aacStartedAt = SaveTimingClock::now();
        std::string audioError;
        for (const auto& pcmTrack : pcmAudioTracks) {
            const auto trackStartedAt = SaveTimingClock::now();
            auto existing = std::find_if(audioTracks.begin(), audioTracks.end(), [&](const AacAudioTrack& candidate) {
                return candidate.sourceId == pcmTrack.sourceId &&
                    candidate.sampleRate == pcmTrack.sampleRate &&
                    candidate.channels == pcmTrack.channels;
            });
            if (existing != audioTracks.end()) finalizeAudioTimeline(*existing);
            const auto missingPcm = selectUncoveredPcm(
                pcmTrack,
                existing == audioTracks.end() ? nullptr : &*existing);
            AacAudioTrack recoveredTrack;
            const bool encoded = missingPcm.samples.empty()
                ? true
                : encodePcmTrackToAacBatched(missingPcm, recoveredTrack, audioError);
            logMuxSaveTiming(
                "aac_encode_track",
                trackStartedAt,
                "ok=" + std::string(encoded ? "true" : "false") +
                    " source=\"" + pcmTrack.sourceId + "\"" +
                    " pcmSamples=" + std::to_string(pcmTrack.samples.size()) +
                    " repairSamples=" + std::to_string(missingPcm.samples.size()) +
                    " aacSamples=" + std::to_string(recoveredTrack.samples.size()));
            if (encoded && !recoveredTrack.samples.empty()) {
                if (existing == audioTracks.end()) {
                    audioTracks.push_back(std::move(recoveredTrack));
                } else {
                    existing->samples.insert(
                        existing->samples.end(),
                        std::make_move_iterator(recoveredTrack.samples.begin()),
                        std::make_move_iterator(recoveredTrack.samples.end()));
                }
            }
        }
        logMuxSaveTiming(
            "aac_encode",
            aacStartedAt,
            "pcmTracks=" + std::to_string(pcmAudioTracks.size()) +
                " aacTracks=" + std::to_string(audioTracks.size()));

        if (audioTracks.empty() && !pcmAudioTracks.empty()) {
            result.message = audioError.empty()
                ? "AAC audio encoding failed for every captured audio track."
                : audioError;
            logMuxSaveTiming("total", totalStartedAt, "ok=false reason=aac_encode_failed");
            return result;
        }
    }

    AudioContinuityStats audioContinuity;
    AudioClipAlignmentStats audioAlignment;
    std::string audioContinuityError;
    for (auto& track : audioTracks) {
        finalizeAudioTimeline(track);
        if (!makeAudioTimelineContinuous(track, audioContinuity, audioContinuityError)) {
            result.message = audioContinuityError.empty()
                ? "AAC timeline normalization failed."
                : audioContinuityError;
            logMuxSaveTiming("total", totalStartedAt, "ok=false reason=aac_timeline_normalization");
            return result;
        }
        alignAudioTrackToVideo(
            track,
            videoPresentationStartPts100ns,
            movieDuration100ns,
            audioAlignment);
    }
    logMuxSaveTiming(
        "audio_timeline",
        totalStartedAt,
        "tracks=" + std::to_string(audioTracks.size()) +
            " insertedSilenceSamples=" + std::to_string(audioContinuity.insertedSilenceSamples) +
            " removedOverlaps=" + std::to_string(audioContinuity.removedOverlappingSamples) +
            " maximumGap100ns=" + std::to_string(audioContinuity.maximumGap100ns) +
            " droppedLeadingSamples=" + std::to_string(audioAlignment.droppedLeadingSamples) +
            " droppedLeadingFrames=" + std::to_string(audioAlignment.droppedLeadingFrames) +
            " droppedTrailingSamples=" + std::to_string(audioAlignment.droppedTrailingSamples) +
            " maximumStartDelay100ns=" + std::to_string(audioAlignment.maximumStartDelay100ns));
    std::erase_if(audioTracks, [](const AacAudioTrack& track) { return track.samples.empty(); });
    if (pacing.experimentalInPlace) {
        // Match the host's common final stream order in metadata, avoiding an
        // entire FFmpeg remux just to move system audio before the microphone.
        const auto order = [](const auto& track) {
            return track.sourceId == "system-loopback-pcm" ? 0 : track.sourceId == "microphone-pcm" ? 1 : 2;
        };
        std::stable_sort(audioTracks.begin(), audioTracks.end(), [&](const auto& a, const auto& b) { return order(a) < order(b); });
    }

    videoSamples.front().info.keyframe = true;

    const auto metadataStartedAt = SaveTimingClock::now();
    const auto path = clipFilePath(saveFolder);
    const auto ftyp = makeFtyp();
    uint64_t mdatPayloadSize = 0;
    for (const auto& sample : videoSamples) mdatPayloadSize += sample.info.size;
    for (const auto& track : audioTracks) {
        for (const auto& sample : track.samples) mdatPayloadSize += sample.info.size;
    }

    std::optional<replay::PayloadLayoutPlan> alignedVideo;
    std::optional<replay::InPlaceMediaLayout> inPlace;
    if (pacing.experimentalInPlace) {
        // Place every track in one time-ordered pass. Samples already in the
        // recording keep their offsets; anything appended (a quick re-save's
        // leftover, the live tail) is then interleaved, so a player reads each
        // audio sample next to its video instead of seeking across the file.
        auto& recording = *pacing.experimentalInPlace;
        constexpr std::size_t kVideoTrack = SIZE_MAX;
        struct Placement { int64_t time100ns; std::size_t track; std::size_t index; };
        std::vector<Placement> order;
        order.reserve(videoSamples.size());
        for (std::size_t i = 0; i < videoSamples.size(); ++i) {
            order.push_back({videoSamples[i].packet->dts100ns, kVideoTrack, i});
        }
        for (std::size_t track = 0; track < audioTracks.size(); ++track) {
            for (std::size_t i = 0; i < audioTracks[track].samples.size(); ++i) {
                order.push_back({audioTracks[track].samples[i].pts100ns, track, i});
            }
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.time100ns < b.time100ns; });
        replay::InPlaceMediaLayout placed{recording.mediaStart(), 0, std::vector<uint64_t>(videoSamples.size())};
        for (const auto& item : order) {
            if (item.track == kVideoTrack) {
                const auto offset = recording.placeVideo(*videoSamples[item.index].packet);
                if (!offset) {
                    result.message = "In-place save could not place its video samples; buffered sources retained.";
                    return result; // Do not silently perform a copy or modify an unrelated file.
                }
                placed.sampleOffsets[item.index] = *offset;
                continue;
            }
            auto& sample = audioTracks[item.track].samples[item.index];
            const auto offset = recording.placeSample(sample.payloadReader, samplePayload(sample), sample.info.size);
            if (!offset) {
                result.message = "In-place save could not place an audio sample; buffered sources retained.";
                return result;
            }
            sample.info.fileOffset = *offset;
        }
        placed.mediaEnd = recording.mediaEnd();
        inPlace = std::move(placed);
    }
    if (!inPlace && pacing.experimentalPayloadAlignment && audioTracks.empty()) {
        std::vector<const EncodedPacket*> selected;
        selected.reserve(videoSamples.size());
        for (const auto& sample : videoSamples) selected.push_back(sample.packet);
        // Experimental layouts always use a 64-bit mdat header, so padding
        // cannot change header width after sample offsets have been planned.
        alignedVideo = mux::preparedVideoLayout(selected, ftyp.size() + 16, pacing.experimentalPayloadAlignment);
    }
    const bool largeMdat = alignedVideo.has_value() || mdatPayloadSize + 8 > 0xFFFFFFFFULL;
    const uint64_t mdatHeaderSize = largeMdat ? 16 : 8;
    uint64_t nextOffset = ftyp.size() + mdatHeaderSize;
    std::size_t videoIndex = 0;
    for (auto& sample : videoSamples) {
        if (alignedVideo) nextOffset = alignedVideo->sampleOffsets()[videoIndex++];
        if (inPlace) nextOffset = inPlace->sampleOffsets[videoIndex++];
        sample.info.fileOffset = nextOffset;
        nextOffset += sample.info.size;
    }
    for (auto& track : audioTracks) {
        for (auto& sample : track.samples) {
            if (inPlace) continue; // Placed above, interleaved with video.
            sample.info.fileOffset = nextOffset;
            nextOffset += sample.info.size;
        }
    }
    if (alignedVideo) {
        mdatPayloadSize = alignedVideo->outputEnd() - alignedVideo->outputStart();
        std::cerr << "[aligned-payload] alignment=" << pacing.experimentalPayloadAlignment
                  << " samples=" << videoSamples.size()
                  << " mediaBytes=" << alignedVideo->payloadBytes()
                  << " paddingBytes=" << alignedVideo->paddingBytes()
                  << " candidateBytes=" << alignedVideo->alignmentCandidateBytes() << '\n';
    }

    const auto moov = makeMoov(
        avcConfig,
        videoSamples,
        audioTracks,
        width,
        height,
        fps,
        movieDuration100ns,
        videoMediaStart100ns,
        videoPresentationStartPts100ns);
    if (inPlace) {
        inPlace->mediaEnd = pacing.experimentalInPlace->mediaEnd();
        const auto header = mux::inPlaceMp4Header(ftyp, inPlace->mediaStart, inPlace->mediaEnd);
        auto& recording = *pacing.experimentalInPlace;
        const auto before = inPlaceBefore;
        const bool finalized = recording.finalize(header, std::as_bytes(std::span(moov)));
        const bool published = finalized && recording.publish(path);
        const auto after = recording.io();
        result.ok = published;
        result.message = published ? "Finalized MP4 in place." : "In-place finalization/rename failed; private recording retained.";
        if (published) result.filePath = narrow(path);
        for (const auto& track : audioTracks) result.audioTracks.push_back(track.sourceId);
        result.ioAnalysis.enabled = pacing.analyzeIo;
        result.ioAnalysis.finalFileBytes = finalized ? inPlace->mediaEnd + moov.size() : 0;
        result.ioAnalysis.elapsedMs = saveTimingElapsedMs(totalStartedAt);
        std::cerr << "[in-place] ok=" << published << " mediaRead=" << after.bytesRead - before.bytesRead
                  << " mediaWritten=" << after.mediaWritten - before.mediaWritten
                  << " metadataWritten=" << after.metadataWritten - before.metadataWritten
                  << " mediaBytes=" << inPlace->mediaEnd - inPlace->mediaStart << '\n';
        return result;
    }
    logMuxSaveTiming(
        "metadata",
        metadataStartedAt,
        "mdatBytes=" + std::to_string(mdatPayloadSize) +
            " moovBytes=" + std::to_string(moov.size()) +
            " audioTracks=" + std::to_string(audioTracks.size()));

    SaveIoAnalysis ioAnalysis;
    SaveIoRecorder ioRecorder(pacing.analyzeIo, ioAnalysis, totalStartedAt);
    Win32FileWriter out(path);
    if (!out.valid()) {
        ioRecorder.finish();
        result.ioAnalysis = std::move(ioAnalysis);
        result.message = "MP4 muxing failed: could not create output file.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=create_output_failed");
        return result;
    }
    const uint64_t finalFileSize = ftyp.size() + mdatHeaderSize + mdatPayloadSize + moov.size();
    const auto preallocateStartedAt = SaveTimingClock::now();
    const bool preallocationAttempted = shouldPreallocateMuxOutput(out.storageSeekPenalty());
    const bool preallocated = preallocationAttempted && out.preallocate(finalFileSize);
    const auto preallocateDurationUs = static_cast<uint64_t>(std::max<int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::microseconds>(
            SaveTimingClock::now() - preallocateStartedAt).count()));
    ioRecorder.recordStructuralOperation(
        "preallocate",
        preallocateStartedAt,
        preallocateDurationUs,
        finalFileSize);
    logMuxSaveTiming(
        "preallocate",
        preallocateStartedAt,
        "attempted=" + std::string(preallocationAttempted ? "true" : "false") +
            " ok=" + std::string(preallocated ? "true" : "false") +
            " finalBytes=" + std::to_string(finalFileSize) +
            " durationUs=" + std::to_string(preallocateDurationUs));
    const bool adaptiveWritePacing = shouldUseAdaptiveWritePacing(
        pacing.storageAwareRate,
        out.storageSeekPenalty());
    const auto adaptiveRateConfig = seedWritePacerConfig(
        writePacerConfigForStorage(pacing.adaptiveRate, out.storageSeekPenalty()),
        pacing.learnedWriteBytesPerSecond);
    ioRecorder.setFileContext(
        storageSeekPenaltyName(out.storageSeekPenalty()),
        ioPriorityName(out.ioPriorityHint()),
        out.lowPriorityApplied(),
        preallocated,
        finalFileSize,
        diskBackedEncodedSourceBytes);

    const auto headerStartedAt = SaveTimingClock::now();
    writeBytes(out, ftyp);
    if (largeMdat) {
        writeU32(out, 1);
        writeType(out, "mdat");
        writeU64(out, mdatPayloadSize + 16);
    } else {
        writeU32(out, static_cast<uint32_t>(mdatPayloadSize + 8));
        writeType(out, "mdat");
    }
    ioRecorder.recordOutputWrite(
        headerStartedAt,
        static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                SaveTimingClock::now() - headerStartedAt).count())),
        out.good() ? ftyp.size() + mdatHeaderSize : 0,
        "write_header");
    const std::size_t saveStagingBytes = muxStagingBytesForSource(
        diskBackedEncodedSourceBytes > 0,
        out.storageSeekPenalty());
    logMuxSaveTiming(
        "write_header",
        headerStartedAt,
        "largeMdat=" + std::string(largeMdat ? "true" : "false") +
            " preallocated=" + std::string(preallocated ? "true" : "false") +
            " finalBytes=" + std::to_string(finalFileSize) +
            " lowIoPriority=" + std::string(out.lowPriorityApplied() ? "true" : "false") +
            " ioPriority=" + ioPriorityName(out.ioPriorityHint()) +
            " storageSeekPenalty=" + storageSeekPenaltyName(out.storageSeekPenalty()) +
            " stagingBytes=" + std::to_string(saveStagingBytes) +
            " adaptiveWritePacing=" + std::string(adaptiveWritePacing ? "true" : "false") +
            " aggregateIoPacing=" + std::string(adaptiveWritePacing ? "true" : "false") +
            " initialRateMiBps=" + std::to_string(
                adaptiveWritePacing
                    ? adaptiveRateConfig.initialBytesPerSecond / (1024ULL * 1024ULL)
                    : 0));

    BufferedByteWriter bufferedOut(
        out,
        saveStagingBytes,
        std::move(pacing.samplePressure),
        adaptiveWritePacing,
        adaptiveRateConfig,
        pacing.burstBytes,
        pacing.analyzeIo ? &ioRecorder : nullptr);
    const auto videoWriteStartedAt = SaveTimingClock::now();
    uint64_t videoWrittenBytes = 0;
    bool payloadReadSucceeded = true;
    uint64_t videoWriteOffset = ftyp.size() + mdatHeaderSize;
    for (const auto& sample : videoSamples) {
        if (sample.info.fileOffset > videoWriteOffset) {
            mux::writePadding(bufferedOut, sample.info.fileOffset - videoWriteOffset);
        }
        if (!writeAvccSample(bufferedOut, sample)) {
            payloadReadSucceeded = false;
            break;
        }
        videoWrittenBytes += sample.info.size;
        videoWriteOffset = sample.info.fileOffset + sample.info.size;
    }
    bufferedOut.flush();
    const auto videoFlushes = bufferedOut.flushCount();
    const auto videoSleeps = bufferedOut.sleepCount();
    const auto videoSleepMs = bufferedOut.sleepMs();
    const int64_t videoWriteMs = std::max<int64_t>(1, saveTimingElapsedMs(videoWriteStartedAt));
    const uint64_t videoThroughputMiBps =
        (videoWrittenBytes * 1000ULL) / (static_cast<uint64_t>(videoWriteMs) * 1024ULL * 1024ULL);
    logMuxSaveTiming(
        "write_video_mdat",
        videoWriteStartedAt,
        "samples=" + std::to_string(videoSamples.size()) +
            " bytes=" + std::to_string(videoWrittenBytes) +
            " throughputMiBps=" + std::to_string(videoThroughputMiBps) +
            " flushes=" + std::to_string(videoFlushes) +
            " maxWriteBytes=" + std::to_string(out.maximumWriteSize()) +
            " sourceReadBytes=" + std::to_string(bufferedOut.sourceReadBytes()) +
            " sourceReadCalls=" + std::to_string(bufferedOut.sourceReadCalls()) +
            " sourceReadMs=" + std::to_string(bufferedOut.sourceReadMs()) +
            " maxSourceReadUs=" + std::to_string(bufferedOut.maximumSourceReadUs()) +
            " outputWriteCalls=" + std::to_string(bufferedOut.outputWriteCalls()) +
            " outputWriteMs=" + std::to_string(bufferedOut.outputWriteMs()) +
            " maxOutputWriteUs=" + std::to_string(bufferedOut.maximumOutputWriteUs()) +
            " adaptiveBackgroundEntries=" + std::to_string(
                bufferedOut.adaptiveBackgroundEntries()) +
            " adaptiveBackgroundMs=" + std::to_string(bufferedOut.adaptiveBackgroundMs()) +
            " adaptiveRate=" + std::string(
                bufferedOut.adaptiveRateEnabled() ? "true" : "false") +
            " finalRateMiBps=" + std::to_string(
                bufferedOut.rateLimitBytesPerSecond() / (1024ULL * 1024ULL)) +
            " minimumRateMiBps=" + std::to_string(
                bufferedOut.minimumRateBytesPerSecond() / (1024ULL * 1024ULL)) +
            " measuredFloorMiBps=" + std::to_string(
                bufferedOut.dynamicMinimumRateBytesPerSecond() / (1024ULL * 1024ULL)) +
            " maximumRateMiBps=" + std::to_string(
                bufferedOut.maximumRateBytesPerSecond() / (1024ULL * 1024ULL)) +
            " observedWriteMiBps=" + std::to_string(
                bufferedOut.observedWriteBytesPerSecond() / (1024ULL * 1024ULL)) +
            " measuredWrites=" + std::to_string(bufferedOut.measuredWrites()) +
            " rateAdjustments=" + std::to_string(bufferedOut.rateAdjustments()) +
            " pressureBackoffs=" + std::to_string(bufferedOut.pressureBackoffs()) +
            " latencyBackoffs=" + std::to_string(bufferedOut.latencyBackoffs()) +
            " ignoredLatencyStalls=" + std::to_string(bufferedOut.ignoredLatencyStalls()) +
            " seededRateMiBps=" + std::to_string(pacing.learnedWriteBytesPerSecond / (1024ULL * 1024ULL)) +
            " rateLimitSleeps=" + std::to_string(bufferedOut.rateLimitSleepCount()) +
            " rateLimitSleepMs=" + std::to_string(bufferedOut.rateLimitSleepMs()) +
            " paceYields=" + std::to_string(bufferedOut.yieldCount()) +
            " paceSleeps=" + std::to_string(videoSleeps) +
            " paceSleepMs=" + std::to_string(videoSleepMs) +
            " recoveryWaits=" + std::to_string(bufferedOut.recoveryWaitCount()) +
            " recoveryWaitMs=" + std::to_string(bufferedOut.recoveryWaitMs()) +
            " pressureTransitions=" + std::to_string(bufferedOut.pressureTransitions()) +
            " maxQueueAge100ns=" + std::to_string(bufferedOut.maximumQueueAge100ns()) +
            " maxEncoderQueueDepth=" + std::to_string(bufferedOut.maximumEncoderQueueDepth()) +
            " maxNvencInFlight=" + std::to_string(bufferedOut.maximumNvencInFlight()) +
            " maxCaptureGap100ns=" + std::to_string(bufferedOut.maximumCaptureGap100ns()) +
            " maxCapturePublicationAge100ns=" + std::to_string(
                bufferedOut.maximumCapturePublicationAge100ns()));

    const auto audioWriteStartedAt = SaveTimingClock::now();
    const auto audioFlushesBefore = bufferedOut.flushCount();
    const auto audioSleepsBefore = bufferedOut.sleepCount();
    const auto audioSleepMsBefore = bufferedOut.sleepMs();
    const auto audioRateLimitSleepsBefore = bufferedOut.rateLimitSleepCount();
    const auto audioRateLimitSleepMsBefore = bufferedOut.rateLimitSleepMs();
    const auto audioSourceReadBytesBefore = bufferedOut.sourceReadBytes();
    const auto audioSourceReadCallsBefore = bufferedOut.sourceReadCalls();
    const auto audioSourceReadMsBefore = bufferedOut.sourceReadMs();
    const auto audioOutputWriteCallsBefore = bufferedOut.outputWriteCalls();
    const auto audioOutputWriteMsBefore = bufferedOut.outputWriteMs();
    uint64_t audioWrittenBytes = 0;
    for (const auto& track : audioTracks) {
        if (!payloadReadSucceeded) break;
        for (const auto& sample : track.samples) {
            if (!writeSamplePayload(bufferedOut, sample)) {
                payloadReadSucceeded = false;
                break;
            }
            audioWrittenBytes += sample.info.size;
        }
    }
    bufferedOut.flush();
    logMuxSaveTiming(
        "write_audio_mdat",
        audioWriteStartedAt,
        "tracks=" + std::to_string(audioTracks.size()) +
            " bytes=" + std::to_string(audioWrittenBytes) +
            " flushes=" + std::to_string(bufferedOut.flushCount() - audioFlushesBefore) +
            " sourceReadBytes=" + std::to_string(
                bufferedOut.sourceReadBytes() - audioSourceReadBytesBefore) +
            " sourceReadCalls=" + std::to_string(
                bufferedOut.sourceReadCalls() - audioSourceReadCallsBefore) +
            " sourceReadMs=" + std::to_string(
                bufferedOut.sourceReadMs() - audioSourceReadMsBefore) +
            " outputWriteCalls=" + std::to_string(
                bufferedOut.outputWriteCalls() - audioOutputWriteCallsBefore) +
            " outputWriteMs=" + std::to_string(
                bufferedOut.outputWriteMs() - audioOutputWriteMsBefore) +
            " rateLimitSleeps=" + std::to_string(
                bufferedOut.rateLimitSleepCount() - audioRateLimitSleepsBefore) +
            " rateLimitSleepMs=" + std::to_string(
                bufferedOut.rateLimitSleepMs() - audioRateLimitSleepMsBefore) +
            " paceSleeps=" + std::to_string(bufferedOut.sleepCount() - audioSleepsBefore) +
            " paceSleepMs=" + std::to_string(bufferedOut.sleepMs() - audioSleepMsBefore));

    const auto moovWriteStartedAt = SaveTimingClock::now();
    writeBytes(out, moov);
    ioRecorder.recordOutputWrite(
        moovWriteStartedAt,
        static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                SaveTimingClock::now() - moovWriteStartedAt).count())),
        out.good() ? moov.size() : 0,
        "write_moov");
    logMuxSaveTiming("write_moov", moovWriteStartedAt, "bytes=" + std::to_string(moov.size()));

    const auto closeStartedAt = SaveTimingClock::now();
    const bool writeSucceeded = payloadReadSucceeded && out.good();
    const bool closeSucceeded = out.close();
    ioRecorder.recordStructuralOperation(
        "file_close",
        closeStartedAt,
        static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                SaveTimingClock::now() - closeStartedAt).count())));
    ioRecorder.finish();
    result.ioAnalysis = std::move(ioAnalysis);
    logMuxSaveTiming("file_close", closeStartedAt);

    if (!writeSucceeded || !closeSucceeded) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        result.message = payloadReadSucceeded
            ? "MP4 muxing failed while writing the output file."
            : "MP4 muxing failed while reading the replay archive.";
        logMuxSaveTiming(
            "total",
            totalStartedAt,
            std::string("ok=false reason=") + (payloadReadSucceeded ? "write_failed" : "archive_read_failed"));
        return result;
    }

    result.ok = true;
    result.filePath = narrow(path);
    for (const auto& track : audioTracks) result.audioTracks.push_back(track.sourceId);
    result.message = "Saved MP4 clip.";
    if (bufferedOut.adaptiveRateEnabled() && bufferedOut.measuredWrites() > 0) {
        result.learnedWriteBytesPerSecond = bufferedOut.rateLimitBytesPerSecond();
    }
    logMuxSaveTiming(
        "total",
        totalStartedAt,
        "ok=true videoBytes=" + std::to_string(videoWrittenBytes) +
            " audioBytes=" + std::to_string(audioWrittenBytes) +
            " fileBytes=" + std::to_string(out.bytesWritten()) +
            " maxWriteBytes=" + std::to_string(out.maximumWriteSize()) +
            " path=\"" + result.filePath + "\"");
    return result;
}

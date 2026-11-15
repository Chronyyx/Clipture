// Part of Mp4Muxer.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.

using Bytes = std::vector<uint8_t>;

struct SampleInfo {
    std::size_t size = 0;
    uint64_t fileOffset = 0;
    bool keyframe = false;
    uint32_t duration = 1;
};

struct OwnedSample {
    std::vector<std::byte> payload;
    PacketPayloadPtr sharedPayload;
    PacketPayloadReaderPtr payloadReader;
    SampleInfo info;
    int64_t pts100ns = 0;
    uint32_t encoderEpoch = 0;
    int32_t primingFrames = 0;
    uint32_t encodedFrameCount = 1024;
};

std::span<const std::byte> samplePayload(const OwnedSample& sample) {
    if (sample.sharedPayload) return { sample.sharedPayload->data(), sample.sharedPayload->size() };
    return { sample.payload.data(), sample.payload.size() };
}

struct PcmSampleView {
    PacketPayloadPtr sharedPayload;
    PacketPayloadReaderPtr payloadReader;
    std::size_t payloadSize = 0;
    int64_t pts100ns = 0;
    uint32_t durationFrames = 1;
};

struct PcmAudioTrack {
    std::string sourceId;
    int sampleRate = 48000;
    int channels = 2;
    std::vector<PcmSampleView> samples;
};

struct AacAudioTrack {
    std::string sourceId;
    int sampleRate = 48000;
    int channels = 2;
    Bytes decoderConfig;
    std::vector<OwnedSample> samples;
    int64_t firstPts100ns = 0;
    int32_t primingFrames = 0;
};

using SaveTimingClock = std::chrono::steady_clock;
constexpr uint64_t kMp4Version0MaxDuration = 0xFFFFFFFFULL;

int64_t saveTimingElapsedMs(SaveTimingClock::time_point startedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SaveTimingClock::now() - startedAt).count();
}

void logMuxSaveTiming(const char* stage, SaveTimingClock::time_point startedAt, const std::string& details = {}) {
    std::cerr << "[save-timing] source=mux stage=" << stage
              << " ms=" << saveTimingElapsedMs(startedAt);
    if (!details.empty()) std::cerr << " " << details;
    std::cerr << std::endl;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring result(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring defaultSaveFolder() {
    wchar_t path[MAX_PATH] {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return std::wstring(path) + L"\\Clipture";
    }
    return L".\\Clipture";
}

std::wstring clipFilePath(const std::string& saveFolder) {
    std::wstring folder = widen(saveFolder);
    if (folder.empty()) folder = defaultSaveFolder();
    std::filesystem::create_directories(folder);

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime {};
    localtime_s(&localTime, &time);
    const int hour12 = localTime.tm_hour % 12 == 0 ? 12 : localTime.tm_hour % 12;
    const wchar_t* period = localTime.tm_hour < 12 ? L"AM" : L"PM";

    std::wostringstream path;
    path << folder << L"\\Clipture "
         << std::put_time(&localTime, L"%Y-%m-%d ")
         << std::setw(2) << std::setfill(L'0') << hour12 << L"-"
         << std::setw(2) << std::setfill(L'0') << localTime.tm_min << L"-"
         << std::setw(2) << std::setfill(L'0') << localTime.tm_sec << L" "
         << period << L".mp4";
    const std::filesystem::path original(path.str());
    auto candidate = original;
    for (uint64_t suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = original.parent_path() /
            (original.stem().wstring() + L" (" + std::to_wstring(suffix) + L").mp4");
    }
    return candidate.wstring();
}

bool isVideoPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Video && !payloadEmpty(packet);
}

bool isPcmAudioPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Audio &&
        packet.encoderId == "PCM_S16" &&
        packet.sampleRate > 0 &&
        packet.channelCount > 0 &&
        packet.bitsPerSample == 16 &&
        !payloadEmpty(packet);
}

bool isAacAudioPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Audio &&
        packet.codec == PacketCodec::AacLc &&
        packet.sampleRate > 0 &&
        packet.channelCount > 0 &&
        packet.audioFrameCount > 0 &&
        !payloadEmpty(packet);
}

const std::string& packetTrackId(const EncodedPacket& packet) {
    return packet.logicalTrackId.empty() ? packet.sourceId : packet.logicalTrackId;
}

int aacSampleRateIndex(int sampleRate) {
    switch (sampleRate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000: return 11;
        case 7350: return 12;
        default: return 3;
    }
}

Bytes makeAacAudioSpecificConfig(int sampleRate, int channels) {
    const int objectType = 2; // AAC LC
    const int frequencyIndex = aacSampleRateIndex(sampleRate);
    const int channelConfig = std::clamp(channels, 1, 2);
    const uint16_t bits = static_cast<uint16_t>((objectType << 11) | (frequencyIndex << 7) | (channelConfig << 3));
    return {
        static_cast<uint8_t>((bits >> 8) & 0xFF),
        static_cast<uint8_t>(bits & 0xFF)
    };
}

std::string hresultMessage(const std::string& prefix, HRESULT hr) {
    std::ostringstream out;
    out << prefix << " HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ".";
    return out.str();
}

void appendU8(Bytes& out, uint8_t value) {
    out.push_back(value);
}

void appendU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU24(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU64(Bytes& out, uint64_t value) {
    appendU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    appendU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

void appendType(Bytes& out, const char type[4]) {
    out.insert(out.end(), type, type + 4);
}

void appendBytes(Bytes& out, const Bytes& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendBytes(Bytes& out, const std::vector<std::byte>& bytes) {
    for (const auto byte : bytes) out.push_back(std::to_integer<uint8_t>(byte));
}

Bytes box(const char type[4], const Bytes& payload) {
    Bytes out;
    appendU32(out, static_cast<uint32_t>(payload.size() + 8));
    appendType(out, type);
    appendBytes(out, payload);
    return out;
}

Bytes fullBox(const char type[4], uint8_t version, uint32_t flags, const Bytes& payload) {
    Bytes full;
    appendU8(full, version);
    appendU24(full, flags);
    appendBytes(full, payload);
    return box(type, full);
}

PcmAudioTrack selectUncoveredPcm(const PcmAudioTrack& pcmTrack, const AacAudioTrack* encodedTrack) {
    if (!encodedTrack || encodedTrack->samples.empty()) return pcmTrack;

    struct Coverage {
        int64_t start = 0;
        int64_t end = 0;
    };
    std::vector<Coverage> coverage;
    coverage.reserve(encodedTrack->samples.size());
    for (const auto& sample : encodedTrack->samples) {
        const int64_t duration =
            (static_cast<int64_t>(std::max<uint32_t>(1, sample.encodedFrameCount)) * 10'000'000LL) /
            std::max(1, encodedTrack->sampleRate);
        Coverage next { sample.pts100ns, sample.pts100ns + duration };
        if (!coverage.empty() && next.start <= coverage.back().end + 50'000LL) {
            coverage.back().end = std::max(coverage.back().end, next.end);
        } else {
            coverage.push_back(next);
        }
    }

    PcmAudioTrack missing { pcmTrack.sourceId, pcmTrack.sampleRate, pcmTrack.channels, {} };
    std::size_t coverageIndex = 0;
    for (const auto& sample : pcmTrack.samples) {
        const int64_t sampleEnd = sample.pts100ns +
            (static_cast<int64_t>(sample.durationFrames) * 10'000'000LL) / std::max(1, pcmTrack.sampleRate);
        while (coverageIndex < coverage.size() && coverage[coverageIndex].end < sample.pts100ns) {
            ++coverageIndex;
        }
        const bool covered = coverageIndex < coverage.size() &&
            coverage[coverageIndex].start <= sample.pts100ns + 10'000LL &&
            coverage[coverageIndex].end + 10'000LL >= sampleEnd;
        if (!covered) missing.samples.push_back(sample);
    }
    return missing;
}

void finalizeAudioTimeline(AacAudioTrack& track) {
    std::sort(track.samples.begin(), track.samples.end(), [](const OwnedSample& left, const OwnedSample& right) {
        if (left.pts100ns != right.pts100ns) return left.pts100ns < right.pts100ns;
        return left.encoderEpoch < right.encoderEpoch;
    });
    track.samples.erase(
        std::unique(track.samples.begin(), track.samples.end(), [](const OwnedSample& left, const OwnedSample& right) {
            return std::abs(left.pts100ns - right.pts100ns) <= 10'000LL;
        }),
        track.samples.end());
    if (track.samples.empty()) return;

    track.firstPts100ns = track.samples.front().pts100ns;
    track.primingFrames = std::max<int32_t>(0, track.samples.front().primingFrames);
    for (auto& sample : track.samples) {
        sample.info.duration = std::max<uint32_t>(1, sample.encodedFrameCount);
    }
}

bool encodePcmTrackToAacBatched(const PcmAudioTrack& pcmTrack, AacAudioTrack& aacTrack, std::string& error) {
    if (pcmTrack.samples.empty()) return false;

    AacEncoderSession encoder;
    const int sampleRate = std::max(8000, pcmTrack.sampleRate);
    const int channels = std::clamp(pcmTrack.channels, 1, 2);
    const uint32_t targetBatchFrames = static_cast<uint32_t>(std::max(1, sampleRate / 10));
    const std::size_t bytesPerFrame = static_cast<std::size_t>(channels) * sizeof(int16_t);
    std::vector<std::byte> batch;
    batch.reserve(static_cast<std::size_t>(targetBatchFrames) * bytesPerFrame);
    uint32_t batchFrames = 0;
    int64_t batchPts100ns = 0;
    int64_t expectedInputPts100ns = 0;
    uint32_t epoch = 1;
    bool epochActive = false;
    std::vector<AacEncodedFrame> encodedFrames;

    auto appendEncoded = [&]() {
        for (auto& frame : encodedFrames) {
            OwnedSample sample;
            sample.payload = std::move(frame.payload);
            sample.info.size = sample.payload.size();
            sample.info.duration = frame.durationFrames;
            sample.pts100ns = frame.pts100ns;
            sample.encoderEpoch = epoch;
            sample.primingFrames = frame.primingFrames;
            sample.encodedFrameCount = frame.durationFrames;
            aacTrack.samples.push_back(std::move(sample));
        }
        encodedFrames.clear();
    };
    auto startEpoch = [&]() {
        if (epochActive) return true;
        if (!encoder.start(sampleRate, channels, error)) return false;
        epochActive = true;
        return true;
    };
    auto flushBatch = [&]() {
        if (batchFrames == 0) return true;
        const bool ok = encoder.encode(batch, batchPts100ns, batchFrames, encodedFrames, error);
        if (ok) appendEncoded();
        batch.clear();
        batchFrames = 0;
        return ok;
    };
    auto finishEpoch = [&]() {
        if (!epochActive) return true;
        if (!flushBatch()) return false;
        if (!encoder.finish(encodedFrames, error)) return false;
        appendEncoded();
        encoder.reset();
        epochActive = false;
        expectedInputPts100ns = 0;
        ++epoch;
        return true;
    };

    for (const auto& pcmSample : pcmTrack.samples) {
        const uint32_t availableFrames = static_cast<uint32_t>(pcmSample.payloadSize / bytesPerFrame);
        if (availableFrames == 0) continue;
        constexpr int64_t kContiguousTolerance100ns = 50'000LL;
        if (epochActive && std::abs(pcmSample.pts100ns - expectedInputPts100ns) > kContiguousTolerance100ns) {
            if (!finishEpoch()) return false;
        }
        if (!startEpoch()) return false;
        uint32_t consumedFrames = 0;
        while (consumedFrames < availableFrames) {
            if (batchFrames == 0) {
                batchPts100ns = pcmSample.pts100ns +
                    (static_cast<int64_t>(consumedFrames) * 10'000'000LL) / sampleRate;
            }
            const uint32_t copiedFrames = std::min(targetBatchFrames - batchFrames, availableFrames - consumedFrames);
            const auto byteOffset = static_cast<std::size_t>(consumedFrames) * bytesPerFrame;
            const auto byteCount = static_cast<std::size_t>(copiedFrames) * bytesPerFrame;
            const std::size_t batchOffset = batch.size();
            batch.resize(batchOffset + byteCount);
            auto destination = std::span<std::byte>(batch).subspan(batchOffset, byteCount);
            bool copied = false;
            if (pcmSample.sharedPayload && byteOffset + byteCount <= pcmSample.sharedPayload->size()) {
                std::memcpy(destination.data(), pcmSample.sharedPayload->data() + byteOffset, byteCount);
                copied = true;
            } else if (pcmSample.payloadReader) {
                copied = pcmSample.payloadReader->read(byteOffset, destination);
            }
            if (!copied) {
                error = "PCM replay payload could not be read during AAC recovery.";
                return false;
            }
            batchFrames += copiedFrames;
            consumedFrames += copiedFrames;
            if (batchFrames == targetBatchFrames && !flushBatch()) return false;
        }
        expectedInputPts100ns = pcmSample.pts100ns +
            (static_cast<int64_t>(availableFrames) * 10'000'000LL) / sampleRate;
    }
    if (!finishEpoch()) return false;

    aacTrack.sourceId = pcmTrack.sourceId;
    aacTrack.sampleRate = sampleRate;
    aacTrack.channels = channels;
    aacTrack.decoderConfig = makeAacAudioSpecificConfig(sampleRate, channels);
    if (!aacTrack.samples.empty()) aacTrack.firstPts100ns = aacTrack.samples.front().pts100ns;
    return !aacTrack.samples.empty();
}

struct AudioContinuityStats {
    std::size_t insertedSilenceSamples = 0;
    std::size_t removedOverlappingSamples = 0;
    int64_t maximumGap100ns = 0;
};

struct AudioClipAlignmentStats {
    std::size_t droppedLeadingSamples = 0;
    std::size_t droppedTrailingSamples = 0;
    uint64_t droppedLeadingFrames = 0;
    int64_t maximumStartDelay100ns = 0;
};

struct ReusableSilenceSample {
    PacketPayloadPtr payload;
    uint32_t frameCount = 1024;
};

bool makeReusableAacSilence(
    int sampleRate,
    int channels,
    std::vector<ReusableSilenceSample>& silence,
    std::string& error) {
    AacEncoderSession encoder;
    if (!encoder.start(sampleRate, channels, error)) return false;

    const uint32_t inputFrames = static_cast<uint32_t>(std::max(sampleRate, 8'000));
    const std::size_t sampleCount = static_cast<std::size_t>(inputFrames) * std::max(1, channels);
    std::vector<std::byte> zeroPcm(sampleCount * sizeof(int16_t), std::byte { 0 });
    std::vector<AacEncodedFrame> encoded;
    if (!encoder.encode(zeroPcm, 1, inputFrames, encoded, error)) return false;
    if (!encoder.finish(encoded, error)) return false;
    if (encoded.empty()) {
        error = "AAC encoder produced no reusable silence samples.";
        return false;
    }

    const std::size_t first = encoded.size() > 10 ? 4 : 0;
    const std::size_t last = std::min(encoded.size(), first + 16);
    silence.reserve(last - first);
    for (std::size_t index = first; index < last; ++index) {
        auto& frame = encoded[index];
        if (frame.payload.empty()) continue;
        silence.push_back({
            std::make_shared<PacketPayload>(std::move(frame.payload)),
            std::max<uint32_t>(1, frame.durationFrames)
        });
    }
    if (silence.empty()) {
        error = "AAC encoder produced no usable silence samples.";
        return false;
    }
    return true;
}

bool makeAudioTimelineContinuous(
    AacAudioTrack& track,
    AudioContinuityStats& stats,
    std::string& error) {
    if (track.samples.empty()) return true;

    const int sampleRate = std::max(1, track.sampleRate);
    constexpr int64_t kTolerance100ns = 50'000LL;
    auto sampleDuration100ns = [sampleRate](const OwnedSample& sample) {
        return (static_cast<int64_t>(std::max<uint32_t>(1, sample.encodedFrameCount)) * 10'000'000LL) /
            sampleRate;
    };

    std::vector<OwnedSample> continuous;
    continuous.reserve(track.samples.size());
    std::vector<ReusableSilenceSample> reusableSilence;
    std::size_t nextSilence = 0;
    const uint32_t timelineEpoch = std::max<uint32_t>(1, track.samples.front().encoderEpoch);

    auto appendSample = [&](OwnedSample sample, int64_t pts100ns, bool preservePriming) {
        sample.pts100ns = pts100ns;
        sample.encoderEpoch = timelineEpoch;
        if (!preservePriming) sample.primingFrames = 0;
        sample.info.duration = std::max<uint32_t>(1, sample.encodedFrameCount);
        continuous.push_back(std::move(sample));
    };

    const int64_t firstPts100ns = track.samples.front().pts100ns;
    appendSample(std::move(track.samples.front()), firstPts100ns, true);
    int64_t expectedPts100ns = continuous.front().pts100ns + sampleDuration100ns(continuous.front());
    for (std::size_t index = 1; index < track.samples.size(); ++index) {
        auto sample = std::move(track.samples[index]);
        const int64_t duration100ns = sampleDuration100ns(sample);
        const int64_t sampleEnd100ns = sample.pts100ns + duration100ns;
        if (sample.pts100ns < expectedPts100ns - kTolerance100ns ||
            sampleEnd100ns <= expectedPts100ns + kTolerance100ns) {
            ++stats.removedOverlappingSamples;
            continue;
        }

        const int64_t gap100ns = sample.pts100ns - expectedPts100ns;
        stats.maximumGap100ns = std::max(stats.maximumGap100ns, gap100ns);
        if (gap100ns > kTolerance100ns) {
            if (reusableSilence.empty() &&
                !makeReusableAacSilence(sampleRate, track.channels, reusableSilence, error)) {
                return false;
            }
            const uint32_t silenceFrameCount = reusableSilence.front().frameCount;
            const int64_t silenceDuration100ns =
                (static_cast<int64_t>(silenceFrameCount) * 10'000'000LL) / sampleRate;
            const int64_t silenceSamples = std::max<int64_t>(
                0,
                (gap100ns + silenceDuration100ns / 2) / std::max<int64_t>(1, silenceDuration100ns));
            for (int64_t silenceIndex = 0; silenceIndex < silenceSamples; ++silenceIndex) {
                const auto& reusable = reusableSilence[nextSilence++ % reusableSilence.size()];
                OwnedSample silent;
                silent.sharedPayload = reusable.payload;
                silent.info.size = reusable.payload ? reusable.payload->size() : 0;
                silent.info.duration = reusable.frameCount;
                silent.encodedFrameCount = reusable.frameCount;
                appendSample(std::move(silent), expectedPts100ns, false);
                expectedPts100ns += sampleDuration100ns(continuous.back());
                ++stats.insertedSilenceSamples;
            }
        }

        appendSample(std::move(sample), expectedPts100ns, false);
        expectedPts100ns += sampleDuration100ns(continuous.back());
    }

    track.samples = std::move(continuous);
    track.firstPts100ns = track.samples.front().pts100ns;
    track.primingFrames = std::max<int32_t>(0, track.samples.front().primingFrames);
    int64_t verifiedNextPts100ns = track.samples.front().pts100ns;
    for (const auto& sample : track.samples) {
        if (sample.encoderEpoch != timelineEpoch ||
            std::abs(sample.pts100ns - verifiedNextPts100ns) > kTolerance100ns) {
            error = "AAC timeline remained discontinuous after normalization.";
            return false;
        }
        verifiedNextPts100ns = sample.pts100ns + sampleDuration100ns(sample);
    }
    return true;
}

void alignAudioTrackToVideo(
    AacAudioTrack& track,
    int64_t videoStartPts100ns,
    uint64_t movieDuration100ns,
    AudioClipAlignmentStats& stats) {
    if (track.samples.empty()) return;

    const int sampleRate = std::max(1, track.sampleRate);
    const int64_t originalStartPts100ns = track.samples.front().pts100ns;
    const int64_t leadingDuration100ns = std::max<int64_t>(
        0,
        videoStartPts100ns - originalStartPts100ns);
    const uint64_t leadingFrames =
        (static_cast<uint64_t>(leadingDuration100ns) * static_cast<uint64_t>(sampleRate) + 9'999'999ULL) /
        10'000'000ULL;
    const uint64_t framesToDiscard = leadingFrames + static_cast<uint64_t>(
        std::max<int32_t>(0, track.samples.front().primingFrames));

    uint64_t discardedFrames = 0;
    std::size_t discardedSamples = 0;
    while (discardedSamples < track.samples.size() && discardedFrames < framesToDiscard) {
        discardedFrames += std::max<uint32_t>(
            1,
            track.samples[discardedSamples].encodedFrameCount);
        ++discardedSamples;
    }
    if (discardedSamples > 0) {
        track.samples.erase(
            track.samples.begin(),
            track.samples.begin() + static_cast<std::ptrdiff_t>(discardedSamples));
        stats.droppedLeadingSamples += discardedSamples;
        stats.droppedLeadingFrames += discardedFrames;
    }
    if (track.samples.empty()) return;

    // MP4 edit lists that seek into the first AAC sample expose the discarded
    // packets with negative timestamps. Chromium rejects those files outright.
    // Starting on the next complete AAC frame costs at most one frame of silence
    // (about 21 ms at 48 kHz) and keeps every demuxed timestamp non-negative.
    track.samples.front().primingFrames = 0;
    track.primingFrames = 0;
    track.samples.front().pts100ns = std::max(
        videoStartPts100ns,
        track.samples.front().pts100ns);

    const int64_t maximumMovieDuration = static_cast<int64_t>(
        std::min<uint64_t>(movieDuration100ns, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    const int64_t videoEndPts100ns = videoStartPts100ns > std::numeric_limits<int64_t>::max() - maximumMovieDuration
        ? std::numeric_limits<int64_t>::max()
        : videoStartPts100ns + maximumMovieDuration;
    const auto trailing = std::lower_bound(
        track.samples.begin(),
        track.samples.end(),
        videoEndPts100ns,
        [](const OwnedSample& sample, int64_t endPts100ns) {
            return sample.pts100ns < endPts100ns;
        });
    stats.droppedTrailingSamples += static_cast<std::size_t>(
        std::distance(trailing, track.samples.end()));
    track.samples.erase(trailing, track.samples.end());
    if (track.samples.empty()) return;

    track.firstPts100ns = track.samples.front().pts100ns;
    stats.maximumStartDelay100ns = std::max(
        stats.maximumStartDelay100ns,
        std::max<int64_t>(0, track.firstPts100ns - videoStartPts100ns));
}


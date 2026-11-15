// Part of Mp4Muxer.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
bool startCodeAt(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    if (offset + 3 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{1}) {
        size = 3;
        return true;
    }
    if (offset + 4 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{0} &&
        data[offset + 3] == std::byte{1}) {
        size = 4;
        return true;
    }
    return false;
}

std::size_t findStartCode(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (startCodeAt(data, i, size)) return i;
    }
    return std::string::npos;
}

using NalUnit = H264NalSpan;

struct VideoSamplePlan {
    const EncodedPacket* packet = nullptr;
    std::vector<NalUnit> writableNalus;
    SampleInfo info;
};

std::vector<NalUnit> parseAnnexBNalus(std::span<const std::byte> data) {
    std::vector<NalUnit> nalus;
    std::size_t offset = 0;

    while (offset < data.size()) {
        std::size_t startCodeSize = 0;
        const std::size_t start = findStartCode(data, offset, startCodeSize);
        if (start == std::string::npos) break;

        const std::size_t nalStart = start + startCodeSize;
        if (nalStart >= data.size()) break;

        std::size_t nextStartCodeSize = 0;
        const std::size_t next = findStartCode(data, nalStart, nextStartCodeSize);
        const std::size_t nalEnd = next == std::string::npos ? data.size() : next;
        if (nalEnd > nalStart) {
            const auto header = std::to_integer<uint8_t>(data[nalStart]);
            nalus.push_back({
                static_cast<uint32_t>(nalStart),
                static_cast<uint32_t>(nalEnd - nalStart),
                static_cast<uint8_t>(header & 0x1F)
            });
        }
        offset = nalEnd;
    }

    return nalus;
}

std::vector<std::byte> copyNaluPayload(const EncodedPacket& packet, const NalUnit& nalu) {
    std::vector<std::byte> result;
    if (!copyPayloadRange(packet, nalu.offset, nalu.size, result)) result.clear();
    return result;
}

Bytes buildAvcDecoderConfig(const std::vector<std::byte>& sps, const std::vector<std::byte>& pps) {
    if (sps.size() < 4 || pps.empty() || sps.size() > 0xFFFF || pps.size() > 0xFFFF) return {};

    Bytes config;
    config.reserve(11 + sps.size() + pps.size());
    appendU8(config, 1);
    appendU8(config, std::to_integer<uint8_t>(sps[1]));
    appendU8(config, std::to_integer<uint8_t>(sps[2]));
    appendU8(config, std::to_integer<uint8_t>(sps[3]));
    appendU8(config, 0xFF);
    appendU8(config, 0xE1);
    appendU16(config, static_cast<uint16_t>(sps.size()));
    appendBytes(config, sps);
    appendU8(config, 1);
    appendU16(config, static_cast<uint16_t>(pps.size()));
    appendBytes(config, pps);
    return config;
}

bool isWritableVideoNalu(const NalUnit& nalu) {
    return nalu.type != 7 &&
        nalu.type != 8 &&
        nalu.type != 9 &&
        nalu.size > 0 &&
        nalu.size <= 0xFFFFFFFFULL;
}

std::size_t avccSampleSize(std::span<const NalUnit> nalus) {
    std::size_t size = 0;
    for (const auto& nalu : nalus) {
        size += sizeof(uint32_t) + nalu.size;
    }
    return size;
}

Bytes makeFtyp() {
    Bytes payload;
    appendType(payload, "isom");
    appendU32(payload, 0x00000200);
    appendType(payload, "isom");
    appendType(payload, "iso2");
    appendType(payload, "avc1");
    appendType(payload, "mp41");
    appendType(payload, "qt  ");
    return box("ftyp", payload);
}

Bytes makeMvhd(uint32_t timescale, uint64_t duration, uint32_t nextTrackId) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, timescale);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, timescale);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0x00010000);
    appendU16(payload, 0x0100);
    appendU16(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    for (int i = 0; i < 6; ++i) appendU32(payload, 0);
    appendU32(payload, nextTrackId);
    return fullBox("mvhd", version1 ? 1 : 0, 0, payload);
}

Bytes makeTkhd(uint64_t duration, int width, int height) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, 1);
        appendU32(payload, 0);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, 1);
        appendU32(payload, 0);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    appendU32(payload, static_cast<uint32_t>(std::max(1, width)) << 16);
    appendU32(payload, static_cast<uint32_t>(std::max(1, height)) << 16);
    return fullBox("tkhd", version1 ? 1 : 0, 0x000007, payload);
}

Bytes makeAudioTkhd(uint32_t trackId, uint64_t duration) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, trackId);
        appendU32(payload, 0);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, trackId);
        appendU32(payload, 0);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0x0100);
    appendU16(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    return fullBox("tkhd", version1 ? 1 : 0, 0x000007, payload);
}

Bytes makeAudioEdts(
    const AacAudioTrack& track,
    uint64_t movieDuration100ns,
    int64_t videoStartPts100ns) {
    if (track.samples.empty() || movieDuration100ns == 0) return {};
    const int sampleRate = std::max(1, track.sampleRate);
    struct EditEntry {
        uint64_t duration100ns = 0;
        int64_t mediaTimeFrames = -1;
    };
    std::vector<EditEntry> edits;

    uint64_t mediaDurationFrames = 0;
    for (const auto& sample : track.samples) {
        mediaDurationFrames += std::max<uint32_t>(1, sample.encodedFrameCount);
    }
    const int64_t relativeStart100ns = std::max<int64_t>(
        0,
        track.samples.front().pts100ns - videoStartPts100ns);
    const uint64_t presentationStart100ns = std::min<uint64_t>(
        static_cast<uint64_t>(relativeStart100ns),
        movieDuration100ns);
    if (presentationStart100ns > 0) edits.push_back({ presentationStart100ns, -1 });

    uint64_t presentationCursor100ns = presentationStart100ns;
    if (mediaDurationFrames > 0 && presentationCursor100ns < movieDuration100ns) {
        const uint64_t playableDuration100ns = std::min(
            (mediaDurationFrames * 10'000'000ULL) / static_cast<uint64_t>(sampleRate),
            movieDuration100ns - presentationCursor100ns);
        if (playableDuration100ns > 0) {
            edits.push_back({ playableDuration100ns, 0 });
            presentationCursor100ns += playableDuration100ns;
        }
    }
    if (presentationCursor100ns < movieDuration100ns) {
        edits.push_back({ movieDuration100ns - presentationCursor100ns, -1 });
    }
    if (edits.empty()) return {};

    Bytes entries;
    appendU32(entries, static_cast<uint32_t>(edits.size()));
    for (const auto& edit : edits) {
        appendU64(entries, edit.duration100ns);
        appendU64(entries, static_cast<uint64_t>(edit.mediaTimeFrames));
        appendU16(entries, 1);
        appendU16(entries, 0);
    }
    return box("edts", fullBox("elst", 1, 0, entries));
}

Bytes makeVideoEdts(uint64_t movieDuration100ns, uint64_t mediaStart100ns) {
    if (movieDuration100ns == 0) return {};
    Bytes entries;
    appendU32(entries, 1);
    appendU64(entries, movieDuration100ns);
    appendU64(entries, mediaStart100ns);
    appendU16(entries, 1);
    appendU16(entries, 0);
    return box("edts", fullBox("elst", 1, 0, entries));
}

Bytes makeMdhd(uint32_t timescale, uint64_t duration) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, timescale);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, timescale);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU16(payload, 0x55C4);
    appendU16(payload, 0);
    return fullBox("mdhd", version1 ? 1 : 0, 0, payload);
}

Bytes makeHdlr() {
    Bytes payload;
    appendU32(payload, 0);
    appendType(payload, "vide");
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    const char name[] = "VideoHandler";
    payload.insert(payload.end(), name, name + sizeof(name));
    return fullBox("hdlr", 0, 0, payload);
}

Bytes makeAudioHdlr() {
    Bytes payload;
    appendU32(payload, 0);
    appendType(payload, "soun");
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    const char name[] = "SoundHandler";
    payload.insert(payload.end(), name, name + sizeof(name));
    return fullBox("hdlr", 0, 0, payload);
}

Bytes makeVmhd() {
    Bytes payload;
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    return fullBox("vmhd", 0, 1, payload);
}

Bytes makeSmhd() {
    Bytes payload;
    appendU16(payload, 0);
    appendU16(payload, 0);
    return fullBox("smhd", 0, 0, payload);
}

Bytes makeDinf() {
    Bytes url = fullBox("url ", 0, 1, {});
    Bytes drefPayload;
    appendU32(drefPayload, 1);
    appendBytes(drefPayload, url);
    return box("dinf", fullBox("dref", 0, 0, drefPayload));
}

Bytes makeStsd(const Bytes& avcConfig, int width, int height) {
    Bytes avc1;
    for (int i = 0; i < 6; ++i) appendU8(avc1, 0);
    appendU16(avc1, 1);
    appendU16(avc1, 0);
    appendU16(avc1, 0);
    appendU32(avc1, 0);
    appendU32(avc1, 0);
    appendU32(avc1, 0);
    appendU16(avc1, static_cast<uint16_t>(std::max(1, width)));
    appendU16(avc1, static_cast<uint16_t>(std::max(1, height)));
    appendU32(avc1, 0x00480000);
    appendU32(avc1, 0x00480000);
    appendU32(avc1, 0);
    appendU16(avc1, 1);
    appendU8(avc1, 0);
    for (int i = 0; i < 31; ++i) appendU8(avc1, 0);
    appendU16(avc1, 0x0018);
    appendU16(avc1, 0xFFFF);
    appendBytes(avc1, box("avcC", avcConfig));

    Bytes payload;
    appendU32(payload, 1);
    appendBytes(payload, box("avc1", avc1));
    return fullBox("stsd", 0, 0, payload);
}

void appendDescriptorLength(Bytes& out, std::size_t length) {
    uint8_t bytes[4] {};
    bytes[3] = static_cast<uint8_t>(length & 0x7F);
    bytes[2] = static_cast<uint8_t>((length >> 7) & 0x7F);
    bytes[1] = static_cast<uint8_t>((length >> 14) & 0x7F);
    bytes[0] = static_cast<uint8_t>((length >> 21) & 0x7F);
    bytes[0] |= 0x80;
    bytes[1] |= 0x80;
    bytes[2] |= 0x80;
    out.insert(out.end(), bytes, bytes + 4);
}

Bytes makeEsds(const Bytes& decoderConfig, int audioBitrate) {
    Bytes decoderSpecific;
    appendU8(decoderSpecific, 0x05);
    appendDescriptorLength(decoderSpecific, decoderConfig.size());
    appendBytes(decoderSpecific, decoderConfig);

    Bytes decoderConfigDescriptor;
    appendU8(decoderConfigDescriptor, 0x40); // MPEG-4 Audio
    appendU8(decoderConfigDescriptor, 0x15); // AudioStream
    appendU24(decoderConfigDescriptor, 0);
    appendU32(decoderConfigDescriptor, static_cast<uint32_t>(audioBitrate));
    appendU32(decoderConfigDescriptor, static_cast<uint32_t>(audioBitrate));
    appendBytes(decoderConfigDescriptor, decoderSpecific);

    Bytes decoderDescriptor;
    appendU8(decoderDescriptor, 0x04);
    appendDescriptorLength(decoderDescriptor, decoderConfigDescriptor.size());
    appendBytes(decoderDescriptor, decoderConfigDescriptor);

    Bytes slConfig;
    appendU8(slConfig, 0x06);
    appendDescriptorLength(slConfig, 1);
    appendU8(slConfig, 0x02);

    Bytes esDescriptor;
    appendU16(esDescriptor, 1);
    appendU8(esDescriptor, 0);
    appendBytes(esDescriptor, decoderDescriptor);
    appendBytes(esDescriptor, slConfig);

    Bytes payload;
    appendU8(payload, 0x03);
    appendDescriptorLength(payload, esDescriptor.size());
    appendBytes(payload, esDescriptor);
    return fullBox("esds", 0, 0, payload);
}

Bytes makeAudioStsd(const AacAudioTrack& track) {
    Bytes sampleEntry;
    for (int i = 0; i < 6; ++i) appendU8(sampleEntry, 0);
    appendU16(sampleEntry, 1);
    appendU16(sampleEntry, 0);
    appendU16(sampleEntry, 0);
    appendU32(sampleEntry, 0);
    appendU16(sampleEntry, static_cast<uint16_t>(std::clamp(track.channels, 1, 2)));
    appendU16(sampleEntry, 16);
    appendU16(sampleEntry, 0);
    appendU16(sampleEntry, 0);
    appendU32(sampleEntry, static_cast<uint32_t>(std::max(1, track.sampleRate)) << 16);
    appendBytes(sampleEntry, makeEsds(track.decoderConfig, track.channels == 1 ? 96000 : 160000));

    Bytes payload;
    appendU32(payload, 1);
    appendBytes(payload, box("mp4a", sampleEntry));
    return fullBox("stsd", 0, 0, payload);
}

const SampleInfo& sampleInfo(const SampleInfo& sample) {
    return sample;
}

const SampleInfo& sampleInfo(const OwnedSample& sample) {
    return sample.info;
}

const SampleInfo& sampleInfo(const VideoSamplePlan& sample) {
    return sample.info;
}

template <typename Samples>
Bytes makeStts(const Samples& samples) {
    Bytes payload;
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto& sample : samples) {
        const uint32_t duration = std::max<uint32_t>(1, sampleInfo(sample).duration);
        if (!entries.empty() && entries.back().second == duration) {
            ++entries.back().first;
        } else {
            entries.emplace_back(1, duration);
        }
    }
    appendU32(payload, static_cast<uint32_t>(entries.size()));
    for (const auto& [count, duration] : entries) {
        appendU32(payload, count);
        appendU32(payload, duration);
    }
    return fullBox("stts", 0, 0, payload);
}

template <typename Samples>
Bytes makeStss(const Samples& samples) {
    Bytes payload;
    std::vector<uint32_t> syncSamples;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (sampleInfo(samples[i]).keyframe) syncSamples.push_back(static_cast<uint32_t>(i + 1));
    }
    appendU32(payload, static_cast<uint32_t>(syncSamples.size()));
    for (const auto sampleNumber : syncSamples) appendU32(payload, sampleNumber);
    return fullBox("stss", 0, 0, payload);
}

Bytes makeStsc() {
    Bytes payload;
    appendU32(payload, 1);
    appendU32(payload, 1);
    appendU32(payload, 1);
    appendU32(payload, 1);
    return fullBox("stsc", 0, 0, payload);
}

template <typename Samples>
Bytes makeStsz(const Samples& samples) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU32(payload, static_cast<uint32_t>(sampleInfo(sample).size));
    return fullBox("stsz", 0, 0, payload);
}

template <typename Samples>
Bytes makeCo64(const Samples& samples) {
    Bytes payload;
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU64(payload, sampleInfo(sample).fileOffset);
    return fullBox("co64", 0, 0, payload);
}

template <typename Samples>
uint64_t samplesDuration(const Samples& samples) {
    uint64_t duration = 0;
    for (const auto& sample : samples) duration += std::max<uint32_t>(1, sampleInfo(sample).duration);
    return duration;
}

Bytes makeVideoTrak(
    const Bytes& avcConfig,
    const std::vector<VideoSamplePlan>& samples,
    int width,
    int height,
    int /*fps*/,
    uint64_t movieDuration100ns,
    uint64_t mediaStart100ns) {
    const uint32_t timescale = 10'000'000u; // 100ns units — matches PTS-based sample durations
    const uint64_t mediaDuration = samplesDuration(samples);

    Bytes stblPayload;
    appendBytes(stblPayload, makeStsd(avcConfig, width, height));
    appendBytes(stblPayload, makeStts(samples));
    appendBytes(stblPayload, makeStss(samples));
    appendBytes(stblPayload, makeStsc());
    appendBytes(stblPayload, makeStsz(samples));
    appendBytes(stblPayload, makeCo64(samples));

    Bytes minfPayload;
    appendBytes(minfPayload, makeVmhd());
    appendBytes(minfPayload, makeDinf());
    appendBytes(minfPayload, box("stbl", stblPayload));

    Bytes mdiaPayload;
    appendBytes(mdiaPayload, makeMdhd(timescale, mediaDuration));
    appendBytes(mdiaPayload, makeHdlr());
    appendBytes(mdiaPayload, box("minf", minfPayload));

    Bytes trakPayload;
    appendBytes(trakPayload, makeTkhd(movieDuration100ns, width, height));
    appendBytes(trakPayload, makeVideoEdts(movieDuration100ns, mediaStart100ns));
    appendBytes(trakPayload, box("mdia", mdiaPayload));
    return box("trak", trakPayload);
}

Bytes makeAudioTrak(
    const AacAudioTrack& track,
    uint32_t trackId,
    uint64_t movieDuration,
    int64_t videoStartPts100ns) {
    const uint32_t timescale = static_cast<uint32_t>(std::max(1, track.sampleRate));
    const uint64_t duration = samplesDuration(track.samples);

    Bytes stblPayload;
    appendBytes(stblPayload, makeAudioStsd(track));
    appendBytes(stblPayload, makeStts(track.samples));
    appendBytes(stblPayload, makeStsc());
    appendBytes(stblPayload, makeStsz(track.samples));
    appendBytes(stblPayload, makeCo64(track.samples));

    Bytes minfPayload;
    appendBytes(minfPayload, makeSmhd());
    appendBytes(minfPayload, makeDinf());
    appendBytes(minfPayload, box("stbl", stblPayload));

    Bytes mdiaPayload;
    appendBytes(mdiaPayload, makeMdhd(timescale, duration));
    appendBytes(mdiaPayload, makeAudioHdlr());
    appendBytes(mdiaPayload, box("minf", minfPayload));

    Bytes trakPayload;
    appendBytes(trakPayload, makeAudioTkhd(trackId, movieDuration));
    if (const auto edits = makeAudioEdts(track, movieDuration, videoStartPts100ns); !edits.empty()) {
        appendBytes(trakPayload, edits);
    }
    appendBytes(trakPayload, box("mdia", mdiaPayload));
    return box("trak", trakPayload);
}

Bytes makeMoov(
    const Bytes& avcConfig,
    const std::vector<VideoSamplePlan>& samples,
    const std::vector<AacAudioTrack>& audioTracks,
    int width,
    int height,
    int fps,
    uint64_t movieDuration100ns,
    uint64_t videoMediaStart100ns,
    int64_t videoPresentationStartPts100ns) {
    const uint32_t timescale = 10'000'000u; // movie timescale in 100ns units — matches video track

    Bytes moovPayload;
    appendBytes(moovPayload, makeMvhd(timescale, movieDuration100ns, static_cast<uint32_t>(audioTracks.size() + 2)));
    appendBytes(moovPayload, makeVideoTrak(
        avcConfig,
        samples,
        width,
        height,
        fps,
        movieDuration100ns,
        videoMediaStart100ns));
    uint32_t trackId = 2;
    for (const auto& audioTrack : audioTracks) {
        appendBytes(moovPayload, makeAudioTrak(
            audioTrack,
            trackId++,
            movieDuration100ns,
            videoPresentationStartPts100ns));
    }
    return box("moov", moovPayload);
}


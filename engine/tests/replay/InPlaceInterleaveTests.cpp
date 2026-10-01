#include "TestSupport.hpp"
#include "clipture/AacEncoderSession.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"
#include <algorithm>
#include <cmath>

namespace replay_tests {
namespace {
using namespace clipture;
constexpr int kRate = 48000;
constexpr int kSeconds = 6;

uint32_t be32(const std::vector<char>& data, std::size_t at) {
    return (uint32_t(uint8_t(data[at])) << 24) | (uint32_t(uint8_t(data[at + 1])) << 16) |
        (uint32_t(uint8_t(data[at + 2])) << 8) | uint32_t(uint8_t(data[at + 3]));
}
uint64_t be64(const std::vector<char>& data, std::size_t at) { return (uint64_t(be32(data, at)) << 32) | be32(data, at + 4); }

// Child boxes of [begin, end) as (type, body, end).
std::vector<std::tuple<std::string, std::size_t, std::size_t>> children(const std::vector<char>& data, std::size_t begin, std::size_t end) {
    std::vector<std::tuple<std::string, std::size_t, std::size_t>> result;
    while (begin + 8 <= end) {
        uint64_t size = be32(data, begin);
        std::size_t header = 8;
        if (size == 1) { size = be64(data, begin + 8); header = 16; }
        if (size < header || begin + size > end) break;
        result.emplace_back(std::string(data.data() + begin + 4, 4), begin + header, begin + size);
        begin += size;
    }
    return result;
}
std::pair<std::size_t, std::size_t> child(const std::vector<char>& data, std::pair<std::size_t, std::size_t> box, const char* type) {
    for (const auto& [kind, body, end] : children(data, box.first, box.second)) if (kind == type) return {body, end};
    throw std::runtime_error(std::string("missing MP4 box ") + type);
}

struct Placed { double seconds; uint64_t offset; };

// File offset and media time for every sample of every track.
std::vector<Placed> samplesByFileOrder(const std::filesystem::path& path) {
    const auto data = readFile(path);
    std::vector<Placed> result;
    const auto moov = child(data, {0, data.size()}, "moov");
    for (const auto& [kind, body, end] : children(data, moov.first, moov.second)) {
        if (kind != "trak") continue;
        const auto mdia = child(data, {body, end}, "mdia");
        const auto mdhd = child(data, mdia, "mdhd");
        const auto timescale = be32(data, mdhd.first + (data[mdhd.first] == 1 ? 20 : 12));
        const auto stbl = child(data, child(data, mdia, "minf"), "stbl");
        const auto stsz = child(data, stbl, "stsz");
        const auto fixed = be32(data, stsz.first + 4), count = be32(data, stsz.first + 8);
        std::vector<uint64_t> chunks;
        for (const auto& [box, chunkBody, chunkEnd] : children(data, stbl.first, stbl.second)) {
            if (box != "stco" && box != "co64") continue;
            const auto n = be32(data, chunkBody + 4);
            for (uint32_t i = 0; i < n; ++i) chunks.push_back(box == "stco" ? be32(data, chunkBody + 8 + 4 * i) : be64(data, chunkBody + 8 + 8 * i));
        }
        const auto stsc = child(data, stbl, "stsc");
        const auto runs = be32(data, stsc.first + 4);
        const auto stts = child(data, stbl, "stts");
        std::vector<uint32_t> deltas;
        for (uint32_t i = 0, n = be32(data, stts.first + 4); i < n; ++i)
            deltas.insert(deltas.end(), be32(data, stts.first + 8 + 8 * i), be32(data, stts.first + 12 + 8 * i));
        uint32_t sample = 0;
        uint64_t time = 0;
        for (uint32_t run = 0; run < runs; ++run) {
            const auto first = be32(data, stsc.first + 8 + 12 * run), perChunk = be32(data, stsc.first + 12 + 12 * run);
            const auto last = run + 1 < runs ? be32(data, stsc.first + 8 + 12 * (run + 1)) - 1 : uint32_t(chunks.size());
            for (auto chunk = first; chunk <= last; ++chunk) {
                auto offset = chunks[chunk - 1];
                for (uint32_t i = 0; i < perChunk && sample < count; ++i, ++sample) {
                    result.push_back({double(time) / timescale, offset});
                    offset += fixed ? fixed : be32(data, stsz.first + 12 + 4 * sample);
                    time += sample < deltas.size() ? deltas[sample] : 0;
                }
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    return result;
}

std::vector<EncodedPacket> aacTrack() {
    AacEncoderSession encoder;
    std::string error;
    require(encoder.start(kRate, 2, error), "start synthetic AAC encoder");
    std::vector<EncodedPacket> packets;
    std::vector<AacEncodedFrame> frames;
    auto collect = [&] {
        for (auto& frame : frames) {
            EncodedPacket packet;
            packet.kind = PacketKind::Audio;
            packet.codec = PacketCodec::AacLc;
            packet.sourceId = packet.logicalTrackId = "system-loopback-pcm";
            packet.pts100ns = packet.dts100ns = frame.pts100ns;
            packet.audioFrameCount = frame.durationFrames;
            packet.duration100ns = static_cast<int64_t>(frame.durationFrames) * 10'000'000 / kRate;
            packet.audioPrimingFrames = frame.primingFrames;
            packet.sampleRate = kRate;
            packet.channelCount = 2;
            packet.encoderEpoch = 1;
            packet.payload = std::make_shared<PacketPayload>(std::move(frame.payload));
            packets.push_back(std::move(packet));
        }
        frames.clear();
    };
    for (int frame = 0; frame < kSeconds * kRate; frame += 4096) {
        const auto count = std::min(4096, kSeconds * kRate - frame);
        std::vector<int16_t> pcm(static_cast<std::size_t>(count) * 2);
        for (int i = 0; i < count; ++i) pcm[2 * i] = pcm[2 * i + 1] = static_cast<int16_t>(6000 * std::sin(0.05 * (frame + i)));
        require(encoder.encode(std::as_bytes(std::span(pcm)), 10'000'000LL + static_cast<int64_t>(frame) * 10'000'000 / kRate,
            count, frames, error), "encode synthetic AAC");
        collect();
    }
    require(encoder.finish(frames, error), "finish synthetic AAC");
    collect();
    return packets;
}
}

void testInPlaceInterleave() {
    ScratchDirectory scratch;
    std::vector<EncodedPacket> source;
    auto video = fixturePacket();
    for (int i = 0; i < kSeconds * 30; ++i) {
        video.keyframe = i % 30 == 0;
        source.push_back(video);
        video.pts100ns = video.dts100ns = video.pts100ns + 333'333;
    }
    for (auto& packet : aacTrack()) source.push_back(std::move(packet));
    std::stable_sort(source.begin(), source.end(), [](const auto& a, const auto& b) { return a.pts100ns < b.pts100ns; });

    // Record everything into arena A, then detach it as the previous clip.
    replay::InPlacePacketArchive archive;
    archive.configure(scratch.path, true, 64u * 1024u * 1024u);
    std::vector<EncodedPacket> stored;
    for (const auto& packet : source) {
        auto persisted = archive.persist(packet);
        require(persisted.has_value(), "persist fixture sample into the live arena");
        stored.push_back(std::move(*persisted));
    }
    auto previousClip = archive.takeForSave();
    require(previousClip != nullptr, "first save detaches arena A");

    // A quick re-save before any backfill: every sample lives in A and must be
    // appended to arena B. File order must follow time across both tracks.
    auto tick = video;
    tick.pts100ns = tick.dts100ns = stored.back().pts100ns + 10'000'000;
    require(archive.persist(tick).has_value(), "live capture continues into arena B");
    auto resave = archive.takeForSave();
    require(resave != nullptr, "re-save detaches arena B");
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = source.front().pts100ns;
    pacing.presentationEndPts100ns = source.back().pts100ns;
    pacing.experimentalInPlace = resave.get();
    const auto saved = muxH264ToMp4(stored, (scratch.path / "resave").string(), 160, 90, 30, 10, pacing);
    require(saved.ok && saved.audioTracks.size() == 1, "quick re-save muxes video and audio in place");

    const auto placed = samplesByFileOrder(saved.filePath);
    require(placed.size() > std::size_t(kSeconds) * 30, "output indexes video and audio samples");
    double latest = 0, worstRegression = 0;
    for (const auto& sample : placed) {
        worstRegression = std::max(worstRegression, latest - sample.seconds);
        latest = std::max(latest, sample.seconds);
    }
    require(worstRegression < 0.5, "appended audio is interleaved with its video, not stored after all of it");
}
}

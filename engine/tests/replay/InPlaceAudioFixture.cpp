#include "TestSupport.hpp"
#include "clipture/AacEncoderSession.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"
#include "clipture/replay/ConsumedWindow.hpp"
#include <cmath>
#include <iostream>

namespace replay_tests {
namespace {
using namespace clipture;
constexpr int rate = 48000;
std::vector<EncodedPacket> audioFixture(const std::string& id, double frequency) {
    AacEncoderSession encoder;
    std::string error;
    require(encoder.start(rate, 2, error), "start synthetic AAC encoder");
    std::vector<EncodedPacket> packets;
    std::vector<AacEncodedFrame> frames;
    auto collect = [&] {
        for (auto& frame : frames) {
            EncodedPacket packet;
            packet.kind = PacketKind::Audio;
            packet.codec = PacketCodec::AacLc;
            packet.sourceId = packet.logicalTrackId = id;
            packet.pts100ns = packet.dts100ns = frame.pts100ns;
            packet.audioFrameCount = frame.durationFrames;
            packet.duration100ns = static_cast<int64_t>(frame.durationFrames) * 10'000'000 / rate;
            packet.audioPrimingFrames = frame.primingFrames;
            packet.sampleRate = rate;
            packet.channelCount = 2;
            packet.encoderEpoch = 1;
            packet.payload = std::make_shared<PacketPayload>(std::move(frame.payload));
            // A late microphone start and a brief dropout exercise the existing
            // silence/gap normalization, not merely contiguous AAC packets.
            const auto relative = packet.pts100ns - 10'000'000;
            if (id != "microphone-pcm" || (relative >= 20'000'000 && !(relative >= 50'000'000 && relative < 52'500'000)))
                packets.push_back(std::move(packet));
        }
        frames.clear();
    };
    for (int frame = 0; frame < 132 * rate; frame += 4096) {
        const auto count = std::min(4096, 132 * rate - frame);
        std::vector<int16_t> pcm(static_cast<std::size_t>(count) * 2);
        for (int i = 0; i < count; ++i) {
            const auto value = static_cast<int16_t>(8000 * std::sin(6.283185307179586 * frequency * (frame + i) / rate));
            pcm[2 * i] = value; pcm[2 * i + 1] = value;
        }
        require(encoder.encode(std::as_bytes(std::span(pcm)), 10'000'000LL + static_cast<int64_t>(frame) * 10'000'000 / rate,
            count, frames, error), "encode synthetic AAC");
        collect();
    }
    require(encoder.finish(frames, error), "finish synthetic AAC");
    collect();
    return packets;
}
void saveWindow(const std::vector<EncodedPacket>& stored, replay::InPlacePacketArchive& archive,
    replay::ConsumedWindow& consumed, int64_t end, const std::filesystem::path& root, int expectedSeconds) {
    auto selected = replay::selectReplayVideo(stored, 120, end, consumed.start());
    require(!selected.empty(), "select video including required decoder preroll");
    const auto start = std::max({selected.front().pts100ns, consumed.start(), end - 120LL * 10'000'000});
    for (const auto& packet : stored) if (packet.kind == PacketKind::Audio && packet.pts100ns < end &&
        packet.pts100ns + packet.duration100ns > start - 2'000'000) selected.push_back(packet);
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = start;
    pacing.presentationEndPts100ns = end;
    const auto compact = muxH264ToMp4(selected, (root / "compact").string(), 160, 90, 30, 10, pacing);
    require(compact.ok && compact.audioTracks.size() == 2, "compact two-track audio reference");
    auto session = archive.takeForSave();
    require(session != nullptr, "detach live file for in-place save");
    const auto before = session->io();
    pacing.experimentalInPlace = session.get();
    const auto saved = muxH264ToMp4(selected, (root / "in-place").string(), 160, 90, 30, 10, pacing);
    require(saved.ok && saved.audioTracks == compact.audioTracks, "in-place retains logical audio track order");
    const auto after = session->io();
    require(after.mediaWritten - before.mediaWritten < 512 * 1024, "only bounded preroll/tail/repair samples copied");
    require(end - start == static_cast<int64_t>(expectedSeconds) * 10'000'000, "exact consumed presentation interval");
    consumed.commit(end);
    std::ofstream metrics(root / "io.json");
    metrics << "{\"seconds\":" << expectedSeconds << ",\"supplementalMediaBytes\":" << after.mediaWritten - before.mediaWritten
        << ",\"metadataBytes\":" << after.metadataWritten - before.metadataWritten << '}';
    require(metrics.good(), "write audio fixture metrics");
}
}

void runInPlaceAudioFixture(const std::vector<clipture::EncodedPacket>& video, const std::filesystem::path& root) {
    using namespace clipture;
    require(std::filesystem::create_directory(root), "exclusive audio fixture root");
    auto audio = audioFixture("system-loopback-pcm", 440);
    auto microphone = audioFixture("microphone-pcm", 880);
    audio.insert(audio.end(), microphone.begin(), microphone.end());
    auto all = video;
    // Extend the 122-second source by 9.5 seconds, starting a fresh GOP.
    for (std::size_t i = 0; i < 285; ++i) {
        auto packet = video[i]; packet.pts100ns += 122LL * 10'000'000; packet.dts100ns = packet.pts100ns;
        all.push_back(std::move(packet));
    }
    all.insert(all.end(), audio.begin(), audio.end());
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pts100ns < b.pts100ns; });
    replay::InPlacePacketArchive archive;
    archive.configure(root, true, 64 * 1024 * 1024);
    replay::ConsumedWindow consumed;
    std::vector<EncodedPacket> stored;
    const auto firstEnd = video.front().pts100ns + 1215LL * 1'000'000;
    auto item = all.begin();
    for (; item != all.end() && item->pts100ns < firstEnd; ++item) {
        auto packet = archive.persist(*item);
        require(packet.has_value(), "persist interleaved A/V before Save");
        stored.push_back(std::move(*packet));
    }
    saveWindow(stored, archive, consumed, firstEnd, root / "first", 120);
    require(replay::selectReplayVideo(stored, 120, firstEnd, consumed.start()).empty(), "immediate repeat must not duplicate prior footage");
    const auto secondEnd = firstEnd + 10LL * 10'000'000;
    for (; item != all.end() && item->pts100ns < secondEnd; ++item) {
        auto packet = archive.persist(*item);
        require(packet.has_value(), "recording continues in a new file");
        stored.push_back(std::move(*packet));
    }
    saveWindow(stored, archive, consumed, secondEnd, root / "second", 10);
    std::cout << "In-place audio fixture: 120s + 10s non-overlapping video, system and microphone AAC, gap repair.\n";
}
}

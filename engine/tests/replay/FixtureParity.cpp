#include "TestSupport.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/ReplaySegmentStore.hpp"

#include <iostream>

namespace replay_tests {
namespace {
using namespace clipture;
using Clock = std::chrono::steady_clock;

std::vector<EncodedPacket> readAccessUnits(const std::filesystem::path& path) {
    const auto input = readFile(path);
    require(input.size() < 32u * 1024u * 1024u, "fixture must be small, synthetic 160x90 H.264");
    EncodedPacket stream;
    stream.payload = std::make_shared<PacketPayload>(input.size());
    std::memcpy(stream.payload->data(), input.data(), input.size());
    require(analyzeH264Packet(stream), "analyze fixture NALs");
    std::vector<std::size_t> starts;
    forEachH264Nal(stream.h264, [&](const H264NalSpan& nalu) {
        if (nalu.type != 9 || nalu.offset < 3) return;
        const auto prefix = nalu.offset >= 4 && (*stream.payload)[nalu.offset - 4] == std::byte{0} ? 4 : 3;
        starts.push_back(nalu.offset - prefix);
    });
    require(starts.size() >= 3660 && starts.front() == 0, "fixture requires 122s at 30fps with AUDs");
    starts.push_back(input.size());
    std::vector<EncodedPacket> packets;
    for (std::size_t index = 0; index + 1 < starts.size(); ++index) {
        EncodedPacket packet;
        packet.payload = std::make_shared<PacketPayload>(
            stream.payload->begin() + starts[index], stream.payload->begin() + starts[index + 1]);
        packet.pts100ns = 10'000'000 + static_cast<int64_t>(index) * 10'000'000 / 30;
        packet.dts100ns = packet.pts100ns;
        packet.duration100ns = static_cast<int64_t>(index + 1) * 10'000'000 / 30 -
            static_cast<int64_t>(index) * 10'000'000 / 30;
        packet.encodedWidth = 160;
        packet.encodedHeight = 90;
        packet.encoderEpoch = 1;
        packet.sourceFrameSequence = index + 1;
        require(analyzeH264Packet(packet), "analyze access unit");
        packets.push_back(std::move(packet));
    }
    return packets;
}

struct MeasuredSave {
    MuxResult mux;
    double persistenceMs = 0;
    uint64_t diskBytes = 0;
};

MeasuredSave save(const std::vector<EncodedPacket>& packets, const std::filesystem::path& root,
                  bool prepare, bool mixed) {
    ReplaySegmentStoreOptions options;
    options.rootDirectory = root / "archive";
    options.retention100ns = 180LL * 10'000'000;
    options.alignSegmentsToKeyframes = true;
    options.prepareMp4Samples = prepare;
    options.residentPayloadBudgetBytes = 0;
    ReplaySegmentStore store(options);
    store.start();
    const auto started = Clock::now();
    for (const auto& packet : packets) store.push(packet);
    require(store.waitUntilIdle(std::chrono::seconds(30)), "fixture persistence timed out");
    MeasuredSave result;
    result.persistenceMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    result.diskBytes = store.stats().diskBytes;
    auto snapshot = store.snapshot();
    require(snapshot.size() == packets.size(), "no fixture packets may expire");
    for (const auto& packet : snapshot) {
        require(!packet.payload && packet.payloadReader &&
                packet.codec == (prepare ? PacketCodec::H264Avcc : PacketCodec::H264AnnexB),
                "fixture must actually exercise the requested disk representation");
    }
    if (mixed) {
        for (std::size_t index = snapshot.size() - 30; index < snapshot.size(); ++index) {
            snapshot[index] = packets[index]; // Simulate the resident, unconverted tail.
        }
    }
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = packets.front().pts100ns + 15'000'000;
    pacing.presentationEndPts100ns = pacing.presentationStartPts100ns + 120LL * 10'000'000;
    pacing.analyzeIo = true;
    result.mux = muxH264ToMp4(snapshot, (root / "output").string(), 160, 90, 30, 10, pacing);
    require(result.mux.ok, "real encoded fixture must mux");
    if (prepare && !mixed) verifyExtentFixture(snapshot, result.mux.filePath, root / "extent-copy.mp4");
    if (prepare && !mixed) verifyWindowsCloneFixture(result.mux.filePath, root / "native-file-copy.mp4");
    if (prepare && !mixed) {
        for (const uint64_t alignment : {4096ULL, 65536ULL}) {
            pacing.experimentalPayloadAlignment = alignment;
            const auto aligned = muxH264ToMp4(snapshot, (root / ("aligned-" + std::to_string(alignment))).string(),
                                              160, 90, 30, 10, pacing);
            require(aligned.ok, "aligned fixture mux must succeed");
        }
    }
    store.stop();
    return result;
}

void writeMetrics(std::ostream& output, const MeasuredSave& result) {
    output << "{\"persistenceMs\":" << result.persistenceMs
           << ",\"replayDiskBytes\":" << result.diskBytes
           << ",\"mux\":" << saveIoAnalysisToJson(result.mux.ioAnalysis) << '}';
}

}  // namespace

void runFixtureParity(const std::filesystem::path& input, const std::filesystem::path& output) {
    // Refuse reuse; fixture outputs are separate from the user's recording/library.
    require(std::filesystem::create_directory(output), "fixture output must be a new directory");
    const auto packets = readAccessUnits(input);
    verifyInPlaceFixture(packets, output / "in-place");
    if (output.filename() == "trial-0") runInPlaceAudioFixture(packets, output / "audio");
    if (output.filename() == "trial-0") verifyInPlaceRollingFixture(packets, output / "rolling");
    const auto legacy = save(packets, output / "legacy", false, false);
    const auto prepared = save(packets, output / "prepared", true, false);
    const auto mixed = save(packets, output / "mixed", true, true);
    const auto baseline = readFile(legacy.mux.filePath);
    require(baseline == readFile(prepared.mux.filePath), "prepared MP4 must be byte-identical to baseline");
    require(baseline == readFile(mixed.mux.filePath), "mixed MP4 must be byte-identical to baseline");
    std::ofstream metrics(output / "metrics.json");
    metrics << "{\"legacy\":";
    writeMetrics(metrics, legacy);
    metrics << ",\"prepared\":";
    writeMetrics(metrics, prepared);
    metrics << ",\"mixed\":";
    writeMetrics(metrics, mixed);
    metrics << '}';
    require(metrics.good(), "write metrics");
    std::cout << "Real encoded fixture: legacy/prepared/mixed MP4 byte parity passed.\n";
}

}  // namespace replay_tests

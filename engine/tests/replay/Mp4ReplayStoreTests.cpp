#include "TestSupport.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/Mp4SamplePacker.hpp"

#include <thread>

namespace replay_tests {
using namespace clipture;

void testStoreAndMux() {
    ScratchDirectory scratch;
    const auto original = fixturePacket();
    const auto expected = replay::packMp4VideoSample(original);
    ReplaySegmentStoreOptions options;
    options.rootDirectory = scratch.path / "archive";
    options.prepareMp4Samples = true;
    options.maximumWriteBytes = 5; // Exercise partial writes of the new layout.
    options.targetSegmentBytes = 1; // Exercise rotation and snapshot ownership.
    options.residentPayloadBudgetBytes = payloadSize(original);
    ReplaySegmentStore store(options);
    store.start();
    store.push(original);
    const auto beforeSpill = store.snapshot();
    store.setResidentPayloadBudget(0);
    require(store.waitUntilIdle(std::chrono::seconds(5)), "packed spill must finish");
    const auto onDisk = store.snapshot();
    require(onDisk.size() == 1 && onDisk[0].codec == PacketCodec::H264Avcc &&
            !onDisk[0].payload && onDisk[0].payloadReader, "only fully persisted packet becomes MP4-ready");
    require(beforeSpill[0].codec == PacketCodec::H264AnnexB && beforeSpill[0].payload,
            "pre-spill snapshot must keep its original representation");
    PacketPayload readback(payloadSize(onDisk[0]));
    require(readPayload(onDisk[0], 0, readback) && readback == *expected->payload,
            "disk bytes must be the prepared MP4 sample");
    const auto stats = store.stats();
    require(stats.residentPayloadBytes == 0 && stats.queuedBytes == 0 &&
            stats.diskBytes == expected->payload->size(), "account original RAM bytes and new disk bytes separately");

    auto tail = original;
    tail.pts100ns += original.duration100ns;
    tail.dts100ns = tail.pts100ns;
    tail.sourceFrameSequence++;
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = original.pts100ns + 50'000;
    pacing.presentationEndPts100ns = tail.pts100ns + tail.duration100ns;
    pacing.analyzeIo = true;
    pacing.experimentalPayloadAlignment = 4096; // Legacy/mixed windows must still take compact fallback.
    const auto oldMux = muxH264ToMp4({original, tail}, (scratch.path / "old").string(), 160, 90, 30, 10, pacing);
    const auto newMux = muxH264ToMp4({onDisk[0], tail}, (scratch.path / "new").string(), 160, 90, 30, 10, pacing);
    require(oldMux.ok && newMux.ok, "both old and mixed-format saves must mux");
    require(readFile(oldMux.filePath) == readFile(newMux.filePath), "mixed-format MP4 must be byte-identical to legacy output");

    auto audio = original;
    audio.kind = PacketKind::Audio;
    audio.codec = PacketCodec::AacLc;
    audio.h264 = {};
    audio.sampleRate = 48000;
    audio.channelCount = 2;
    audio.audioFrameCount = 1024;
    audio.logicalTrackId = "fixture-audio";
    audio.payload = std::make_shared<PacketPayload>(bytes({0x21, 0x10, 4, 0x60}));
    store.push(audio);
    require(store.waitUntilIdle(std::chrono::seconds(5)), "audio fallback persists");
    const auto withAudio = store.snapshot();
    require(withAudio.back().codec == PacketCodec::AacLc && payloadSize(withAudio.back()) == 4,
            "audio storage representation is unchanged");
    // Retain the sole synthetic AAC packet: this test checks layout fallback,
    // not the existing whole-packet trimming behavior at a fractional start.
    pacing.presentationStartPts100ns = original.pts100ns;
    const auto audioFallback = muxH264ToMp4(withAudio, (scratch.path / "audio-fallback").string(), 160, 90, 30, 10, pacing);
    pacing.experimentalPayloadAlignment = 0;
    const auto audioBaseline = muxH264ToMp4(withAudio, (scratch.path / "audio-baseline").string(), 160, 90, 30, 10, pacing);
    require(audioFallback.ok && audioBaseline.ok && audioFallback.audioTracks == std::vector<std::string>{"fixture-audio"},
            "audio-containing save retains its track when aligned experiment is requested");
    require(readFile(audioFallback.filePath) == readFile(audioBaseline.filePath),
            "audio-containing save must use byte-identical compact fallback");

    store.clear();
    store.stop();
    require(readPayload(onDisk[0], 0, readback) && readback == *expected->payload,
            "saved snapshot pins must survive store clear/stop");

    // A failed disk write must never discard or relabel the only valid payload.
    const auto blocker = scratch.path / "not-a-directory";
    std::ofstream(blocker) << "fixture";
    options.rootDirectory = blocker / "archive";
    options.residentPayloadBudgetBytes = 0;
    ReplaySegmentStore failing(options);
    failing.start();
    failing.push(original);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (failing.stats().writeFailures == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto fallback = failing.snapshot();
    require(failing.stats().writeFailures > 0 && fallback.size() == 1 &&
            fallback[0].codec == PacketCodec::H264AnnexB && fallback[0].payload == original.payload,
            "write failure retains original bytes and format");
    failing.stop();
}

}  // namespace replay_tests

#include "TestSupport.hpp"
#include "clipture/replay/Mp4SamplePacker.hpp"

#include <limits>

namespace replay_tests {
using namespace clipture;

void testPacker() {
    const auto original = fixturePacket();
    const auto originalBytes = *original.payload;
    const auto packed = replay::packMp4VideoSample(original);
    require(packed.has_value(), "pack analyzed Annex B packet");
    require(*packed->payload == bytes({
        0, 0, 0, 3, 0x06, 0x05, 0xaa,
        0, 0, 0, 3, 0x65, 0xab, 0xcd,
        0, 0, 0, 2, 0x61, 0xef
    }), "MP4 sample must preserve SEI and slices, remove SPS/PPS/AUD, and use big-endian lengths");
    require(packed->codec == PacketCodec::H264Avcc && !packed->h264.analyzed &&
            !packed->payloadReader, "packed representation must not retain stale Annex B offsets");
    require(packed->h264Config && packed->h264Config->sps == bytes({0x67, 0x64, 0, 0x28}) &&
            packed->h264Config->pps == bytes({0x68, 0xee, 0x3c, 0x80}), "preserve decoder configuration");
    require(packed->pts100ns == original.pts100ns && packed->dts100ns == original.dts100ns &&
            packed->duration100ns == original.duration100ns && packed->keyframe &&
            packed->sourceFrameSequence == 123 && packed->encoderEpoch == 7 &&
            packed->sourceId == original.sourceId && packed->encodedWidth == 160,
            "packing must preserve timeline, source, keyframe and epoch metadata");
    require(*original.payload == originalBytes && original.codec == PacketCodec::H264AnnexB &&
            original.h264.analyzed, "source must be unchanged");

    auto unanalyzed = original;
    unanalyzed.h264 = {};
    const auto packedUnanalyzed = replay::packMp4VideoSample(unanalyzed);
    require(packedUnanalyzed && *packedUnanalyzed->payload == *packed->payload,
            "resident unanalyzed input can be analyzed locally");
    require(!replay::packMp4VideoSample(*packed), "already packed input takes pass-through fallback");

    auto rejected = original;
    rejected.kind = PacketKind::Audio;
    require(!replay::packMp4VideoSample(rejected), "audio must remain untouched");
    rejected = original;
    rejected.payload.reset();
    require(!replay::packMp4VideoSample(rejected), "packer must not materialize a disk-backed window");
    rejected = original;
    rejected.h264.inlineCount = 255;
    require(!replay::packMp4VideoSample(rejected), "reject invalid inline count before iteration");
    rejected = original;
    rejected.h264.inlineNalus[0].size = std::numeric_limits<uint32_t>::max();
    require(!replay::packMp4VideoSample(rejected), "reject out-of-bounds size");
    rejected = original;
    rejected.h264.inlineNalus[0].offset = std::numeric_limits<uint32_t>::max();
    require(!replay::packMp4VideoSample(rejected), "reject out-of-bounds offset");
    rejected = original;
    rejected.h264.avccSampleSize++;
    require(!replay::packMp4VideoSample(rejected), "reject stale sample size");
    rejected = original;
    rejected.h264.inlineNalus[1].offset = 0;
    require(!replay::packMp4VideoSample(rejected), "reject overlapping spans");
    rejected = original;
    rejected.payload = std::make_shared<PacketPayload>(bytes({0, 0, 1, 0x67, 0x64, 0, 0x28}));
    analyzeH264Packet(rejected);
    require(!replay::packMp4VideoSample(rejected), "parameter-only packet retains legacy path");
    rejected.payload = std::make_shared<PacketPayload>(bytes({1, 2, 3, 4}));
    rejected.h264 = {};
    require(!replay::packMp4VideoSample(rejected), "malformed packet retains legacy path");
}

}  // namespace replay_tests

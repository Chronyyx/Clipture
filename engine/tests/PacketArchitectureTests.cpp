#include "clipture/BoundedWrite.hpp"
#include "clipture/AudioTimeline.hpp"
#include "clipture/AudioPacketRouting.hpp"
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/H264PacketAnalyzer.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/VideoTimeline.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <memory>
#include <map>
#include <thread>
#include <vector>
#include <chrono>

namespace {

clipture::EncodedPacket packetFromBytes(std::initializer_list<uint8_t> values) {
    clipture::EncodedPacket packet;
    packet.kind = clipture::PacketKind::Video;
    packet.payload = std::make_shared<clipture::PacketPayload>();
    packet.payload->reserve(values.size());
    for (const auto value : values) packet.payload->push_back(static_cast<std::byte>(value));
    return packet;
}

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

bool testStartCodesAndFlags() {
    auto packet = packetFromBytes({
        0, 0, 1, 0x09, 0x10,
        0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28,
        0, 0, 1, 0x68, 0xEE,
        0, 0, 0, 1, 0x65, 0xAA, 0xBB,
        0, 0, 1, 0x01, 0xCC,
        0, 0, 1, 0x06, 0xDD
    });
    if (!require(clipture::analyzeH264Packet(packet), "mixed start-code packet should parse")) return false;
    if (!require(packet.codec == clipture::PacketCodec::H264AnnexB, "codec should be H.264 Annex B")) return false;
    if (!require(packet.h264.hasAud && packet.h264.hasSps && packet.h264.hasPps && packet.h264.hasIdr,
                 "AUD/SPS/PPS/IDR flags should be recorded")) return false;
    if (!require(packet.keyframe, "IDR should mark packet as keyframe")) return false;
    if (!require(packet.h264.inlineCount == 4, "first four NAL spans should be inline")) return false;
    if (!require(packet.h264.overflowNalus && packet.h264.overflowNalus->size() == 2,
                 "unusual multi-NAL packet should use shared overflow metadata")) return false;
    const uint32_t expectedAvccBytes = (4 + 3) + (4 + 2) + (4 + 2);
    return require(packet.h264.avccSampleSize == expectedAvccBytes, "AVCC size should exclude AUD/SPS/PPS");
}

bool testMalformedPackets() {
    auto malformed = packetFromBytes({0, 0, 0, 0, 0x65, 0xAA});
    if (!require(!clipture::analyzeH264Packet(malformed), "packet without a start code should be rejected")) return false;
    if (!require(malformed.h264.analyzed && malformed.h264.nalCount() == 0,
                 "malformed packet should still be marked analyzed")) return false;

    clipture::EncodedPacket empty;
    return require(!clipture::analyzeH264Packet(empty), "null payload should be rejected safely");
}

bool testBoundedWrites() {
    constexpr std::size_t maximum = 4u * 1024u * 1024u;
    std::size_t remaining = 11u * 1024u * 1024u + 17u;
    std::size_t total = 0;
    std::size_t writes = 0;
    while (remaining > 0) {
        const std::size_t request = clipture::boundedWriteSize(remaining, maximum);
        if (!require(request > 0 && request <= maximum, "write request must remain within the hard cap")) return false;
        remaining -= request;
        total += request;
        ++writes;
    }
    return require(total == 11u * 1024u * 1024u + 17u && writes == 3,
                   "bounded writer should cover every byte in three requests");
}

bool testVideoTimelineNeverCatchesUpRetroactively() {
    constexpr int fps = 60;
    constexpr int64_t firstPts100ns = 50'000'000'000LL;
    const int64_t spacing100ns = 10'000'000LL / fps;
    const int64_t stalls100ns[] {
        500'000LL,
        5'000'000LL,
        49'000'000LL,
        55'000'000LL,
        100'000'000LL,
    };

    for (const int64_t stall100ns : stalls100ns) {
        clipture::VideoTimeline timeline(firstPts100ns, fps);
        const auto initial = timeline.advance(0);
        if (!require(initial.pts100ns == firstPts100ns && initial.skippedTicks == 0,
                     "timeline should emit its first current tick once")) return false;

        const auto afterStall = timeline.advance(stall100ns);
        const int64_t expectedSkipped = stall100ns / spacing100ns;
        if (!require(afterStall.skippedTicks == expectedSkipped,
                     "timeline should account for every elapsed tick")) return false;
        if (!require(afterStall.pts100ns == firstPts100ns + (expectedSkipped + 1) * spacing100ns,
                     "stalled timeline should jump forward without catch-up packets")) return false;
    }
    if (!require(clipture::finalVideoSampleDuration100ns(spacing100ns, fps) == spacing100ns,
                 "final video duration should use the packet duration after an earlier gap")) return false;
    return require(clipture::finalVideoSampleDuration100ns(0, fps) == spacing100ns,
                   "final video duration should use one frame as its fallback");
}

bool testAudioTimelineNeverRewinds() {
    bool anchored = false;
    int64_t nextPts100ns = 10'000'000;
    clipture::alignAudioPtsForwardOnly(9'500'000, 100'000, anchored, nextPts100ns);
    if (!require(anchored && nextPts100ns == 10'000'000,
                 "audio clock must not rewind over committed fallback samples")) return false;

    clipture::alignAudioPtsForwardOnly(10'150'000, 100'000, anchored, nextPts100ns);
    if (!require(nextPts100ns == 10'000'000,
                 "small audio clock jitter should not create a gap")) return false;

    clipture::alignAudioPtsForwardOnly(10'300'000, 100'000, anchored, nextPts100ns);
    return require(nextPts100ns == 10'300'000,
                   "a real forward audio discontinuity should remain visible on the timeline");
}

bool testFrameQueueDropAccounting() {
    clipture::FrameQueue queue(3);
    for (uint64_t sequence = 1; sequence <= 5; ++sequence) {
        clipture::CapturedFrame frame;
        frame.sequence = sequence;
        frame.pts100ns = static_cast<int64_t>(sequence) * 100;
        queue.push(std::move(frame));
    }

    const auto afterPush = queue.stats();
    if (!require(afterPush.overflowDrops == 2 && afterPush.maximumDepth == 3,
                 "frame queue should count capacity overflow")) return false;
    const auto latest = queue.consumeAllAndGetLatest();
    if (!require(latest && latest->sequence == 5,
                 "frame queue should preserve the newest frame")) return false;
    const auto afterConsume = queue.stats();
    if (!require(afterConsume.coalescedDrops == 2 && queue.droppedFrames() == 4,
                 "frame queue should count latest-frame coalescing")) return false;

    clipture::FrameQueue jitterQueue(4);
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        clipture::CapturedFrame frame;
        frame.sequence = sequence;
        frame.pts100ns = static_cast<int64_t>(sequence) * 100;
        jitterQueue.push(std::move(frame));
    }
    const auto selected = jitterQueue.consumeLatestAtOrBefore(250);
    return require(selected && selected->sequence == 2 && jitterQueue.size() == 1,
                   "jitter selection should leave future frames queued");
}

bool testImmutableAudioRouting() {
    clipture::EncodedPacket helper;
    helper.sourceId = "app:helper.exe";
    helper.sampleRate = 48000;
    helper.channelCount = 2;
    helper.bitsPerSample = 16;
    helper.payload = std::make_shared<clipture::PacketPayload>(4 * sizeof(int16_t));
    const int16_t audibleSamples[] = { 0, 0, 12, -12 };
    std::memcpy(helper.payload->data(), audibleSamples, sizeof(audibleSamples));

    const std::map<std::string, std::string> firstRoutes {
        { "app:helper.exe", "system-loopback-pcm" }
    };
    clipture::prepareAudioReplayPacket(helper, firstRoutes);
    if (!require(helper.logicalTrackId == "system-loopback-pcm", "foreground helper should use its system alias")) return false;
    if (!require(helper.audible && helper.audioFrameCount == 2, "PCM metadata should record audibility and frame count")) return false;

    auto later = helper;
    later.logicalTrackId.clear();
    const std::map<std::string, std::string> changedRoutes {
        { "app:helper.exe", "app:helper.exe" }
    };
    clipture::prepareAudioReplayPacket(later, changedRoutes);
    if (!require(helper.logicalTrackId == "system-loopback-pcm", "historical routing must remain unchanged")) return false;
    if (!require(later.logicalTrackId == "app:helper.exe", "new packets should use the new route")) return false;

    std::fill(later.payload->begin(), later.payload->end(), std::byte { 0 });
    clipture::prepareAudioReplayPacket(later, changedRoutes);
    return require(!later.audible, "silent PCM should be marked for track omission");
}

clipture::EncodedPacket makePcmPacket(
    clipture::PacketRingBuffer& pool,
    std::string sourceId,
    int64_t pts100ns,
    int16_t value) {
    constexpr int sampleRate = 48000;
    constexpr int channels = 2;
    constexpr int frames = sampleRate / 100;
    clipture::EncodedPacket packet;
    packet.kind = clipture::PacketKind::Audio;
    packet.codec = clipture::PacketCodec::PcmS16;
    packet.pts100ns = pts100ns;
    packet.dts100ns = pts100ns;
    packet.duration100ns = 100'000;
    packet.sourceId = std::move(sourceId);
    packet.encoderId = "PCM_S16";
    packet.sampleRate = sampleRate;
    packet.channelCount = channels;
    packet.bitsPerSample = 16;
    packet.payload = pool.acquirePayload(frames * channels * sizeof(int16_t));
    auto* samples = reinterpret_cast<int16_t*>(packet.payload->data());
    std::fill(samples, samples + frames * channels, value);
    return packet;
}

bool testLiveAacCoordinator() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({
        { "app:ui.exe", "system-loopback-pcm" },
        { "app:audio.exe", "system-loopback-pcm" }
    });
    coordinator.start();

    constexpr int64_t startPts = 10'000'000'000LL;
    for (int block = 0; block < 50; ++block) {
        const int64_t pts = startPts + static_cast<int64_t>(block) * 100'000LL;
        coordinator.publish(makePcmPacket(raw, "app:ui.exe", pts, 700));
        coordinator.publish(makePcmPacket(raw, "app:audio.exe", pts + 20'000LL, 500));
    }
    coordinator.updateRouting({ { "app:ui.exe", "app:ui.exe" } });
    for (int block = 50; block < 80; ++block) {
        const int64_t pts = startPts + static_cast<int64_t>(block) * 100'000LL;
        coordinator.publish(makePcmPacket(raw, "app:ui.exe", pts, 900));
    }
    coordinator.stop();

    const auto encoded = aac.snapshot();
    if (!require(!encoded.empty(), "live coordinator should produce AAC packets")) return false;
    bool sawSystemTrack = false;
    bool sawAppTrack = false;
    for (const auto& packet : encoded) {
        if (!require(packet.codec == clipture::PacketCodec::AacLc && packet.audioFrameCount > 0,
                     "coordinator output should carry AAC metadata")) return false;
        sawSystemTrack = sawSystemTrack || packet.logicalTrackId == "system-loopback-pcm";
        sawAppTrack = sawAppTrack || packet.logicalTrackId == "app:ui.exe";
    }
    if (!require(sawSystemTrack && sawAppTrack, "route changes should create distinct historical AAC tracks")) return false;

    clipture::PacketRingBuffer silentRaw(10LL * 10'000'000LL);
    clipture::PacketRingBuffer silentAac(10LL * 10'000'000LL);
    clipture::AudioReplayCoordinator silentCoordinator(silentRaw, silentAac);
    silentCoordinator.updateRouting({ { "microphone-pcm", "microphone-pcm" } });
    silentCoordinator.start();
    for (int block = 0; block < 30; ++block) {
        silentCoordinator.publish(makePcmPacket(
            silentRaw,
            "microphone-pcm",
            startPts + static_cast<int64_t>(block) * 100'000LL,
            0));
    }
    silentCoordinator.stop();
    return require(silentAac.size() == 0, "silent logical tracks should not emit AAC samples");
}

bool testLiveAacTimelineStaysContinuousThroughSilence() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({ { "microphone-pcm", "microphone-pcm" } });
    coordinator.start();

    constexpr int64_t startPts = 15'000'000'000LL;
    for (int block = 0; block < 200; ++block) {
        const bool quiet = block >= 30 && block < 170;
        coordinator.publish(makePcmPacket(
            raw,
            "microphone-pcm",
            startPts + static_cast<int64_t>(block) * 100'000LL,
            quiet ? 0 : 800));
    }
    coordinator.stop();

    const auto encoded = aac.snapshot();
    if (!require(encoded.size() >= 80, "AAC should continue through a long quiet interval after activation")) {
        return false;
    }
    const uint32_t epoch = encoded.front().encoderEpoch;
    int64_t previousPts = encoded.front().pts100ns;
    for (std::size_t index = 1; index < encoded.size(); ++index) {
        const auto& packet = encoded[index];
        if (!require(packet.encoderEpoch == epoch, "silence must not split a live track into AAC epochs")) {
            return false;
        }
        const int64_t expectedDuration =
            (static_cast<int64_t>(encoded[index - 1].audioFrameCount) * 10'000'000LL) /
            std::max(1, encoded[index - 1].sampleRate);
        const int64_t gap = packet.pts100ns - previousPts;
        if (!require(
                std::abs(gap - expectedDuration) <= 50'000LL,
                "live AAC timestamps should stay contiguous through silence")) {
            return false;
        }
        previousPts = packet.pts100ns;
    }
    return true;
}

bool testConcurrentPublishDoesNotTriggerRepair() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({
        { "app:one.exe", "system-loopback-pcm" },
        { "app:two.exe", "system-loopback-pcm" },
        { "app:three.exe", "system-loopback-pcm" },
        { "app:four.exe", "system-loopback-pcm" }
    });
    coordinator.start();

    constexpr int64_t startPts = 20'000'000'000LL;
    const std::vector<std::string> sources {
        "app:one.exe", "app:two.exe", "app:three.exe", "app:four.exe"
    };
    std::vector<std::thread> publishers;
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        publishers.emplace_back([&, sourceIndex] {
            for (int block = 0; block < 200; ++block) {
                coordinator.publish(makePcmPacket(
                    raw,
                    sources[sourceIndex],
                    startPts + static_cast<int64_t>(block) * 100'000LL,
                    static_cast<int16_t>(100 + sourceIndex)));
            }
        });
    }
    for (auto& publisher : publishers) publisher.join();
    coordinator.stop();

    const auto stats = coordinator.stats();
    if (!require(stats.queueOverflows == 0, "ordinary publisher contention must not count as queue overflow")) {
        return false;
    }
    return require(stats.encoderRestarts == 0, "ordinary publisher contention must not trigger AAC repair");
}

}  // namespace

int main() {
    if (!testStartCodesAndFlags()) return 1;
    if (!testMalformedPackets()) return 1;
    if (!testBoundedWrites()) return 1;
    if (!testVideoTimelineNeverCatchesUpRetroactively()) return 1;
    if (!testAudioTimelineNeverRewinds()) return 1;
    if (!testFrameQueueDropAccounting()) return 1;
    if (!testImmutableAudioRouting()) return 1;
    if (!testLiveAacCoordinator()) return 1;
    if (!testLiveAacTimelineStaysContinuousThroughSilence()) return 1;
    if (!testConcurrentPublishDoesNotTriggerRepair()) return 1;
    std::cout << "Packet architecture tests passed.\n";
    return 0;
}

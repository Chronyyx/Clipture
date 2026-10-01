#include "TestSupport.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/BackfillPace.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"
#include "clipture/replay/InPlaceExtent.hpp"

namespace replay_tests {
namespace {
using namespace clipture;

std::shared_ptr<platform::windows::InPlaceFile> fileOf(const EncodedPacket& packet) {
    const auto extent = packet.payloadReader ? packet.payloadReader->extent() : std::nullopt;
    const auto lease = extent ? std::dynamic_pointer_cast<const replay::InPlaceExtent>(extent->source) : nullptr;
    return lease ? lease->file() : nullptr;
}

std::vector<EncodedPacket> pushDistinct(ReplaySegmentStore& store, int count, std::size_t padding = 0) {
    auto source = fixturePacket();
    std::vector<EncodedPacket> originals;
    for (int i = 0; i < count; ++i) {
        source.payload = std::make_shared<PacketPayload>(*fixturePacket().payload);
        source.payload->insert(source.payload->end(), padding, std::byte{0xef}); // Slice body bytes.
        source.payload->back() = static_cast<std::byte>(i + 1);
        require(analyzeH264Packet(source), "padded fixture must analyze");
        source.keyframe = i == 0;
        store.push(source);
        originals.push_back(source);
        source.pts100ns += 333'333;
    }
    require(store.waitUntilIdle(std::chrono::seconds(5)), "live packets persist into the arena");
    return originals;
}

void requireSameBytes(const std::vector<EncodedPacket>& actual, const std::vector<EncodedPacket>& before) {
    require(actual.size() == before.size(), "backfill keeps every retained packet");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        PacketPayload expected(payloadSize(before[i])), moved(payloadSize(actual[i]));
        require(expected.size() == moved.size() && readPayload(before[i], 0, expected) &&
            readPayload(actual[i], 0, moved) && expected == moved, "backfill preserves payload bytes");
        require(actual[i].pts100ns == before[i].pts100ns && actual[i].codec == before[i].codec,
            "backfill preserves packet metadata");
    }
}

void testBackfillPace() {
    using namespace std::chrono_literals;
    replay::BackfillPace pace;
    const auto start = replay::BackfillPace::Clock::time_point{} + 1h;
    require(pace.multiplier(125) == replay::BackfillPace::baseMultiplier, "first save trickles at 1.1x live");
    pace.recordSave(start);
    require(pace.multiplier(125) == replay::BackfillPace::baseMultiplier, "one save gives no cadence yet");
    pace.recordSave(start + 30s);
    const auto quick = pace.multiplier(125);
    require(quick > 4.0 && quick < 4.5, "a 30 s re-save finishes a 125 s window before 80% of the gap");
    pace.recordSave(start + 33s);
    require(pace.multiplier(125) == replay::BackfillPace::maximumMultiplier, "rapid re-saves stay capped, never unlimited");
    pace.recordSave(start + 33s + 30min);
    pace.recordSave(start + 33s + 60min);
    require(pace.multiplier(125) == replay::BackfillPace::baseMultiplier, "long pauses return to the passive trickle");
}
}

void testInPlaceOverlap() {
    testBackfillPace();
    ScratchDirectory scratch;
    auto archive = std::make_shared<replay::InPlacePacketArchive>();
    archive->configure(scratch.path, true, 64u * 1024u * 1024u);
    ReplaySegmentStoreOptions options;
    options.rootDirectory = scratch.path / "legacy-fallback";
    options.retention100ns = 120LL * 10'000'000;
    ReplaySegmentStore store(options);
    store.setInPlaceArchive(archive);
    store.start();
    pushDistinct(store, 12);
    const auto beforeSave = store.snapshot();
    require(!archive->isDetached(beforeSave.front()), "live arena packets are not backfill candidates");

    auto saved = archive->takeForSave();
    require(saved != nullptr, "first save detaches the arena");
    const auto savedFile = fileOf(beforeSave.front());
    for (const auto& packet : store.snapshot()) {
        require(archive->isDetached(packet), "retained packets now live in the saved file");
    }

    store.scheduleInPlaceRehome(1000.0);
    require(store.waitUntilRehomed(std::chrono::seconds(5)), "backfill completes");
    const auto afterBackfill = store.snapshot();
    requireSameBytes(afterBackfill, beforeSave);
    for (std::size_t i = 0; i < afterBackfill.size(); ++i) {
        require(!archive->isDetached(afterBackfill[i]) && fileOf(afterBackfill[i]) != savedFile,
            "backfilled packets live in the new active arena");
        if (i == 0) continue;
        const auto previous = afterBackfill[i - 1].payloadReader->extent();
        require(afterBackfill[i].payloadReader->extent()->offset == previous->offset + previous->length,
            "a chunk is one contiguous sequential write in presentation order");
    }
    require(store.stats().rehomedPackets == afterBackfill.size(), "stats count every backfilled packet");
    store.scheduleInPlaceRehome(1000.0);
    require(store.waitUntilRehomed(std::chrono::seconds(5)) && store.stats().rehomedPackets == afterBackfill.size(),
        "rescheduling without a new save copies nothing");

    // The overlapping second save reuses backfilled offsets: no media is copied.
    auto second = archive->takeForSave();
    require(second != nullptr, "second save detaches the backfilled arena");
    const auto mediaEnd = second->mediaEnd();
    std::vector<const EncodedPacket*> samples;
    for (const auto& packet : afterBackfill) samples.push_back(&packet);
    const auto layout = second->layout(samples);
    require(layout && layout->sampleOffsets.size() == samples.size(), "overlapping save lays out every sample");
    require(layout->mediaEnd == mediaEnd, "overlapping save after backfill appends no media");
    store.stop();

    // Pacing is relative to how fast footage arrives. A tiny multiplier copies
    // the newest chunk first, then waits; stop must not wait for that delay.
    ScratchDirectory pacedScratch;
    archive->configure(pacedScratch.path, true, 64u * 1024u * 1024u);
    options.rootDirectory = pacedScratch.path / "legacy-fallback";
    ReplaySegmentStore paced(options);
    paced.setInPlaceArchive(archive);
    paced.start();
    pushDistinct(paced, 4, 300u * 1024u);
    require(archive->takeForSave() != nullptr, "paced save detaches the arena");
    paced.scheduleInPlaceRehome(1e-6);
    require(!paced.waitUntilRehomed(std::chrono::milliseconds(200)), "a slow pace spreads backfill over time");
    const auto partial = paced.snapshot();
    const auto stats = paced.stats();
    require(stats.rehomedPackets == 1 && !archive->isDetached(partial.back()) &&
        archive->isDetached(partial.front()), "backfill copies the newest footage first");
    require(stats.rehomeBytesPerSecond >= 1 && stats.rehomeBytesPerSecond < 1024, "rate scales with the multiplier");
    const auto stopStarted = std::chrono::steady_clock::now();
    paced.stop();
    require(std::chrono::steady_clock::now() - stopStarted < std::chrono::seconds(2), "stop interrupts a pacing wait");

    // A re-save before backfill finishes copies the leftover itself: paced, not
    // a full-speed burst. The first quarter second of budget stays instant.
    std::vector<const EncodedPacket*> leftover;
    uint64_t leftoverBytes = 0;
    for (const auto& packet : partial) {
        if (!archive->isDetached(packet)) continue;
        leftover.push_back(&packet);
        leftoverBytes += payloadSize(packet);
    }
    auto quickResave = archive->takeForSave();
    require(quickResave != nullptr, "quick re-save detaches the next arena");
    require(leftover.size() == 3 && leftoverBytes > 800u * 1024u, "three unreached samples are left to copy");
    constexpr uint64_t pace = 2u * 1024u * 1024u;
    quickResave->setCopyPace(pace);
    const auto copyStarted = std::chrono::steady_clock::now();
    require(quickResave->layout(leftover).has_value(), "leftover samples are copied into the re-saved clip");
    const auto copySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - copyStarted).count();
    const auto expectedSeconds = static_cast<double>(leftoverBytes - pace / 4) / static_cast<double>(pace);
    require(quickResave->copiedBytes() == leftoverBytes && copySeconds >= expectedSeconds * 0.9,
        "save-time copy is spread at the configured pace");
    archive->configure(pacedScratch.path, false, 0);
}
}

#include "TestSupport.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"
#include "clipture/replay/InPlaceExtent.hpp"
#include "clipture/replay/ConsumedWindow.hpp"

namespace replay_tests {
void testInPlaceArchive() {
    using namespace clipture;
    ScratchDirectory scratch;
    auto archive = std::make_shared<replay::InPlacePacketArchive>();
    archive->configure(scratch.path, true, 4096 + 40);
    auto source = fixturePacket();
    auto first = archive->persist(source);
    auto second = archive->persist(source);
    require(first && second, "two prepared 20-byte slots fit the arena");
    const auto firstExtent = first->payloadReader->extent();
    const auto secondOffset = second->payloadReader->extent()->offset;
    require(!archive->persist(source), "pinned full arena must use fallback, never overwrite");
    PacketPayload expected(payloadSize(*first));
    require(readPayload(*first, 0, expected), "read original leased bytes");
    second.reset();
    source.payload = std::make_shared<PacketPayload>(*source.payload);
    source.payload->back() = std::byte{0x42};
    auto replacement = archive->persist(source);
    require(replacement && replacement->payloadReader->extent()->offset == secondOffset, "reuse a released slot");
    PacketPayload actual(expected.size());
    require(readPayload(*first, 0, actual) && actual == expected, "reusing another slot preserves pinned media");
    for (int i = 0; i < 1000; ++i) {
        replacement.reset();
        replacement = archive->persist(source);
        require(replacement.has_value(), "repeated retention/reuse stays bounded");
    }
    require(archive->diskBytes() == 4096 + 40, "arena does not grow across repeated reuse");
    auto saving = archive->takeForSave();
    require(saving != nullptr, "detach frozen file without stopping recording");
    auto next = archive->persist(source);
    require(next.has_value(), "new file accepts next recording sample");
    const auto oldFile = std::dynamic_pointer_cast<const replay::InPlaceExtent>(firstExtent->source)->file();
    const auto nextFile = std::dynamic_pointer_cast<const replay::InPlaceExtent>(next->payloadReader->extent()->source)->file();
    require(oldFile != nextFile, "save and next recording own different files");
    require(firstExtent->source->read(firstExtent->offset, actual) && actual == expected, "frozen file remains readable");
    archive->configure(scratch.path, false, 4096 + 40);
    require(!archive->persist(source) && !archive->takeForSave(), "opt-out restores legacy persistence");

    // Exercise the real spill-worker integration, including the file-full fallback.
    archive->configure(scratch.path, true, 4096 + 20);
    ReplaySegmentStoreOptions options;
    options.rootDirectory = scratch.path / "legacy-fallback";
    options.residentPayloadBudgetBytes = 0;
    options.retention100ns = 120LL * 10'000'000;
    ReplaySegmentStore store(options);
    store.setInPlaceArchive(archive);
    store.start();
    store.push(source);
    source.pts100ns += 333333;
    store.push(source);
    require(store.waitUntilIdle(std::chrono::seconds(5)), "archive and fallback both persist");
    const auto snapshot = store.snapshot();
    require(snapshot.size() == 2 && !snapshot[0].payload && !snapshot[1].payload,
        "both paths release resident payloads");
    require(snapshot[0].codec == PacketCodec::H264Avcc && snapshot[1].codec == PacketCodec::H264AnnexB,
        "full arena preserves the original fallback representation");
    store.clear();
    store.stop();
    require(readPayload(snapshot[0], 0, actual), "retired store snapshot pins remain valid");

    replay::ConsumedWindow consumed;
    require(consumed.start() == 0, "default begins with full available buffer");
    consumed.commit(120LL * 10'000'000);
    require(consumed.start() == 120LL * 10'000'000, "successful save consumes its presentation");
    consumed.commit(10LL * 10'000'000);
    require(consumed.start() == 120LL * 10'000'000, "stale completion cannot move boundary backward");
    consumed.enable(true);
    require(consumed.start() == 120LL * 10'000'000, "ordinary reconfiguration must not reset consumption");
    consumed.enable(false);
    require(consumed.start() == 0, "explicit opt-out allows overlapping windows");
}
}

#include "ExtentTestSupport.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/Mp4SamplePacker.hpp"

namespace replay_tests {
using namespace clipture;
using namespace clipture::replay;

void testExtentStore() {
    ScratchDirectory scratchDirectory;
    PayloadLayoutPlan firstSave, overlappingSave;
    std::weak_ptr<const PayloadExtentSource> pin;
    const auto original = fixturePacket();
    const auto packed = packMp4VideoSample(original);
    require(packed.has_value(), "prepared fixture");
    {
        ReplaySegmentStoreOptions options;
        options.rootDirectory = scratchDirectory.path / "extent-store";
        options.prepareMp4Samples = true;
        options.residentPayloadBudgetBytes = 0;
        options.targetSegmentBytes = 1; // Each keyframe rotates to a distinct backing.
        ReplaySegmentStore store(options);
        store.start();
        store.push(original);
        auto next = original;
        next.pts100ns += original.duration100ns;
        next.dts100ns = next.pts100ns;
        store.push(next);
        require(store.waitUntilIdle(std::chrono::seconds(5)), "persist extents");
        const auto snapshot = store.snapshot();
        require(snapshot.size() == 2, "two disk samples");
        const auto one = snapshot[0].payloadReader->extent();
        const auto two = snapshot[1].payloadReader->extent();
        require(one && two && one->source != two->source, "rotated segments expose independent pins");
        const std::vector<PayloadExtent> selected{*one, *two};
        firstSave = planPayloadLayout(selected, 16, 8);
        overlappingSave = planPayloadLayout(std::span(&selected[1], 1), 16, 8);
        pin = two->source;
        store.clear();
        store.stop();
    } // Readers, snapshots, worker and store are all gone. Only plans pin files.
    require(!pin.expired(), "plans retain backing after buffer retirement and destruction");
    VectorSink first(static_cast<std::size_t>(firstSave.outputEnd()));
    std::array<std::byte, 5> scratch;
    require(copyPayload(firstSave, first, scratch).ok(), "copy retired source through plan");
    const auto& expected = *packed->payload;
    require(std::equal(expected.begin(), expected.end(), first.output.begin() + 16) &&
            std::equal(expected.begin(), expected.end(), first.output.begin() + 16 + expected.size()),
            "real prepared bytes match both selected samples");
    firstSave = {};
    require(!pin.expired(), "overlapping save keeps its own pin");
    VectorSink overlap(static_cast<std::size_t>(overlappingSave.outputEnd()));
    require(copyPayload(overlappingSave, overlap, scratch).ok(), "second save survives first releasing pins");
    overlappingSave = {};
    require(pin.expired(), "backing released when last plan ends");
    require(std::filesystem::is_empty(scratchDirectory.path / "extent-store"),
            "owned replay files and session directory reclaimed only after last pin");
}

} // namespace replay_tests

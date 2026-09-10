#include "TestSupport.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"
#include "clipture/replay/ConsumedWindow.hpp"
#include <deque>

namespace replay_tests {
void verifyInPlaceRollingFixture(const std::vector<clipture::EncodedPacket>& source, const std::filesystem::path& root) {
    using namespace clipture;
    require(std::filesystem::create_directory(root), "exclusive rolling fixture root");
    replay::InPlacePacketArchive archive;
    archive.configure(root, true, 2 * 1024 * 1024);
    std::deque<EncodedPacket> retained;
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& packet : source) {
            auto input = packet;
            input.pts100ns += pass * 122LL * 10'000'000;
            input.dts100ns = input.pts100ns;
            while (!retained.empty() && retained.front().pts100ns < input.pts100ns - 16LL * 10'000'000)
                retained.pop_front();
            auto stored = archive.persist(input);
            require(stored.has_value(), "rolling file must reuse slots without falling back");
            retained.push_back(std::move(*stored));
        }
    }
    const auto end = retained.back().pts100ns + retained.back().duration100ns;
    auto selected = replay::selectReplayVideo({retained.begin(), retained.end()}, 15, end);
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = end - 15LL * 10'000'000;
    pacing.presentationEndPts100ns = end;
    const auto compact = muxH264ToMp4(selected, (root / "compact").string(), 160, 90, 30, 10, pacing);
    require(compact.ok, "rolling compact reference");
    auto session = archive.takeForSave();
    require(session != nullptr, "seal reused arena");
    const auto before = session->io();
    pacing.experimentalInPlace = session.get();
    const auto saved = muxH264ToMp4(selected, (root / "in-place").string(), 160, 90, 30, 10, pacing);
    const auto after = session->io();
    require(saved.ok && after.mediaWritten == before.mediaWritten, "reused media needs no Save rewrite");
    require(std::filesystem::file_size(saved.filePath) < 2 * 1024 * 1024, "366 seconds of recording has bounded final file size");
}
}

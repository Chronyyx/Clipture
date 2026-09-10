#include "TestSupport.hpp"
#include "clipture/replay/InPlaceMediaBuffer.hpp"
#include "clipture/platform/windows/InPlaceFile.hpp"
#include "clipture/mux/InPlaceMp4Header.hpp"
#include "clipture/Mp4Muxer.hpp"
#include <array>
#include <iostream>

namespace replay_tests {
namespace {
using namespace clipture;
FILE_ID_INFO identity(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open identity handle");
    FILE_ID_INFO result{};
    const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &result, sizeof(result)) != FALSE;
    CloseHandle(handle);
    require(ok, "query file identity");
    return result;
}
void sameIdentity(const FILE_ID_INFO& before, const std::filesystem::path& after) {
    const auto current = identity(after);
    require(before.VolumeSerialNumber == current.VolumeSerialNumber &&
        std::memcmp(before.FileId.Identifier, current.FileId.Identifier, sizeof(before.FileId.Identifier)) == 0,
        "publication must rename the same file, not copy media");
}
}

void testInPlace() {
    using namespace clipture;
    ScratchDirectory scratch;
    const auto privatePath = scratch.path / "private.recording";
    replay::InPlaceMediaBuffer buffer(privatePath);
    require(!platform::windows::InPlaceFile::create(privatePath), "never replace an existing recording");
    require(!buffer.freeze(), "empty buffer cannot freeze");
    auto packet = fixturePacket();
    require(buffer.append(packet), "append prepared sample");
    require(!buffer.append(packet), "reject duplicate timestamp");
    auto changed = packet;
    changed.pts100ns += changed.duration100ns;
    changed.encoderEpoch++;
    require(!buffer.append(changed), "reject encoder epoch change");
    changed = packet;
    changed.pts100ns += changed.duration100ns;
    changed.encodedWidth++;
    require(!buffer.append(changed), "reject dimension change");
    auto snapshot = buffer.snapshot();
    const std::array<const EncodedPacket*, 1> selection{&snapshot.front()};
    require(!buffer.layout(selection), "cannot finalize an active file");
    require(!buffer.publish(scratch.path / "early.mp4"), "cannot publish before finalization");
    require(buffer.freeze() && buffer.freeze(), "freeze is idempotent before finalize");
    require(!buffer.append(changed), "frozen recording cannot accept new media");
    const auto layout = buffer.layout(selection);
    require(layout && layout->sampleOffsets.front() == 4096, "reserved header precedes media");
    replay::InPlaceMediaBuffer other(scratch.path / "other.recording");
    require(other.append(packet) && other.freeze() && other.layout(selection).has_value(), "foreign preroll can be copied without modifying its source");
    const auto beforeId = identity(privatePath);
    const auto before = buffer.io();
    const std::array<uint8_t, 8> ftyp{0, 0, 0, 8, 'f', 't', 'y', 'p'};
    bool invalidHeader = false;
    try { mux::inPlaceMp4Header(ftyp, 24, 100); }
    catch (const std::invalid_argument&) { invalidHeader = true; }
    require(invalidHeader, "reject insufficient reservation");
    const auto largeHeader = mux::inPlaceMp4Header(ftyp, 4096, 4096 + (1ULL << 33));
    uint64_t largeMdat = 0;
    for (std::size_t i = 4088; i < 4096; ++i) largeMdat = (largeMdat << 8) | std::to_integer<uint8_t>(largeHeader[i]);
    require(largeMdat == (1ULL << 33) + 16, "mdat size stays 64-bit above 4 GiB");
    const auto header = mux::inPlaceMp4Header(ftyp, layout->mediaStart, layout->mediaEnd);
    const auto index = bytes({0, 0, 0, 8, 'm', 'o', 'o', 'v'}); // Ownership test, not a decodable fixture.
    require(!buffer.finalize({}, index), "reject wrong reservation size without changing file");
    require(buffer.finalize(header, index), "finalize reserved prefix and index");
    require(!buffer.finalize(header, index), "no second finalization allowed");
    const auto after = buffer.io();
    require(after.mediaWritten == before.mediaWritten && after.bytesRead == before.bytesRead,
        "finalization must neither read nor rewrite media");
    require(after.metadataWritten - before.metadataWritten == 4096 + index.size(), "only metadata written");
    const auto collision = scratch.path / "existing.mp4";
    { std::ofstream output(collision); output << "preserve"; }
    const auto existing = readFile(collision);
    require(!buffer.publish(collision), "rename must not overwrite an existing clip");
    require(readFile(collision) == existing && std::filesystem::exists(privatePath), "failed rename retains both files");
    const auto destination = scratch.path / "final.mp4";
    require(buffer.publish(destination), "retry rename to unused destination");
    sameIdentity(beforeId, destination);
    const auto relocated = scratch.path / "published-by-host.mp4";
    std::filesystem::rename(destination, relocated);
    sameIdentity(beforeId, relocated);
    require(!std::filesystem::exists(privatePath), "private name removed by rename");
    require(!buffer.publish(scratch.path / "again.mp4"), "publication cannot repeat");
    PacketPayload payload(payloadSize(snapshot.front()));
    require(snapshot.front().payloadReader->read(0, payload), "pinned samples readable after rename");
    std::vector<EncodedPacket> retained;
    const auto abandoned = scratch.path / "unfinished.recording";
    {
        replay::InPlaceMediaBuffer unfinished(abandoned);
        require(unfinished.append(packet), "append before simulated owner shutdown");
        retained = unfinished.snapshot();
    }
    require(std::filesystem::exists(abandoned) && retained.front().payloadReader->read(0, payload),
        "owner shutdown retains media and readable snapshot pins");
    retained.clear();
    require(std::filesystem::exists(abandoned), "last pin release must not delete unfinished media");
    std::cout << "In-place ownership, freeze, metadata-only finalize and collision-safe rename passed.\n";
}

void verifyInPlaceFixture(const std::vector<clipture::EncodedPacket>& packets, const std::filesystem::path& root) {
    using namespace clipture;
    require(std::filesystem::create_directory(root), "exclusive in-place fixture directory");
    const auto privatePath = root / "buffer.recording";
    replay::InPlaceMediaBuffer buffer(privatePath);
    for (const auto& packet : packets) require(buffer.append(packet), "append real sample before Save");
    require(buffer.freeze(), "freeze real video recording");
    const auto beforeId = identity(privatePath);
    const auto before = buffer.io();
    const auto snapshot = buffer.snapshot();
    MuxWritePacing pacing;
    pacing.presentationStartPts100ns = packets.front().pts100ns + 15'000'000;
    pacing.presentationEndPts100ns = pacing.presentationStartPts100ns + 120LL * 10'000'000;
    pacing.experimentalInPlace = &buffer;
    pacing.analyzeIo = true;
    const auto result = muxH264ToMp4(snapshot, (root / "output").string(), 160, 90, 30, 10, pacing);
    require(result.ok, "real in-place mux must succeed");
    const auto after = buffer.io();
    require(before.mediaWritten > 0 && after.mediaWritten == before.mediaWritten && after.bytesRead == before.bytesRead,
        "entire Save, including metadata preparation, must not read/copy media");
    require(after.metadataWritten > before.metadataWritten, "Save writes MP4 metadata");
    require(!std::filesystem::exists(privatePath), "publication consumes the private name");
    sameIdentity(beforeId, result.filePath);
    require(std::filesystem::file_size(result.filePath) == result.ioAnalysis.finalFileBytes, "reported final size");
    std::cout << "In-place real fixture: same file ID, zero Save media reads/writes.\n";
}
} // namespace replay_tests

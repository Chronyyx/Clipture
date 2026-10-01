#pragma once
#include "clipture/PacketRingBuffer.hpp"
#include "clipture/replay/InPlaceIo.hpp"
#include <chrono>
#include <filesystem>

namespace clipture::platform::windows { class InPlaceFile; }

namespace clipture::replay {
struct InPlaceMediaLayout {
    uint64_t mediaStart = 0, mediaEnd = 0;
    std::vector<uint64_t> sampleOffsets;
};
// Save-time media placement/finalization. Production adopts a detached arena;
// the path constructor/append API supports small append-only fixtures. Rolling
// retention/reuse belongs to InPlacePacketArchive and the replay stores.
class InPlaceMediaBuffer {
public:
    explicit InPlaceMediaBuffer(const std::filesystem::path& privatePath);
    explicit InPlaceMediaBuffer(std::shared_ptr<platform::windows::InPlaceFile> sealedFile);
    InPlaceMediaBuffer(const InPlaceMediaBuffer&) = delete;
    InPlaceMediaBuffer& operator=(const InPlaceMediaBuffer&) = delete;
    bool append(const EncodedPacket& packet);
    bool freeze();
    std::vector<EncodedPacket> snapshot() const { return packets_; }
    std::optional<InPlaceMediaLayout> layout(std::span<const EncodedPacket* const> samples);
    // One video sample: reuses its offset when already in this file, else
    // packs and appends it. Callers placing several tracks should go in time
    // order so appended audio lands next to its video.
    std::optional<uint64_t> placeVideo(const EncodedPacket& packet);
    uint64_t mediaStart() const;
    std::optional<uint64_t> placeSample(const PacketPayloadReaderPtr& reader,
        std::span<const std::byte> memory, std::size_t length);
    uint64_t mediaEnd() const;
    // Paces samples copied at save time (footage a quick re-save needs that the
    // backfill had not reached). Zero keeps copies unpaced. Reused samples are free.
    void setCopyPace(uint64_t bytesPerSecond);
    uint64_t copiedBytes() const { return copiedBytes_; }
    bool finalize(std::span<const std::byte> prefix, std::span<const std::byte> movieIndex);
    bool publish(const std::filesystem::path& destination);
    InPlaceIo io() const;
private:
    std::shared_ptr<platform::windows::InPlaceFile> file_;
    std::vector<EncodedPacket> packets_;
    bool frozen_ = false;
    uint64_t copyBytesPerSecond_ = 0;
    uint64_t copiedBytes_ = 0;
    std::chrono::steady_clock::time_point copyStartedAt_{};
    void paceCopy(std::size_t bytes);
};
} // namespace clipture::replay

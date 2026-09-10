#pragma once
#include "clipture/PacketRingBuffer.hpp"
#include "clipture/replay/InPlaceIo.hpp"
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
    std::optional<uint64_t> placeSample(const PacketPayloadReaderPtr& reader,
        std::span<const std::byte> memory, std::size_t length);
    uint64_t mediaEnd() const;
    bool finalize(std::span<const std::byte> prefix, std::span<const std::byte> movieIndex);
    bool publish(const std::filesystem::path& destination);
    InPlaceIo io() const;
private:
    std::shared_ptr<platform::windows::InPlaceFile> file_;
    std::vector<EncodedPacket> packets_;
    bool frozen_ = false;
};
} // namespace clipture::replay

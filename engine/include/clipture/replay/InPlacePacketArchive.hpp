#pragma once
#include "clipture/PacketRingBuffer.hpp"
#include "clipture/replay/InPlaceMediaBuffer.hpp"
#include <filesystem>

namespace clipture::replay {
// Shared video/AAC disk destination. Existing replay stores own retention and
// leases; a released range may be reused, a pinned range may never be overwritten.
class InPlacePacketArchive {
public:
    InPlacePacketArchive();
    ~InPlacePacketArchive();
    void configure(const std::filesystem::path& folder, bool enabled, uint64_t maximumBytes);
    std::optional<EncodedPacket> persist(const EncodedPacket& packet);
    std::unique_ptr<InPlaceMediaBuffer> takeForSave();
    uint64_t diskBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

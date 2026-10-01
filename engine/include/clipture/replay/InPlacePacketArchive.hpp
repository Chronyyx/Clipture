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
    // Overlap backfill: a packet still leased from a detached (saved) file is
    // copied into the active arena so a later save can reuse it without copying.
    bool isDetached(const EncodedPacket& packet) const;
    // Copies a run of packets (ascending, as read order) into one contiguous
    // arena range. Entries are nullopt when not detached or not copied.
    std::vector<std::optional<EncodedPacket>> rehome(std::span<const EncodedPacket> packets);
    std::unique_ptr<InPlaceMediaBuffer> takeForSave();
    uint64_t diskBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

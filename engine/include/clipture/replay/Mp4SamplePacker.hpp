#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <optional>

namespace clipture::replay {

// Converts one resident Annex B packet into the exact video sample bytes used
// by the existing MP4 muxer. Never mutates the input or performs disk I/O.
// Unsupported, malformed, parameter-only or oversized packets return nullopt;
// the caller must retain the original packet as its compatibility fallback.
std::optional<EncodedPacket> packMp4VideoSample(const EncodedPacket& source);

}  // namespace clipture::replay

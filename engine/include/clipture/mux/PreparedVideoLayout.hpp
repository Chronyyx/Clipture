#pragma once

#include "clipture/PacketRingBuffer.hpp"
#include "clipture/replay/PayloadLayoutPlan.hpp"

namespace clipture::mux {

// Eligibility for the experimental video-only layout. Caller must exclude audio
// tracks requiring separate layout. No guessed source offsets or RAM conversion.
// Invalid alignment and unsupported samples select the existing compact path.
std::optional<replay::PayloadLayoutPlan> preparedVideoLayout(
    std::span<const EncodedPacket* const> samples, uint64_t outputStart, uint64_t alignment);

} // namespace clipture::mux

#include "clipture/mux/PreparedVideoLayout.hpp"

namespace clipture::mux {

std::optional<replay::PayloadLayoutPlan> preparedVideoLayout(
    std::span<const EncodedPacket* const> samples, uint64_t outputStart, uint64_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || samples.empty()) return std::nullopt;
    std::vector<replay::PayloadExtent> extents;
    extents.reserve(samples.size());
    for (const auto* packet : samples) {
        if (!packet || packet->kind != PacketKind::Video || packet->codec != PacketCodec::H264Avcc ||
            packet->payload || !packet->payloadReader) return std::nullopt;
        auto extent = packet->payloadReader->extent();
        if (!extent || !extent->source || extent->length != payloadSize(*packet)) return std::nullopt;
        extents.push_back(std::move(*extent));
    }
    return replay::planPayloadLayout(extents, outputStart, alignment, replay::PayloadPlacement::AlignRegions);
}

} // namespace clipture::mux

#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace clipture {

inline void prepareAudioReplayPacket(
    EncodedPacket& packet,
    const std::map<std::string, std::string>& sourceToLogicalTrack) {
    packet.kind = PacketKind::Audio;
    if (packet.codec == PacketCodec::Unknown) packet.codec = PacketCodec::PcmS16;
    if (packet.audioFrameCount == 0 && packet.channelCount > 0) {
        packet.audioFrameCount = static_cast<uint32_t>(
            payloadSize(packet) / (sizeof(int16_t) * static_cast<std::size_t>(packet.channelCount)));
    }

    packet.audible = false;
    const auto bytes = payloadBytes(packet);
    const auto* samples = reinterpret_cast<const int16_t*>(bytes.data());
    const std::size_t sampleCount = bytes.size() / sizeof(int16_t);
    for (std::size_t i = 0; i < sampleCount; ++i) {
        if (samples[i] != 0) {
            packet.audible = true;
            break;
        }
    }

    const auto route = sourceToLogicalTrack.find(packet.sourceId);
    packet.logicalTrackId = route == sourceToLogicalTrack.end() ? std::string {} : route->second;
}

}  // namespace clipture

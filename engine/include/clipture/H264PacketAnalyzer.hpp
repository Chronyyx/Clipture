#pragma once

#include "clipture/PacketRingBuffer.hpp"

namespace clipture {

bool analyzeH264Packet(EncodedPacket& packet);

}  // namespace clipture

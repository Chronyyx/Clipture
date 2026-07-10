#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <functional>
#include <string>
#include <vector>

namespace clipture {

struct MuxResult {
    bool ok = false;
    std::string message;
    std::string filePath;
};

struct MuxWritePacing {
    std::function<bool()> shouldPace;
};

MuxResult muxH264ToMp4(
    const std::vector<EncodedPacket>& packets,
    const std::string& saveFolder,
    int width,
    int height,
    int fps,
    int bitrateMbps,
    MuxWritePacing pacing = {});

}  // namespace clipture

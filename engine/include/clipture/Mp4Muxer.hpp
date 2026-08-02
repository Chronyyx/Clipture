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

enum class MuxPressureLevel {
    Healthy,
    Elevated,
    Critical
};

struct MuxPressureSample {
    MuxPressureLevel level = MuxPressureLevel::Healthy;
    std::size_t frameQueueDepth = 0;
    int64_t oldestFrameAge100ns = 0;
    int encoderQueueDepth = 0;
    int nvencInFlight = 0;
    int64_t captureGap100ns = 0;
    int64_t capturePublicationAge100ns = 0;
    int droppedFramesDelta = 0;
};

struct MuxWritePacing {
    std::function<MuxPressureSample()> samplePressure;
    int64_t presentationStartPts100ns = 0;
    int64_t presentationEndPts100ns = 0;
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

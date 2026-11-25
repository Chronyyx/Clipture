#include "clipture/ClipContentCheck.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace clipture {
namespace {
constexpr double kFrozenMinimumSeconds = 10.0;
constexpr uint64_t kFrozenMaximumSourceFrames = 2;
constexpr uint64_t kBlankBytesPerPixelDivisor = 256;

uint64_t packetBytes(const EncodedPacket& packet) {
    if (packet.h264.analyzed && packet.h264.avccSampleSize > 0) return packet.h264.avccSampleSize;
    if (packet.payload) return packet.payload->size();
    if (packet.payloadReader) return packet.payloadReader->size();
    return 0;
}
}  // namespace

ClipContentCheck checkClipContent(std::span<const EncodedPacket> videoPackets) {
    ClipContentCheck result;
    if (videoPackets.empty()) return result;
    result.analyzed = true;
    uint64_t totalBytes = 0;
    uint64_t previousSequence = 0;
    for (const auto& packet : videoPackets) {
        const uint64_t bytes = packetBytes(packet);
        totalBytes += bytes;
        ++result.frames;
        if (packet.keyframe) {
            ++result.keyframes;
            result.maxKeyframeBytes = std::max(result.maxKeyframeBytes, bytes);
        }
        if (packet.sourceFrameSequence != 0 && packet.sourceFrameSequence != previousSequence) {
            ++result.distinctSourceFrames;
            previousSequence = packet.sourceFrameSequence;
        }
        if (!result.width && packet.encodedWidth > 0) {
            result.width = packet.encodedWidth;
            result.height = packet.encodedHeight;
        }
    }
    const auto& first = videoPackets.front();
    const auto& last = videoPackets.back();
    result.spanSeconds = static_cast<double>(std::max<int64_t>(0, last.pts100ns - first.pts100ns + last.duration100ns)) / 10'000'000.0;
    result.meanFrameBytes = totalBytes / result.frames;

    const uint64_t pixels = static_cast<uint64_t>(result.width > 0 ? result.width : 1920) *
        static_cast<uint64_t>(result.height > 0 ? result.height : 1080);
    result.blank = result.keyframes > 0 && result.maxKeyframeBytes * kBlankBytesPerPixelDivisor < pixels;
    result.frozen = result.spanSeconds >= kFrozenMinimumSeconds && result.distinctSourceFrames > 0 &&
        result.distinctSourceFrames <= kFrozenMaximumSourceFrames;
    return result;
}

std::string toJson(const ClipContentCheck& content, const ClipSourceFacts& source) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "{\"blank\":" << (content.blank ? "true" : "false")
        << ",\"frozen\":" << (content.frozen ? "true" : "false")
        << ",\"spanSeconds\":" << content.spanSeconds
        << ",\"frames\":" << content.frames
        << ",\"keyframes\":" << content.keyframes
        << ",\"distinctSourceFrames\":" << content.distinctSourceFrames
        << ",\"maxKeyframeBytes\":" << content.maxKeyframeBytes
        << ",\"meanFrameBytes\":" << content.meanFrameBytes
        << ",\"recordedSource\":\"" << (source.recordingGameWindow ? "game-window" : "monitor") << "\""
        << ",\"sourceSeconds\":" << source.sourceSeconds
        << ",\"recordedMonitorPrimary\":" << (source.recordedMonitorPrimary ? "true" : "false")
        << ",\"foregroundOnRecordedMonitor\":" << (source.foregroundOnRecordedMonitor ? "true" : "false")
        << "}";
    return out.str();
}

}  // namespace clipture

#include "clipture/Mp4Muxer.hpp"
#include "clipture/AacEncoderSession.hpp"
#include "clipture/BoundedWrite.hpp"
#include "clipture/mux/PreparedVideoLayout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace clipture::mux {

// Uses the existing mux writer's buffering, failure state, pacing and metrics.
// Padding is zero-filled inside mdat but outside every referenced sample.
template<class Writer>
void writePadding(Writer& writer, uint64_t bytes) {
    const std::array<std::byte, 4096> zeros{};
    while (bytes) {
        const auto count = static_cast<std::size_t>(std::min<uint64_t>(bytes, zeros.size()));
        writer.write(std::span(zeros).first(count));
        bytes -= count;
    }
}

} // namespace clipture::mux
#include "clipture/mux/InPlaceMp4Header.hpp"
#include "clipture/replay/InPlaceMediaBuffer.hpp"


#include <algorithm>
#include <cstdint>
#include <limits>

namespace clipture {

inline uint32_t videoSampleDuration100ns(int64_t pts100ns, int64_t nextPts100ns) {
    const int64_t duration = nextPts100ns > pts100ns
        ? nextPts100ns - pts100ns
        : 1;
    return static_cast<uint32_t>(std::clamp<int64_t>(
        duration,
        1,
        std::numeric_limits<uint32_t>::max()));
}

inline uint32_t finalVideoSampleDuration100ns(int64_t packetDuration100ns, int fps) {
    const int64_t duration = packetDuration100ns > 0
        ? packetDuration100ns
        : 10'000'000LL / std::max(1, fps);
    return static_cast<uint32_t>(std::clamp<int64_t>(
        duration,
        1,
        std::numeric_limits<uint32_t>::max()));
}

}  // namespace clipture

#include <Windows.h>
#include <winioctl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <thread>
#include <utility>

namespace clipture {
namespace {
#include "Mp4MuxTracks.inl"  // samples, tracks, AAC and the audio timeline
#include "Mp4MuxBoxes.inl"  // H.264 NAL parsing and MP4 boxes
#include "Mp4MuxWriters.inl"  // file writers and save I/O accounting
}  // namespace

std::string saveIoAnalysisToJson(const SaveIoAnalysis& analysis) {
    if (!analysis.enabled) return "null";

    std::ostringstream out;
    auto key = [&](const char* name) {
        out << '"' << name << '"' << ':';
    };
    out << '{';
    key("bucketMs"); out << analysis.bucketMs << ',';
    key("elapsedMs"); out << analysis.elapsedMs << ',';
    key("timelineTruncated"); out << (analysis.timelineTruncated ? "true" : "false") << ',';
    key("omittedSlowOperations"); out << analysis.omittedSlowOperations << ',';
    key("storageSeekPenalty"); out << '"' << analysis.storageSeekPenalty << '"' << ',';
    key("ioPriority"); out << '"' << analysis.ioPriority << '"' << ',';
    key("lowIoPriorityApplied"); out << (analysis.lowIoPriorityApplied ? "true" : "false") << ',';
    key("preallocated"); out << (analysis.preallocated ? "true" : "false") << ',';
    key("finalFileBytes"); out << analysis.finalFileBytes << ',';
    key("diskBackedSourceBytes"); out << analysis.diskBackedSourceBytes << ',';
    key("processReadOperationsDelta"); out << analysis.processReadOperationsDelta << ',';
    key("processWriteOperationsDelta"); out << analysis.processWriteOperationsDelta << ',';
    key("processReadBytesDelta"); out << analysis.processReadBytesDelta << ',';
    key("processWriteBytesDelta"); out << analysis.processWriteBytesDelta << ',';
    key("processOtherBytesDelta"); out << analysis.processOtherBytesDelta << ',';
    key("timeline"); out << '[';
    for (std::size_t i = 0; i < analysis.timeline.size(); ++i) {
        if (i > 0) out << ',';
        const auto& bucket = analysis.timeline[i];
        out << '{';
        key("startMs"); out << bucket.startMs << ',';
        key("replayReadBytes"); out << bucket.replayReadBytes << ',';
        key("outputWriteBytes"); out << bucket.outputWriteBytes << ',';
        key("replayReadBusyUs"); out << bucket.replayReadBusyUs << ',';
        key("outputWriteBusyUs"); out << bucket.outputWriteBusyUs << ',';
        key("pacingWaitUs"); out << bucket.pacingWaitUs << ',';
        key("replayReadCalls"); out << bucket.replayReadCalls << ',';
        key("outputWriteCalls"); out << bucket.outputWriteCalls << ',';
        key("pacingWaitCalls"); out << bucket.pacingWaitCalls << ',';
        key("maximumReplayReadUs"); out << bucket.maximumReplayReadUs << ',';
        key("maximumOutputWriteUs"); out << bucket.maximumOutputWriteUs << ',';
        key("maximumPacingWaitUs"); out << bucket.maximumPacingWaitUs << ',';
        key("pressureLevel"); out << bucket.pressureLevel << ',';
        key("frameQueueDepth"); out << bucket.frameQueueDepth << ',';
        key("encoderQueueDepth"); out << bucket.encoderQueueDepth << ',';
        key("nvencInFlight"); out << bucket.nvencInFlight << ',';
        key("droppedFramesDelta"); out << bucket.droppedFramesDelta << ',';
        key("captureGap100ns"); out << bucket.captureGap100ns << ',';
        key("capturePublicationAge100ns"); out << bucket.capturePublicationAge100ns;
        out << '}';
    }
    out << ']' << ',';
    key("slowOperations"); out << '[';
    for (std::size_t i = 0; i < analysis.slowOperations.size(); ++i) {
        if (i > 0) out << ',';
        const auto& operation = analysis.slowOperations[i];
        out << '{';
        key("operation"); out << '"' << operation.operation << '"' << ',';
        key("startMs"); out << operation.startMs << ',';
        key("durationUs"); out << operation.durationUs << ',';
        key("bytes"); out << operation.bytes;
        out << '}';
    }
    out << ']' << '}';
    return out.str();
}

#include "Mp4MuxClip.inl"  // muxH264ToMp4

}  // namespace clipture

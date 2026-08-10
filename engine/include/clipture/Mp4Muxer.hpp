#pragma once

#include "clipture/BoundedWrite.hpp"
#include "clipture/PacketRingBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace clipture {

struct SaveIoTimelineBucket {
    uint32_t startMs = 0;
    uint64_t replayReadBytes = 0;
    uint64_t outputWriteBytes = 0;
    uint64_t replayReadBusyUs = 0;
    uint64_t outputWriteBusyUs = 0;
    uint64_t pacingWaitUs = 0;
    uint32_t replayReadCalls = 0;
    uint32_t outputWriteCalls = 0;
    uint32_t pacingWaitCalls = 0;
    uint64_t maximumReplayReadUs = 0;
    uint64_t maximumOutputWriteUs = 0;
    uint64_t maximumPacingWaitUs = 0;
    int pressureLevel = 0;
    std::size_t frameQueueDepth = 0;
    int encoderQueueDepth = 0;
    int nvencInFlight = 0;
    int droppedFramesDelta = 0;
    int64_t captureGap100ns = 0;
    int64_t capturePublicationAge100ns = 0;
};

struct SaveIoSlowOperation {
    std::string operation;
    uint32_t startMs = 0;
    uint64_t durationUs = 0;
    uint64_t bytes = 0;
};

struct SaveIoAnalysis {
    bool enabled = false;
    uint32_t bucketMs = 50;
    uint64_t elapsedMs = 0;
    bool timelineTruncated = false;
    uint32_t omittedSlowOperations = 0;
    std::string storageSeekPenalty;
    std::string ioPriority;
    bool lowIoPriorityApplied = false;
    bool preallocated = false;
    uint64_t finalFileBytes = 0;
    uint64_t diskBackedSourceBytes = 0;
    uint64_t processReadOperationsDelta = 0;
    uint64_t processWriteOperationsDelta = 0;
    uint64_t processReadBytesDelta = 0;
    uint64_t processWriteBytesDelta = 0;
    uint64_t processOtherBytesDelta = 0;
    std::vector<SaveIoTimelineBucket> timeline;
    std::vector<SaveIoSlowOperation> slowOperations;
};

struct MuxResult {
    bool ok = false;
    std::string message;
    std::string filePath;
    SaveIoAnalysis ioAnalysis;
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
    AdaptiveWritePacerConfig adaptiveRate;
    std::size_t burstBytes = 0;
    bool storageAwareRate = false;
    bool analyzeIo = false;
};

std::string saveIoAnalysisToJson(const SaveIoAnalysis& analysis);

MuxResult muxH264ToMp4(
    const std::vector<EncodedPacket>& packets,
    const std::string& saveFolder,
    int width,
    int height,
    int fps,
    int bitrateMbps,
    MuxWritePacing pacing = {});

}  // namespace clipture

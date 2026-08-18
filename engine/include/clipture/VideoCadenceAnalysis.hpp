#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace clipture {

struct VideoCadenceBucket {
    int secondIndex = 0;
    int64_t duration100ns = 0;
    uint64_t sampleCount = 0;
    uint64_t distinctSourceFrames = 0;
    uint64_t repeatedSourceFrames = 0;
    uint64_t unknownSourceFrames = 0;
    uint64_t desktopPresentSourceFrames = 0;
    uint64_t pointerOnlySourceFrames = 0;
    uint64_t unknownUpdateKindSourceFrames = 0;
    int64_t maximumSampleGap100ns = 0;

    double distinctSourceFps() const {
        return duration100ns > 0
            ? static_cast<double>(distinctSourceFrames) * 10'000'000.0 /
                static_cast<double>(duration100ns)
            : 0.0;
    }

    double desktopPresentSourceFps() const {
        return duration100ns > 0
            ? static_cast<double>(desktopPresentSourceFrames) * 10'000'000.0 /
                static_cast<double>(duration100ns)
            : 0.0;
    }
};

struct VideoCadenceAnalysis {
    bool available = false;
    int targetFps = 0;
    int64_t span100ns = 0;
    int64_t targetFrameDuration100ns = 0;
    int64_t maximumSampleGap100ns = 0;
    uint64_t expectedOutputTicks = 0;
    uint64_t sampleCount = 0;
    uint64_t distinctSourceFrames = 0;
    uint64_t repeatedSourceFrames = 0;
    uint64_t unknownSourceFrames = 0;
    uint64_t desktopPresentSourceFrames = 0;
    uint64_t pointerOnlySourceFrames = 0;
    uint64_t unknownUpdateKindSourceFrames = 0;
    uint64_t longestHeldRunSamples = 0;
    uint64_t gapEvents = 0;
    uint64_t missingFrameSlots = 0;
    uint64_t underTargetSeconds = 0;
    uint64_t underTargetDesktopPresentSeconds = 0;
    double distinctSourceFps = 0.0;
    double desktopPresentSourceFps = 0.0;
    double repeatRatio = 0.0;
    double worstSecondDistinctSourceFps = 0.0;
    double worstSecondDesktopPresentSourceFps = 0.0;
    std::vector<VideoCadenceBucket> buckets;
};

inline VideoCadenceAnalysis analyzeVideoCadence(
    std::span<const EncodedPacket> packets,
    int targetFps) {
    VideoCadenceAnalysis result;
    result.targetFps = std::max(1, targetFps);
    result.targetFrameDuration100ns = 10'000'000LL / result.targetFps;
    if (packets.empty()) return result;

    const int64_t firstPts100ns = packets.front().pts100ns;
    const int64_t lastDuration100ns = std::max<int64_t>(
        packets.back().duration100ns,
        result.targetFrameDuration100ns);
    result.span100ns = std::max<int64_t>(
        1,
        packets.back().pts100ns - firstPts100ns + lastDuration100ns);
    result.sampleCount = packets.size();
    result.expectedOutputTicks = static_cast<uint64_t>(
        (result.span100ns + result.targetFrameDuration100ns / 2) /
        result.targetFrameDuration100ns);

    const std::size_t bucketCount = std::max<std::size_t>(
        1,
        static_cast<std::size_t>((result.span100ns + 9'999'999LL) / 10'000'000LL));
    result.buckets.resize(bucketCount);
    for (std::size_t index = 0; index < bucketCount; ++index) {
        auto& bucket = result.buckets[index];
        bucket.secondIndex = static_cast<int>(index);
        const int64_t bucketStart100ns = static_cast<int64_t>(index) * 10'000'000LL;
        bucket.duration100ns = std::clamp<int64_t>(
            result.span100ns - bucketStart100ns,
            0,
            10'000'000LL);
    }

    uint64_t previousSourceSequence = 0;
    uint64_t heldRunSamples = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto& packet = packets[index];
        const int64_t relativePts100ns = std::max<int64_t>(0, packet.pts100ns - firstPts100ns);
        const std::size_t bucketIndex = std::min<std::size_t>(
            result.buckets.size() - 1,
            static_cast<std::size_t>(relativePts100ns / 10'000'000LL));
        auto& bucket = result.buckets[bucketIndex];
        ++bucket.sampleCount;

        if (packet.sourceFrameSequence == 0) {
            ++result.unknownSourceFrames;
            ++bucket.unknownSourceFrames;
            previousSourceSequence = 0;
            heldRunSamples = 0;
        } else if (packet.sourceFrameSequence == previousSourceSequence) {
            ++result.repeatedSourceFrames;
            ++bucket.repeatedSourceFrames;
            ++heldRunSamples;
            result.longestHeldRunSamples = std::max(result.longestHeldRunSamples, heldRunSamples);
        } else {
            ++result.distinctSourceFrames;
            ++bucket.distinctSourceFrames;
            if (packet.sourceHadDesktopPresent) {
                ++result.desktopPresentSourceFrames;
                ++bucket.desktopPresentSourceFrames;
            } else if (packet.sourceHadPointerUpdate) {
                ++result.pointerOnlySourceFrames;
                ++bucket.pointerOnlySourceFrames;
            } else {
                ++result.unknownUpdateKindSourceFrames;
                ++bucket.unknownUpdateKindSourceFrames;
            }
            previousSourceSequence = packet.sourceFrameSequence;
            heldRunSamples = 1;
            result.longestHeldRunSamples = std::max(result.longestHeldRunSamples, heldRunSamples);
        }

        if (index == 0) continue;
        const int64_t gap100ns = std::max<int64_t>(
            1,
            packet.pts100ns - packets[index - 1].pts100ns);
        result.maximumSampleGap100ns = std::max(result.maximumSampleGap100ns, gap100ns);
        bucket.maximumSampleGap100ns = std::max(bucket.maximumSampleGap100ns, gap100ns);
        const int64_t missing = std::max<int64_t>(
            0,
            (gap100ns + result.targetFrameDuration100ns / 2) /
                result.targetFrameDuration100ns - 1);
        if (missing > 0) {
            ++result.gapEvents;
            result.missingFrameSlots += static_cast<uint64_t>(missing);
        }
    }

    const double spanSeconds = static_cast<double>(result.span100ns) / 10'000'000.0;
    result.distinctSourceFps = static_cast<double>(result.distinctSourceFrames) / spanSeconds;
    result.desktopPresentSourceFps =
        static_cast<double>(result.desktopPresentSourceFrames) / spanSeconds;
    result.repeatRatio = result.sampleCount > 0
        ? static_cast<double>(result.repeatedSourceFrames) /
            static_cast<double>(result.sampleCount)
        : 0.0;
    result.worstSecondDistinctSourceFps = result.buckets.front().distinctSourceFps();
    result.worstSecondDesktopPresentSourceFps =
        result.buckets.front().desktopPresentSourceFps();
    const double underTargetThreshold = static_cast<double>(result.targetFps) * 0.95;
    for (const auto& bucket : result.buckets) {
        const double bucketFps = bucket.distinctSourceFps();
        const double desktopPresentFps = bucket.desktopPresentSourceFps();
        result.worstSecondDistinctSourceFps = std::min(
            result.worstSecondDistinctSourceFps,
            bucketFps);
        result.worstSecondDesktopPresentSourceFps = std::min(
            result.worstSecondDesktopPresentSourceFps,
            desktopPresentFps);
        if (bucketFps < underTargetThreshold) ++result.underTargetSeconds;
        if (desktopPresentFps < underTargetThreshold) {
            ++result.underTargetDesktopPresentSeconds;
        }
    }
    result.available = true;
    return result;
}

}  // namespace clipture

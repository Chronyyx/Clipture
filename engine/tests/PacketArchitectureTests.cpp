#include "clipture/BoundedWrite.hpp"
#include "clipture/AudioTimeline.hpp"
#include "clipture/AudioPacketRouting.hpp"
#include "clipture/AudioProcessSpec.hpp"
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/CaptureBackendPolicy.hpp"
#include "clipture/DesktopDuplicationHelpers.hpp"
#include "clipture/DesktopPointerShape.hpp"
#include "clipture/CfrFrameScheduler.hpp"
#include "clipture/H264PacketAnalyzer.hpp"
#include "clipture/LatencyWindow.hpp"
#include "clipture/FixedRateFrameSampler.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/PcmSampleConverter.hpp"
#include "clipture/ProcessSnapshot.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/VideoTimeline.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <array>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <thread>
#include <vector>
#include <chrono>

namespace {

clipture::EncodedPacket packetFromBytes(std::initializer_list<uint8_t> values) {
    clipture::EncodedPacket packet;
    packet.kind = clipture::PacketKind::Video;
    packet.payload = std::make_shared<clipture::PacketPayload>();
    packet.payload->reserve(values.size());
    for (const auto value : values) packet.payload->push_back(static_cast<std::byte>(value));
    return packet;
}

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

bool testStartCodesAndFlags() {
    auto packet = packetFromBytes({
        0, 0, 1, 0x09, 0x10,
        0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28,
        0, 0, 1, 0x68, 0xEE,
        0, 0, 0, 1, 0x65, 0xAA, 0xBB,
        0, 0, 1, 0x01, 0xCC,
        0, 0, 1, 0x06, 0xDD
    });
    if (!require(clipture::analyzeH264Packet(packet), "mixed start-code packet should parse")) return false;
    if (!require(packet.codec == clipture::PacketCodec::H264AnnexB, "codec should be H.264 Annex B")) return false;
    if (!require(packet.h264.hasAud && packet.h264.hasSps && packet.h264.hasPps && packet.h264.hasIdr,
                 "AUD/SPS/PPS/IDR flags should be recorded")) return false;
    if (!require(packet.keyframe, "IDR should mark packet as keyframe")) return false;
    if (!require(packet.h264.inlineCount == 4, "first four NAL spans should be inline")) return false;
    if (!require(packet.h264.overflowNalus && packet.h264.overflowNalus->size() == 2,
                 "unusual multi-NAL packet should use shared overflow metadata")) return false;
    const uint32_t expectedAvccBytes = (4 + 3) + (4 + 2) + (4 + 2);
    return require(packet.h264.avccSampleSize == expectedAvccBytes, "AVCC size should exclude AUD/SPS/PPS");
}

bool testMalformedPackets() {
    auto malformed = packetFromBytes({0, 0, 0, 0, 0x65, 0xAA});
    if (!require(!clipture::analyzeH264Packet(malformed), "packet without a start code should be rejected")) return false;
    if (!require(malformed.h264.analyzed && malformed.h264.nalCount() == 0,
                 "malformed packet should still be marked analyzed")) return false;

    clipture::EncodedPacket empty;
    return require(!clipture::analyzeH264Packet(empty), "null payload should be rejected safely");
}

bool testBoundedWrites() {
    constexpr std::size_t maximum = 512u * 1024u;
    std::size_t remaining = 11u * 1024u * 1024u + 17u;
    std::size_t total = 0;
    std::size_t writes = 0;
    while (remaining > 0) {
        const std::size_t request = clipture::boundedWriteSize(remaining, maximum);
        if (!require(request > 0 && request <= maximum, "write request must remain within the hard cap")) return false;
        remaining -= request;
        total += request;
        ++writes;
    }
    if (!require(total == 11u * 1024u * 1024u + 17u && writes == 23,
                 "bounded writer should cover every byte without exceeding 512 KiB")) {
        return false;
    }
    if (!require(
            clipture::muxStagingBytesForSource(false, clipture::StorageSeekPenalty::Incurs) ==
                512u * 1024u,
            "RAM-only muxing should retain the small low-latency staging window")) {
        return false;
    }
    if (!require(
            clipture::muxStagingBytesForSource(true, clipture::StorageSeekPenalty::DoesNotIncur) ==
                    4u * 1024u * 1024u &&
                clipture::muxStagingBytesForSource(true, clipture::StorageSeekPenalty::Unknown) ==
                    16u * 1024u * 1024u &&
                clipture::muxStagingBytesForSource(true, clipture::StorageSeekPenalty::Incurs) ==
                    64u * 1024u * 1024u,
            "disk-backed mux staging should grow with the destination seek penalty")) {
        return false;
    }
    return true;
}

bool testAdaptiveWriteRateController() {
    if (!require(
            clipture::shouldUseAdaptiveWritePacing(
                true,
                clipture::StorageSeekPenalty::DoesNotIncur),
            "solid-state storage should bound sustained cache writes")) {
        return false;
    }
    if (!require(
            clipture::shouldUseAdaptiveWritePacing(
                true,
                clipture::StorageSeekPenalty::Incurs) &&
                clipture::shouldUseAdaptiveWritePacing(
                    true,
                    clipture::StorageSeekPenalty::Unknown),
            "seek-penalty and unknown storage should use adaptive pacing")) {
        return false;
    }
    if (!require(
            !clipture::shouldUseAdaptiveWritePacing(
                false,
                clipture::StorageSeekPenalty::Incurs),
            "adaptive pacing should remain opt-in for other mux callers")) {
        return false;
    }

    const auto solidStateConfig = clipture::writePacerConfigForStorage(
        {},
        clipture::StorageSeekPenalty::DoesNotIncur);
    if (!require(
            solidStateConfig.initialBytesPerSecond == 640ULL * 1024ULL * 1024ULL &&
                solidStateConfig.maximumLearnedBytesPerSecond == 768ULL * 1024ULL * 1024ULL &&
                solidStateConfig.adjustmentWindowBytes == 128u * 1024u * 1024u,
            "solid-state pacing should stay fast while bounding dirty-cache growth")) {
        return false;
    }

    clipture::SustainedWritePressureGate pressureGate;
    if (!require(
            pressureGate.update(clipture::AdaptiveWritePressure::Elevated, 0) ==
                clipture::AdaptiveWritePressure::Healthy &&
                pressureGate.update(clipture::AdaptiveWritePressure::Elevated, 49'999) ==
                    clipture::AdaptiveWritePressure::Healthy,
            "brief elevated pressure should not demote the save thread")) {
        return false;
    }
    if (!require(
            pressureGate.update(clipture::AdaptiveWritePressure::Elevated, 50'000) ==
                clipture::AdaptiveWritePressure::Elevated,
            "sustained elevated pressure should activate background protection")) {
        return false;
    }
    if (!require(
            pressureGate.update(clipture::AdaptiveWritePressure::Healthy, 60'000) ==
                clipture::AdaptiveWritePressure::Elevated &&
                pressureGate.update(clipture::AdaptiveWritePressure::Healthy, 309'999) ==
                    clipture::AdaptiveWritePressure::Elevated &&
                pressureGate.update(clipture::AdaptiveWritePressure::Healthy, 310'000) ==
                    clipture::AdaptiveWritePressure::Healthy,
            "background protection should require a stable healthy recovery window")) {
        return false;
    }
    if (!require(
            pressureGate.update(clipture::AdaptiveWritePressure::Critical, 310'001) ==
                clipture::AdaptiveWritePressure::Critical,
            "critical pressure should activate protection immediately")) {
        return false;
    }

    constexpr uint64_t mib = 1024ULL * 1024ULL;
    clipture::AdaptiveWritePacerConfig config;
    config.initialBytesPerSecond = 96 * mib;
    config.minimumBytesPerSecond = 16 * mib;
    config.maximumLearnedBytesPerSecond = 512 * mib;
    config.adjustmentWindowBytes = static_cast<std::size_t>(mib);
    config.targetUtilizationPercent = 75;
    config.minimumMeasuredWriteUs = 1;

    auto unmeasuredConfig = config;
    unmeasuredConfig.minimumMeasuredWriteUs = 1'000;
    clipture::AdaptiveWriteRateController unmeasuredController(unmeasuredConfig);
    unmeasuredController.observeWrite(static_cast<std::size_t>(mib), 100);
    if (!require(
            unmeasuredController.currentBytesPerSecond() ==
                unmeasuredConfig.initialBytesPerSecond,
            "cache-speed writes should not make the controller guess at physical throughput")) {
        return false;
    }

    clipture::AdaptiveWriteRateController controller(config);
    controller.observeWrite(static_cast<std::size_t>(mib), 4'000);
    const uint64_t healthyRate = controller.currentBytesPerSecond();
    if (!require(
            healthyRate > config.initialBytesPerSecond,
            "healthy fast writes should cautiously increase the learned rate")) {
        return false;
    }

    controller.observePressure(clipture::AdaptiveWritePressure::Elevated);
    const uint64_t elevatedRate = controller.currentBytesPerSecond();
    if (!require(
            elevatedRate < healthyRate,
            "elevated capture pressure should reduce write throughput immediately")) {
        return false;
    }
    controller.observePressure(clipture::AdaptiveWritePressure::Elevated);
    if (!require(
            controller.currentBytesPerSecond() == elevatedRate,
            "sustained elevated pressure should not repeatedly collapse the rate")) {
        return false;
    }

    controller.observePressure(clipture::AdaptiveWritePressure::Critical);
    const uint64_t criticalRate = controller.currentBytesPerSecond();
    if (!require(
            criticalRate < elevatedRate &&
                criticalRate >= controller.dynamicMinimumRate(),
            "critical pressure should back off without falling below the measured service floor")) {
        return false;
    }
    controller.observePressure(clipture::AdaptiveWritePressure::Healthy);
    controller.observeWrite(static_cast<std::size_t>(mib), 4'000);
    return require(
        controller.currentBytesPerSecond() > criticalRate &&
            controller.pressureBackoffs() == 2 &&
            controller.measuredWrites() == 2,
        "stable capture should recover gradually after pressure subsides");
}

bool testPcmContainerConversion() {
    const std::array<int32_t, 4> extensible24In32 {
        1'073'741'824,
        -1'073'741'824,
        2'147'483'392,
        std::numeric_limits<int32_t>::min()
    };
    std::array<int16_t, 4> converted {};
    const clipture::PcmInputLayout extensibleLayout {
        2,
        32,
        24,
        8,
        false
    };
    if (!require(
            clipture::convertInterleavedPcmToS16(
                std::as_bytes(std::span(extensible24In32)),
                2,
                extensibleLayout,
                2,
                converted),
            "24-valid-bit PCM in 32-bit containers should convert")) {
        return false;
    }
    if (!require(
            converted == std::array<int16_t, 4> { 16384, -16384, 32767, -32768 },
            "32-bit container stride must remain intact for 24-bit microphone samples")) {
        return false;
    }

    const std::array<uint8_t, 6> packed24 { 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80 };
    std::array<int16_t, 2> packedConverted {};
    const clipture::PcmInputLayout packedLayout { 1, 24, 24, 3, false };
    if (!require(
            clipture::convertInterleavedPcmToS16(
                std::as_bytes(std::span(packed24)),
                2,
                packedLayout,
                1,
                packedConverted),
            "packed 24-bit PCM should convert")) {
        return false;
    }
    if (!require(
            packedConverted == std::array<int16_t, 2> { 32767, -32768 },
            "packed 24-bit PCM should preserve full-scale polarity")) {
        return false;
    }

    const std::array<float, 2> floating { 0.5f, -0.5f };
    std::array<int16_t, 2> floatConverted {};
    const clipture::PcmInputLayout floatLayout { 1, 32, 32, 4, true };
    if (!require(
            clipture::convertInterleavedPcmToS16(
                std::as_bytes(std::span(floating)),
                2,
                floatLayout,
                1,
                floatConverted),
            "32-bit float PCM should convert")) {
        return false;
    }
    if (!require(
            floatConverted == std::array<int16_t, 2> { 16384, -16384 },
            "float PCM conversion should preserve normalized samples")) {
        return false;
    }

    auto malformedLayout = extensibleLayout;
    malformedLayout.blockAlign = 6;
    return require(
        !clipture::convertInterleavedPcmToS16(
            std::as_bytes(std::span(extensible24In32)),
            2,
            malformedLayout,
            2,
            converted),
        "PCM conversion should reject a block alignment smaller than its containers");
}

bool testLatencyWindowIsBoundedAndRecent() {
    clipture::LatencyWindow<8> window;
    for (int64_t sample = 1; sample <= 8; ++sample) {
        window.record(9 + sample, sample);
    }
    const auto complete = window.snapshot(17, 100);
    if (!require(complete.samples == 8, "latency window should retain its bounded sample set")) return false;
    if (!require(
            complete.average100ns == 4 && complete.p50_100ns == 5 &&
                complete.p95_100ns == 8 && complete.maximum100ns == 8,
            "latency window should calculate deterministic aggregate values")) {
        return false;
    }

    window.record(20, 9);
    const auto recent = window.snapshot(20, 5);
    if (!require(recent.samples == 4, "latency window should exclude samples outside its horizon")) return false;
    if (!require(
            recent.average100ns == 7 && recent.p50_100ns == 8 &&
                recent.p95_100ns == 9 && recent.maximum100ns == 9,
            "latency window should report percentiles from only recent samples")) {
        return false;
    }

    window.clear();
    return require(window.snapshot(20, 100).samples == 0, "clearing latency telemetry should remove samples");
}

bool testRefreshRateSamplerMaintainsTargetCadence() {
    constexpr int targetFps = 60;
    constexpr int durationSeconds = 120;
    constexpr int64_t basePts100ns = 50'000'000'000LL;
    struct SourceRate {
        int numerator;
        int denominator;
    };
    const SourceRate sourceRates[] {
        { 60, 1 },
        { 60'000, 1'001 },
        { 75, 1 },
        { 120, 1 },
        { 1439, 10 },
        { 143855, 1000 },
        { 144, 1 },
        { 165, 1 },
        { 210, 1 },
        { 240, 1 },
    };
    for (const auto sourceRate : sourceRates) {
        clipture::FixedRateFrameSampler sampler;
        int sampled = 0;
        int64_t previousSelectedPts100ns = 0;
        const int64_t sourceFrameCount =
            static_cast<int64_t>(sourceRate.numerator) * durationSeconds / sourceRate.denominator;
        for (int64_t sourceFrame = 0; sourceFrame <= sourceFrameCount; ++sourceFrame) {
            const int64_t pts100ns = basePts100ns +
                sourceFrame * 10'000'000LL * sourceRate.denominator / sourceRate.numerator;
            if (!sampler.shouldSample(pts100ns, targetFps)) continue;
            ++sampled;
            const int64_t selectedOffset100ns = sampler.selectedPts100ns() - basePts100ns;
            const uint64_t targetTick = static_cast<uint64_t>(
                (selectedOffset100ns * targetFps + 9'999'999LL) / 10'000'000LL);
            const int64_t expectedSelectedPts100ns = basePts100ns + static_cast<int64_t>(
                targetTick * 10'000'000ULL / targetFps);
            if (!require(
                    sampler.selectedPts100ns() == expectedSelectedPts100ns &&
                    sampler.selectedPts100ns() > previousSelectedPts100ns &&
                    sampler.selectedPts100ns() <= pts100ns,
                    "selected capture timestamps must stay on one exact target-rate phase")) {
                return false;
            }
            previousSelectedPts100ns = sampler.selectedPts100ns();
        }
        const int64_t lastElapsed100ns =
            sourceFrameCount * 10'000'000LL * sourceRate.denominator / sourceRate.numerator;
        const int64_t targetSamples = lastElapsed100ns * targetFps / 10'000'000LL + 1;
        const int64_t expectedSamples = std::min(sourceFrameCount + 1, targetSamples);
        if (!require(sampled == expectedSamples,
                     "fixed-rate sampler must not alias integer or fractional display rates below 60 FPS")) {
            return false;
        }
    }
    return true;
}

bool testJitteredRefreshSamplerMaintainsCadence() {
    constexpr int targetFps = 60;
    constexpr int64_t basePts100ns = 80'000'000'000LL;
    constexpr int64_t duration100ns = 1'200'000'000LL;
    constexpr std::array<int64_t, 10> sourceIntervals100ns {
        45'000, 49'000, 47'000, 52'000, 43'000,
        61'000, 46'000, 55'000, 44'000, 50'000,
    };
    clipture::FixedRateFrameSampler sampler;
    int64_t pts100ns = basePts100ns;
    uint64_t sourceFrame = 0;
    uint64_t sampled = 0;
    while (pts100ns <= basePts100ns + duration100ns) {
        if (sampler.shouldSample(pts100ns, targetFps)) ++sampled;
        pts100ns += sourceIntervals100ns[sourceFrame % sourceIntervals100ns.size()];
        ++sourceFrame;
    }
    const int64_t covered100ns = pts100ns - sourceIntervals100ns[(sourceFrame - 1) % sourceIntervals100ns.size()] - basePts100ns;
    const uint64_t expected = static_cast<uint64_t>(covered100ns * targetFps / 10'000'000LL + 1);
    return require(sampled == expected,
                   "timestamp-driven sampling must remain phase-locked under high-refresh jitter and VRR-like intervals");
}

bool testCaptureBackendPolicyAndDxgiHelpers() {
    bool valid = false;
    if (!require(
            clipture::parseCaptureBackendPreference("dxgi", valid) == clipture::CaptureBackendPreference::Dxgi && valid,
            "DXGI backend override should parse")) return false;
    if (!require(
            clipture::parseCaptureBackendPreference("invalid", valid) == clipture::CaptureBackendPreference::Auto && !valid,
            "invalid backend override should safely resolve to auto")) return false;

    const auto sdr = clipture::decideCaptureBackend(
        clipture::CaptureBackendPreference::Auto, false, true, false);
    const auto hdr = clipture::decideCaptureBackend(
        clipture::CaptureBackendPreference::Auto, true, true, false);
    const auto rotated = clipture::decideCaptureBackend(
        clipture::CaptureBackendPreference::Auto, false, false, false);
    const auto quarantined = clipture::decideCaptureBackend(
        clipture::CaptureBackendPreference::Auto, false, true, true);
    const auto forcedHdr = clipture::decideCaptureBackend(
        clipture::CaptureBackendPreference::Dxgi, true, true, false);
    if (!require(sdr.kind == clipture::CaptureBackendKind::Dxgi && sdr.supported,
                 "auto SDR capture should prefer Desktop Duplication")) return false;
    if (!require(hdr.kind == clipture::CaptureBackendKind::Dxgi && rotated.kind == clipture::CaptureBackendKind::Wgc,
                 "HDR should use DXGI while rotated capture remains on WGC")) return false;
    if (!require(quarantined.kind == clipture::CaptureBackendKind::Wgc,
                 "a quarantined DXGI output should not oscillate back during the session")) return false;
    if (!require(forcedHdr.supported && forcedHdr.kind == clipture::CaptureBackendKind::Dxgi,
                 "forced DXGI should support the validated FP16 HDR path")) return false;
    if (!require(
            clipture::dxgiCaptureFormatSupported(DXGI_FORMAT_R16G16B16A16_FLOAT, true) &&
            !clipture::dxgiCaptureFormatSupported(DXGI_FORMAT_R10G10B10A2_UNORM, true) &&
            clipture::dxgiCaptureFormatSupported(DXGI_FORMAT_B8G8R8A8_UNORM, false),
            "DXGI must accept only formats with a validated conversion path")) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo {};
    frameInfo.LastPresentTime.QuadPart = 100;
    frameInfo.LastMouseUpdateTime.QuadPart = 120;
    frameInfo.AccumulatedFrames = 4;
    if (!require(clipture::dxgiEffectiveTimestampTicks(frameInfo) == 120,
                 "pointer-only timing should participate in the effective DXGI timestamp")) return false;
    frameInfo.LastPresentTime.QuadPart = 0;
    if (!require(clipture::dxgiEffectiveTimestampTicks(frameInfo) == 120,
                 "a pointer-only update should retain its QPC timestamp")) return false;
    if (!require(clipture::dxgiAccumulatedFramesBeyondFirst(frameInfo.AccumulatedFrames) == 3,
                 "DXGI accumulation accounting should exclude the current acquired frame")) return false;
    if (!require(
            clipture::captureTimestampIsStrictlyNew(100, 101) &&
            !clipture::captureTimestampIsStrictlyNew(100, 100) &&
            !clipture::captureTimestampIsStrictlyNew(100, 99),
            "capture timestamps must never move backward or repeat")) return false;
    if (!require(clipture::detail::qpcTicksTo100ns(10'000, 10'000) == 10'000'000,
                 "QPC ticks should convert exactly to the shared 100 ns clock")) return false;

    int recoveryDelayTotal = 0;
    for (const int delay : clipture::kDxgiRecoveryDelaysMs) recoveryDelayTotal += delay;
    return require(recoveryDelayTotal == 1900,
                   "DXGI recovery must remain bounded before automatic WGC fallback");
}

bool testDesktopPointerDecodingAndClipping() {
    clipture::DecodedDesktopPointerShape decoded;
    std::string error;

    DXGI_OUTDUPL_POINTER_SHAPE_INFO colorInfo {};
    colorInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    colorInfo.Width = 1;
    colorInfo.Height = 1;
    colorInfo.Pitch = 4;
    const std::array<std::byte, 4> colorBytes {
        std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 128 },
    };
    if (!require(clipture::decodeDesktopPointerShape(colorInfo, colorBytes, decoded, error),
                 "ARGB desktop pointer should decode")) return false;
    if (!require(
            decoded.width == 1 && decoded.height == 1 &&
            decoded.rgbaOperationPixels[0] == 3 &&
            decoded.rgbaOperationPixels[1] == 2 &&
            decoded.rgbaOperationPixels[2] == 1 &&
            (decoded.rgbaOperationPixels[3] >> 8) ==
                static_cast<uint16_t>(clipture::DesktopPointerPixelMode::Alpha) &&
            (decoded.rgbaOperationPixels[3] & 0xFF) == 128,
            "color pointer channels and alpha operation should remain exact")) return false;

    DXGI_OUTDUPL_POINTER_SHAPE_INFO maskedInfo {};
    maskedInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    maskedInfo.Width = 2;
    maskedInfo.Height = 1;
    maskedInfo.Pitch = 8;
    const std::array<std::byte, 8> maskedBytes {
        std::byte { 10 }, std::byte { 20 }, std::byte { 30 }, std::byte { 0 },
        std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 255 },
    };
    if (!require(clipture::decodeDesktopPointerShape(maskedInfo, maskedBytes, decoded, error),
                 "masked-color desktop pointer should decode")) return false;
    if (!require(
            (decoded.rgbaOperationPixels[3] >> 8) ==
                static_cast<uint16_t>(clipture::DesktopPointerPixelMode::Replace) &&
            (decoded.rgbaOperationPixels[7] >> 8) ==
                static_cast<uint16_t>(clipture::DesktopPointerPixelMode::Xor),
            "masked-color replace and XOR pixels should remain distinct")) return false;

    DXGI_OUTDUPL_POINTER_SHAPE_INFO monoInfo {};
    monoInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    monoInfo.Width = 2;
    monoInfo.Height = 4;
    monoInfo.Pitch = 1;
    const std::array<std::byte, 4> monoBytes {
        std::byte { 0x80 }, std::byte { 0x40 },
        std::byte { 0x40 }, std::byte { 0x80 },
    };
    if (!require(clipture::decodeDesktopPointerShape(monoInfo, monoBytes, decoded, error),
                 "monochrome AND/XOR desktop pointer should decode")) return false;
    if (!require(decoded.width == 2 && decoded.height == 2,
                 "monochrome pointer height should exclude the second XOR mask plane")) return false;

    const auto clipped = clipture::clipDesktopPointer(POINT { -3, -2 }, 10, 8, 100, 100);
    if (!require(
            clipped && clipped.destinationX == 0 && clipped.destinationY == 0 &&
            clipped.sourceX == 3 && clipped.sourceY == 2 &&
            clipped.width == 7 && clipped.height == 6,
            "partially offscreen desktop pointers should preserve source offsets")) return false;
    const auto outside = clipture::clipDesktopPointer(POINT { 120, 120 }, 10, 10, 100, 100);
    if (!require(!outside, "fully offscreen desktop pointers should not render")) return false;

    colorInfo.Pitch = 3;
    return require(!clipture::decodeDesktopPointerShape(colorInfo, colorBytes, decoded, error),
                   "malformed pointer pitch should fail without reading outside the shape buffer");
}

bool testVideoTimelineCatchesUpWithoutUnboundedBursts() {
    constexpr int fps = 60;
    constexpr int64_t firstPts100ns = 50'000'000'000LL;
    const int64_t spacing100ns = 10'000'000LL / fps;
    clipture::VideoTimeline timeline(firstPts100ns, fps);
    const auto initial = timeline.advance(0);
    if (!require(initial.pts100ns == firstPts100ns && initial.dueTicks == 1 && initial.skippedTicks == 0,
                 "timeline should emit its first current tick once")) return false;

    const auto transientStall = timeline.advance(spacing100ns * 5);
    if (!require(transientStall.dueTicks == 6 && transientStall.skippedTicks == 0,
                 "transient scheduler stalls should preserve every output tick")) return false;
    if (!require(transientStall.pts100ns == firstPts100ns + spacing100ns,
                 "catch-up should begin at the next uncommitted timestamp")) return false;

    const auto longStall = timeline.advance(spacing100ns * 20, 8);
    if (!require(longStall.dueTicks == 8 && longStall.skippedTicks == 13,
                 "long suspension recovery must remain bounded")) return false;
    if (!require(clipture::finalVideoSampleDuration100ns(spacing100ns, fps) == spacing100ns,
                 "final video duration should use the packet duration after an earlier gap")) return false;
    return require(clipture::finalVideoSampleDuration100ns(0, fps) == spacing100ns,
                   "final video duration should use one frame as its fallback");
}

bool testCfrFrameRunsCompactRepeatedTicks() {
    clipture::CfrFrameRun run {
        42,
        7,
        1'000'000,
        166'666,
        1,
        true,
    };
    if (!require(
            clipture::cfrRunCanAppend(run, 42, 7, 1'166'666, 166'666),
            "a contiguous repeated tick should append to its source-frame run")) return false;
    ++run.tickCount;
    if (!require(
            clipture::cfrFreshTickCount(run) == 1 && clipture::cfrRepeatTickCount(run) == 1,
            "a run should account for its fresh tick separately from repeats")) return false;
    if (!require(
            !clipture::cfrRunCanAppend(run, 43, 7, 1'333'332, 166'666) &&
                !clipture::cfrRunCanAppend(run, 42, 8, 1'333'332, 166'666) &&
                !clipture::cfrRunCanAppend(run, 42, 7, 1'499'998, 166'666),
            "source, epoch, and timestamp discontinuities should begin a new run")) return false;
    if (!require(
            clipture::shouldScheduleCfrTick(42, 42, true) &&
                !clipture::shouldScheduleCfrTick(42, 42, false) &&
                clipture::shouldScheduleCfrTick(43, 42, false),
            "the developer switch should disable only repeated source ticks")) return false;
    clipture::CfrFrameRun repeatedOnly { 42, 7, 2'000'000, 166'666, 12, false };
    return require(
        clipture::cfrFreshTickCount(repeatedOnly) == 0 &&
            clipture::cfrRepeatTickCount(repeatedOnly) == 12,
        "a continuation run should remain outside fresh-frame pressure accounting");
}

bool testAudioTimelineNeverRewinds() {
    bool anchored = false;
    int64_t nextPts100ns = 10'000'000;
    clipture::alignAudioPtsForwardOnly(9'500'000, 100'000, anchored, nextPts100ns);
    if (!require(anchored && nextPts100ns == 10'000'000,
                 "audio clock must not rewind over committed fallback samples")) return false;

    clipture::alignAudioPtsForwardOnly(10'150'000, 100'000, anchored, nextPts100ns);
    if (!require(nextPts100ns == 10'000'000,
                 "small audio clock jitter should not create a gap")) return false;

    clipture::alignAudioPtsForwardOnly(10'300'000, 100'000, anchored, nextPts100ns);
    return require(nextPts100ns == 10'300'000,
                   "a real forward audio discontinuity should remain visible on the timeline");
}

bool testFrameQueueDropAccounting() {
    clipture::FrameQueue queue(3);
    for (uint64_t sequence = 1; sequence <= 5; ++sequence) {
        clipture::CapturedFrame frame;
        frame.sequence = sequence;
        frame.pts100ns = static_cast<int64_t>(sequence) * 100;
        queue.push(std::move(frame));
    }

    const auto afterPush = queue.stats();
    if (!require(afterPush.overflowDrops == 2 && afterPush.maximumDepth == 3,
                 "frame queue should count capacity overflow")) return false;
    const auto latest = queue.consumeAllAndGetLatest();
    if (!require(latest && latest->sequence == 5,
                 "frame queue should preserve the newest frame")) return false;
    const auto afterConsume = queue.stats();
    if (!require(afterConsume.coalescedDrops == 2 && queue.droppedFrames() == 4,
                 "frame queue should count latest-frame coalescing")) return false;

    clipture::FrameQueue jitterQueue(4);
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        clipture::CapturedFrame frame;
        frame.sequence = sequence;
        frame.pts100ns = static_cast<int64_t>(sequence) * 100;
        jitterQueue.push(std::move(frame));
    }
    const auto selected = jitterQueue.consumeLatestAtOrBefore(250);
    return require(selected && selected->sequence == 2 && jitterQueue.size() == 1,
                   "jitter selection should leave future frames queued");
}

bool testImmutableAudioRouting() {
    clipture::EncodedPacket helper;
    helper.sourceId = "app:helper.exe";
    helper.sampleRate = 48000;
    helper.channelCount = 2;
    helper.bitsPerSample = 16;
    helper.payload = std::make_shared<clipture::PacketPayload>(4 * sizeof(int16_t));
    const int16_t audibleSamples[] = { 0, 0, 12, -12 };
    std::memcpy(helper.payload->data(), audibleSamples, sizeof(audibleSamples));

    const std::map<std::string, std::string> firstRoutes {
        { "app:helper.exe", "system-loopback-pcm" }
    };
    clipture::prepareAudioReplayPacket(helper, firstRoutes);
    if (!require(helper.logicalTrackId == "system-loopback-pcm", "foreground helper should use its system alias")) return false;
    if (!require(helper.audible && helper.audioFrameCount == 2, "PCM metadata should record audibility and frame count")) return false;

    auto later = helper;
    later.logicalTrackId.clear();
    const std::map<std::string, std::string> changedRoutes {
        { "app:helper.exe", "app:helper.exe" }
    };
    clipture::prepareAudioReplayPacket(later, changedRoutes);
    if (!require(helper.logicalTrackId == "system-loopback-pcm", "historical routing must remain unchanged")) return false;
    if (!require(later.logicalTrackId == "app:helper.exe", "new packets should use the new route")) return false;

    std::fill(later.payload->begin(), later.payload->end(), std::byte { 0 });
    clipture::prepareAudioReplayPacket(later, changedRoutes);
    return require(!later.audible, "silent PCM should be marked for track omission");
}

bool testPidAudioProcessSpecsAndTreeCollapse() {
    const auto gameSpec = clipture::makePidAudioProcessSpec(
        clipture::AudioProcessKind::Game,
        101,
        "Game-Win64-Shipping.exe");
    const auto parsedGame = clipture::parseAudioProcessSpec(gameSpec);
    if (!require(parsedGame.kind == clipture::AudioProcessKind::Game, "game PID source should preserve its kind")) return false;
    if (!require(parsedGame.pidSpecific && parsedGame.processId == 101, "game PID source should preserve its process ID")) return false;
    if (!require(parsedGame.processName == "Game-Win64-Shipping.exe", "game PID source should preserve its executable")) return false;
    if (!require(
            clipture::audioProcessSourceId(gameSpec) == "game:Game-Win64-Shipping.exe",
            "game PID packets should use the stable game source ID")) {
        return false;
    }

    const std::vector<clipture::RunningProcessInfo> processes {
        { 100, 10, "Launcher.exe", "launcher.exe" },
        { 101, 100, "Game.exe", "game.exe" },
        { 102, 101, "AudioHelper.exe", "audiohelper.exe" },
        { 200, 10, "VoiceHelper.exe", "voicehelper.exe" }
    };
    const auto roots = clipture::collapseProcessTreeRoots(processes, { 101, 200, 100, 102 });
    if (!require(roots.size() == 2, "overlapping game process trees should collapse to one root")) return false;
    if (!require(std::find(roots.begin(), roots.end(), DWORD { 100 }) != roots.end(), "the highest game ancestor should own its tree")) return false;
    return require(
        std::find(roots.begin(), roots.end(), DWORD { 200 }) != roots.end(),
        "an independent audio helper should remain a separate capture root");
}

bool testPreferredProcessAudioTreeRoot() {
    const std::vector<clipture::RunningProcessInfo> browserProcesses {
        { 10, 0, "explorer.exe", "explorer.exe" },
        { 100, 10, "brave.exe", "brave.exe" },
        { 101, 100, "brave.exe", "brave.exe" },
        { 102, 100, "utility.exe", "utility.exe" },
        { 103, 102, "brave.exe", "brave.exe" },
        { 200, 10, "brave.exe", "brave.exe" },
        { 201, 200, "brave.exe", "brave.exe" }
    };
    if (!require(
            clipture::preferredProcessTreeRootForName(browserProcesses, "app:BRAVE.EXE") == 100,
            "app capture should bind the browser root covering the largest same-name process tree")) {
        return false;
    }

    const std::vector<clipture::RunningProcessInfo> tiedProcesses {
        { 400, 10, "brave.exe", "brave.exe" },
        { 300, 10, "brave.exe", "brave.exe" }
    };
    if (!require(
            clipture::preferredProcessTreeRootForName(tiedProcesses, "brave.exe") == 300,
            "equally sized app process trees should use a deterministic PID tie-break")) {
        return false;
    }
    return require(
        clipture::preferredProcessTreeRootForName(browserProcesses, "missing.exe") == 0,
        "missing configured apps should remain unbound until a later retry");
}

clipture::EncodedPacket makePcmPacket(
    clipture::PacketRingBuffer& pool,
    std::string sourceId,
    int64_t pts100ns,
    int16_t value) {
    constexpr int sampleRate = 48000;
    constexpr int channels = 2;
    constexpr int frames = sampleRate / 100;
    clipture::EncodedPacket packet;
    packet.kind = clipture::PacketKind::Audio;
    packet.codec = clipture::PacketCodec::PcmS16;
    packet.pts100ns = pts100ns;
    packet.dts100ns = pts100ns;
    packet.duration100ns = 100'000;
    packet.sourceId = std::move(sourceId);
    packet.encoderId = "PCM_S16";
    packet.sampleRate = sampleRate;
    packet.channelCount = channels;
    packet.bitsPerSample = 16;
    packet.payload = pool.acquirePayload(frames * channels * sizeof(int16_t));
    auto* samples = reinterpret_cast<int16_t*>(packet.payload->data());
    std::fill(samples, samples + frames * channels, value);
    return packet;
}

bool testLiveAacCoordinator() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({
        { "app:ui.exe", "system-loopback-pcm" },
        { "app:audio.exe", "system-loopback-pcm" }
    });
    coordinator.start();

    constexpr int64_t startPts = 10'000'000'000LL;
    for (int block = 0; block < 50; ++block) {
        const int64_t pts = startPts + static_cast<int64_t>(block) * 100'000LL;
        coordinator.publish(makePcmPacket(raw, "app:ui.exe", pts, 700));
        coordinator.publish(makePcmPacket(raw, "app:audio.exe", pts + 20'000LL, 500));
    }
    coordinator.updateRouting({ { "app:ui.exe", "app:ui.exe" } });
    for (int block = 50; block < 80; ++block) {
        const int64_t pts = startPts + static_cast<int64_t>(block) * 100'000LL;
        coordinator.publish(makePcmPacket(raw, "app:ui.exe", pts, 900));
    }
    coordinator.stop();

    const auto encoded = aac.snapshot();
    if (!require(!encoded.empty(), "live coordinator should produce AAC packets")) return false;
    bool sawSystemTrack = false;
    bool sawAppTrack = false;
    for (const auto& packet : encoded) {
        if (!require(packet.codec == clipture::PacketCodec::AacLc && packet.audioFrameCount > 0,
                     "coordinator output should carry AAC metadata")) return false;
        sawSystemTrack = sawSystemTrack || packet.logicalTrackId == "system-loopback-pcm";
        sawAppTrack = sawAppTrack || packet.logicalTrackId == "app:ui.exe";
    }
    if (!require(sawSystemTrack && sawAppTrack, "route changes should create distinct historical AAC tracks")) return false;

    clipture::PacketRingBuffer silentRaw(10LL * 10'000'000LL);
    clipture::PacketRingBuffer silentAac(10LL * 10'000'000LL);
    clipture::AudioReplayCoordinator silentCoordinator(silentRaw, silentAac);
    silentCoordinator.updateRouting({ { "microphone-pcm", "microphone-pcm" } });
    silentCoordinator.start();
    for (int block = 0; block < 30; ++block) {
        silentCoordinator.publish(makePcmPacket(
            silentRaw,
            "microphone-pcm",
            startPts + static_cast<int64_t>(block) * 100'000LL,
            0));
    }
    silentCoordinator.stop();
    return require(silentAac.size() == 0, "silent logical tracks should not emit AAC samples");
}

bool testLiveAacTimelineStaysContinuousThroughSilence() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({ { "microphone-pcm", "microphone-pcm" } });
    coordinator.start();

    constexpr int64_t startPts = 15'000'000'000LL;
    for (int block = 0; block < 200; ++block) {
        const bool quiet = block >= 30 && block < 170;
        coordinator.publish(makePcmPacket(
            raw,
            "microphone-pcm",
            startPts + static_cast<int64_t>(block) * 100'000LL,
            quiet ? 0 : 800));
    }
    coordinator.stop();

    const auto encoded = aac.snapshot();
    if (!require(encoded.size() >= 80, "AAC should continue through a long quiet interval after activation")) {
        return false;
    }
    const uint32_t epoch = encoded.front().encoderEpoch;
    int64_t previousPts = encoded.front().pts100ns;
    for (std::size_t index = 1; index < encoded.size(); ++index) {
        const auto& packet = encoded[index];
        if (!require(packet.encoderEpoch == epoch, "silence must not split a live track into AAC epochs")) {
            return false;
        }
        const int64_t expectedDuration =
            (static_cast<int64_t>(encoded[index - 1].audioFrameCount) * 10'000'000LL) /
            std::max(1, encoded[index - 1].sampleRate);
        const int64_t gap = packet.pts100ns - previousPts;
        if (!require(
                std::abs(gap - expectedDuration) <= 50'000LL,
                "live AAC timestamps should stay contiguous through silence")) {
            return false;
        }
        previousPts = packet.pts100ns;
    }
    return true;
}

bool testConcurrentPublishDoesNotTriggerRepair() {
    clipture::PacketRingBuffer raw(60LL * 10'000'000LL);
    clipture::PacketRingBuffer aac(60LL * 10'000'000LL);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({
        { "app:one.exe", "system-loopback-pcm" },
        { "app:two.exe", "system-loopback-pcm" },
        { "app:three.exe", "system-loopback-pcm" },
        { "app:four.exe", "system-loopback-pcm" }
    });
    coordinator.start();

    constexpr int64_t startPts = 20'000'000'000LL;
    const std::vector<std::string> sources {
        "app:one.exe", "app:two.exe", "app:three.exe", "app:four.exe"
    };
    std::vector<std::thread> publishers;
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        publishers.emplace_back([&, sourceIndex] {
            for (int block = 0; block < 200; ++block) {
                coordinator.publish(makePcmPacket(
                    raw,
                    sources[sourceIndex],
                    startPts + static_cast<int64_t>(block) * 100'000LL,
                    static_cast<int16_t>(100 + sourceIndex)));
            }
        });
    }
    for (auto& publisher : publishers) publisher.join();
    coordinator.stop();

    const auto stats = coordinator.stats();
    if (!require(stats.queueOverflows == 0, "ordinary publisher contention must not count as queue overflow")) {
        return false;
    }
    return require(stats.encoderRestarts == 0, "ordinary publisher contention must not trigger AAC repair");
}

std::filesystem::path uniqueReplayTestRoot(const char* suffix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("clipture-replay-") + suffix + "-" + std::to_string(ticks));
}

bool testReplaySegmentStorePersistsAndRetainsSnapshots() {
    const auto root = uniqueReplayTestRoot("persist");
    clipture::ReplaySegmentStoreOptions options;
    options.streamName = "video-test";
    options.rootDirectory = root;
    options.retention100ns = 200;
    options.targetSegmentDuration100ns = 100;
    options.targetSegmentBytes = 32;
    options.maximumWriteBytes = 7;
    options.deleteOnClose = true;

    clipture::PacketRingBuffer pool;
    clipture::ReplaySegmentStore store(options);
    store.start();
    auto makePacket = [&](int64_t pts100ns, uint8_t seed) {
        clipture::EncodedPacket packet;
        packet.kind = clipture::PacketKind::Video;
        packet.codec = clipture::PacketCodec::H264AnnexB;
        packet.pts100ns = pts100ns;
        packet.duration100ns = 100;
        packet.keyframe = true;
        packet.payload = pool.acquirePayload(37);
        for (std::size_t index = 0; index < packet.payload->size(); ++index) {
            (*packet.payload)[index] = static_cast<std::byte>(seed + static_cast<uint8_t>(index));
        }
        return packet;
    };

    store.push(makePacket(1'000, 1));
    store.push(makePacket(1'100, 2));
    store.push(makePacket(1'300, 3));
    if (!require(store.waitUntilIdle(std::chrono::seconds(3)), "replay archive should drain promptly")) {
        store.stop();
        return false;
    }

    auto snapshot = store.snapshot();
    const auto stats = store.stats();
    if (!require(snapshot.size() == 2 && snapshot.front().pts100ns == 1'100,
                 "replay retention should remove packets older than the configured window")) {
        store.stop();
        return false;
    }
    if (!require(
            !snapshot.front().payload && snapshot.front().payloadReader && stats.ramFallbackBytes == 0,
            "persisted archive entries should release their private RAM payload reference")) {
        store.stop();
        return false;
    }
    if (!require(stats.persistedPackets >= 2 && stats.maximumWriteBytes <= 7,
                 "archive writes should persist every retained packet within the hard request cap")) {
        store.stop();
        return false;
    }

    std::vector<std::byte> restored;
    if (!require(
            clipture::copyPayloadRange(snapshot.front(), 0, clipture::payloadSize(snapshot.front()), restored),
            "disk-backed packet payload should remain readable")) {
        store.stop();
        return false;
    }
    if (!require(restored.size() == 37 && restored.front() == std::byte { 2 } && restored.back() == std::byte { 38 },
                 "disk-backed payload bytes should round-trip exactly")) {
        store.stop();
        return false;
    }

    store.stop();
    restored.clear();
    if (!require(
            clipture::copyPayloadRange(snapshot.back(), 0, clipture::payloadSize(snapshot.back()), restored),
            "an active save snapshot should keep archived segments alive after store shutdown")) {
        return false;
    }
    snapshot.clear();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return true;
}

bool testReplaySegmentStoreFallsBackToRam() {
    const auto root = uniqueReplayTestRoot("fallback");
    std::error_code ignored;
    std::filesystem::create_directories(root.parent_path(), ignored);
    {
        std::ofstream blocker(root, std::ios::binary);
        blocker << "not a directory";
    }

    clipture::ReplaySegmentStoreOptions options;
    options.streamName = "failure-test";
    options.rootDirectory = root;
    options.maximumWriteBytes = 8;
    clipture::PacketRingBuffer pool;
    clipture::ReplaySegmentStore store(options);
    store.start();

    clipture::EncodedPacket packet;
    packet.kind = clipture::PacketKind::Video;
    packet.pts100ns = 1'000;
    packet.payload = pool.acquirePayload(16);
    std::fill(packet.payload->begin(), packet.payload->end(), std::byte { 0x5A });
    store.push(packet);

    for (int attempt = 0; attempt < 30 && store.stats().writeFailures == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto stats = store.stats();
    const auto snapshot = store.snapshot();
    const bool preserved = require(
        stats.writeFailures > 0 && stats.ramFallbackBytes == 16 &&
            snapshot.size() == 1 && snapshot.front().payload && !snapshot.front().payloadReader,
        "an unavailable archive path should preserve the packet in RAM without dropping it");
    store.stop();
    std::filesystem::remove(root, ignored);
    return preserved;
}

bool testReplaySegmentStoreSpillsOldestBeyondRamBudget() {
    const auto root = uniqueReplayTestRoot("hybrid");
    clipture::ReplaySegmentStoreOptions options;
    options.streamName = "hybrid-video";
    options.rootDirectory = root;
    options.retention100ns = 10'000;
    options.targetSegmentBytes = 1024;
    options.maximumWriteBytes = 11;
    options.residentPayloadBudgetBytes = 74;

    clipture::PacketRingBuffer pool;
    clipture::ReplaySegmentStore store(options);
    store.start();
    auto makePacket = [&](int64_t pts100ns, uint8_t seed) {
        clipture::EncodedPacket packet;
        packet.kind = clipture::PacketKind::Video;
        packet.pts100ns = pts100ns;
        packet.duration100ns = 100;
        packet.payload = pool.acquirePayload(37);
        std::fill(packet.payload->begin(), packet.payload->end(), static_cast<std::byte>(seed));
        return packet;
    };

    store.push(makePacket(1'000, 1));
    store.push(makePacket(1'100, 2));
    store.push(makePacket(1'200, 3));
    if (!require(store.waitUntilIdle(std::chrono::seconds(3)),
                 "hybrid replay spill should drain promptly")) {
        store.stop();
        return false;
    }

    const auto snapshot = store.snapshot();
    const auto stats = store.stats();
    const bool layoutValid = require(
        snapshot.size() == 3 && !snapshot[0].payload && snapshot[0].payloadReader &&
            snapshot[1].payload && !snapshot[1].payloadReader &&
            snapshot[2].payload && !snapshot[2].payloadReader,
        "hybrid replay should spill only the oldest payload beyond its RAM budget");
    const bool statsValid = require(
        stats.healthy && stats.residentPayloadBytes == 74 &&
            stats.residentPayloadBudgetBytes == 74 && stats.residentPackets == 2 &&
            stats.diskBackedPackets == 1 && stats.ramFallbackBytes == 0,
        "hybrid replay diagnostics should distinguish healthy RAM cache from disk spill");

    std::vector<std::byte> restored;
    const bool payloadValid = require(
        clipture::copyPayloadRange(snapshot[0], 0, clipture::payloadSize(snapshot[0]), restored) &&
            restored.size() == 37 && restored.front() == std::byte { 1 },
        "a hybrid-spilled payload should round-trip exactly");
    store.stop();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return layoutValid && statsValid && payloadValid;
}

bool testMp4MuxerStreamsDiskBackedVideo() {
    const auto root = uniqueReplayTestRoot("mux");
    clipture::ReplaySegmentStoreOptions options;
    options.streamName = "mux-video";
    options.rootDirectory = root / "archive";
    options.maximumWriteBytes = 5;
    options.targetSegmentBytes = 64;
    options.targetSegmentDuration100ns = 10'000'000LL;

    auto packet = packetFromBytes({
        0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28,
        0, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80,
        0, 0, 0, 1, 0x65, 0xAA, 0xBB, 0xCC
    });
    packet.pts100ns = 100'000'000LL;
    packet.dts100ns = packet.pts100ns;
    packet.duration100ns = 166'667LL;
    packet.encodedWidth = 1920;
    packet.encodedHeight = 1080;
    if (!require(clipture::analyzeH264Packet(packet), "mux test packet should have valid Annex B layout")) {
        return false;
    }

    clipture::ReplaySegmentStore store(options);
    store.start();
    store.push(packet);
    if (!require(store.waitUntilIdle(std::chrono::seconds(3)), "mux archive should persist its video packet")) {
        store.stop();
        return false;
    }
    const auto archived = store.snapshot();
    if (!require(archived.size() == 1 && !archived.front().payload && archived.front().payloadReader,
                 "mux integration should consume a genuinely disk-backed packet")) {
        store.stop();
        return false;
    }

    clipture::MuxWritePacing pacing;
    pacing.presentationStartPts100ns = packet.pts100ns;
    pacing.presentationEndPts100ns = packet.pts100ns + packet.duration100ns;
    const auto mux = clipture::muxH264ToMp4(
        archived,
        (root / "output").string(),
        1920,
        1080,
        60,
        50,
        std::move(pacing));
    store.stop();
    if (!require(mux.ok, "MP4 muxer should stream H.264 payload ranges from the replay archive")) {
        std::cerr << "Mux error: " << mux.message << '\n';
        return false;
    }

    std::ifstream input(mux.filePath, std::ios::binary);
    std::vector<char> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const std::array<char, 4> editType { 'e', 'l', 's', 't' };
    const bool hasEditList = std::search(bytes.begin(), bytes.end(), editType.begin(), editType.end()) != bytes.end();
    const bool valid = require(bytes.size() > 100 && hasEditList,
                               "disk-backed mux output should contain MP4 metadata and an exact-range edit list");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return valid;
}

}  // namespace

int main() {
    if (!testStartCodesAndFlags()) return 1;
    if (!testMalformedPackets()) return 1;
    if (!testBoundedWrites()) return 1;
    if (!testAdaptiveWriteRateController()) return 1;
    if (!testPcmContainerConversion()) return 1;
    if (!testLatencyWindowIsBoundedAndRecent()) return 1;
    if (!testRefreshRateSamplerMaintainsTargetCadence()) return 1;
    if (!testJitteredRefreshSamplerMaintainsCadence()) return 1;
    if (!testCaptureBackendPolicyAndDxgiHelpers()) return 1;
    if (!testDesktopPointerDecodingAndClipping()) return 1;
    if (!testVideoTimelineCatchesUpWithoutUnboundedBursts()) return 1;
    if (!testCfrFrameRunsCompactRepeatedTicks()) return 1;
    if (!testAudioTimelineNeverRewinds()) return 1;
    if (!testFrameQueueDropAccounting()) return 1;
    if (!testImmutableAudioRouting()) return 1;
    if (!testPidAudioProcessSpecsAndTreeCollapse()) return 1;
    if (!testPreferredProcessAudioTreeRoot()) return 1;
    if (!testLiveAacCoordinator()) return 1;
    if (!testLiveAacTimelineStaysContinuousThroughSilence()) return 1;
    if (!testConcurrentPublishDoesNotTriggerRepair()) return 1;
    if (!testReplaySegmentStorePersistsAndRetainsSnapshots()) return 1;
    if (!testReplaySegmentStoreFallsBackToRam()) return 1;
    if (!testReplaySegmentStoreSpillsOldestBeyondRamBudget()) return 1;
    if (!testMp4MuxerStreamsDiskBackedVideo()) return 1;
    std::cout << "Packet architecture tests passed.\n";
    return 0;
}

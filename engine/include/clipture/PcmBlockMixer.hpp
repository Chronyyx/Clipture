#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace clipture {

// Add one packet to an interleaved PCM mix block. Packet timestamps and the
// block origin share the media clock; output remains in signed-16 amplitude.
inline void addPcmToMixBlock(std::span<const int16_t> input, int channels,
                             int sampleRate, int64_t relativePts100ns,
                             std::span<float> output) {
    if (channels <= 0 || sampleRate <= 0) return;
    const int64_t inputFrames = static_cast<int64_t>(input.size() / channels);
    const int64_t outputFrames = static_cast<int64_t>(output.size() / channels);
    // Reject non-overlapping packets before multiplying the relative time.
    if (relativePts100ns > (outputFrames * 10'000'000LL) / sampleRate + 1 ||
        relativePts100ns < -(inputFrames * 10'000'000LL) / sampleRate - 1) return;
    // Quantize to the nearest frame on one consistent grid. Exact half-frame
    // ties go toward the later frame, even for packets preceding the block.
    // Truncation toward zero maps -479.995 frames to -479 but +0.005 to 0:
    // both packets then contribute to frame zero. This can pump the limiter
    // at the 100 Hz block cadence after a fractional clock correction.
    const int64_t rounded = relativePts100ns * sampleRate + 5'000'000LL;
    const int64_t relativeStart = rounded / 10'000'000LL -
        (rounded < 0 && rounded % 10'000'000LL != 0 ? 1 : 0);
    const int64_t outputStart = std::max<int64_t>(0, relativeStart);
    const int64_t inputStart = std::max<int64_t>(0, -relativeStart);
    const int64_t count = std::min(outputFrames - outputStart, inputFrames - inputStart);
    for (int64_t frame = 0; frame < count; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            output[(outputStart + frame) * channels + channel] +=
                input[(inputStart + frame) * channels + channel];
        }
    }
}

} // namespace clipture
